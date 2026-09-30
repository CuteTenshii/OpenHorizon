#include "Navigation.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <queue>
#include <string_view>
#include <unordered_map>

namespace openhorizon::ui {

namespace {

/// The grid's origin offset and cell size (0x83398c34, 0x83398c38), its
/// cells per side (0x82d9b0c8), and the band along a cell's edges whose
/// nodes also go in the next cell (the cell size times 0.2 at 0x82000d68,
/// computed at 0x8328d450).
constexpr float kGridOffset = 5000.0F;
constexpr float kCellSize = 400.0F;
constexpr float kCellMargin = kCellSize * 0.2F;
constexpr int kGridCells = 25;

/// Nodes on each side of the current one along a way that the tracking
/// looks at (0x82d9cc64: from position - 10 up to, not including,
/// position + 10).
constexpr int kSearchBefore = 10;
constexpr int kSearchAfter = 10;
/// How many junctions deep the tracking follows (0x82d9f708 passes 1).
constexpr int kSearchDepth = 1;
/// Squared metres from the found node beyond which the tracking gives up
/// and looks the node up afresh (0x82000d00); the route planner compares
/// the car's distance from its last place on the route with the same
/// constant (0x82a54c94) before searching again.
constexpr float kLostSquaredDistance = 10000.0F;
/// Below this height the car is off the network (0x82246a9c).
constexpr float kLowestHeight = -1000.0F;

constexpr const char* kRegistryPath = "media/gametunablesettings/globalregistry.xml";
constexpr const char* kSatNavSpeeds = "Navigation/RoutingHeuristicSpeeds/SatNav/";

/// Miles per hour to metres per second (the factor at 0x82005aec that
/// 0x82da2800 applies when asked for metres per second).
constexpr float kMetresPerSecondPerMph = 0.44704F;
/// The speed of a shortcut way while shortcuts are refused (0x82000ccc,
/// loaded at 0x82da1b84).
constexpr float kClosedShortcutSpeed = 0.01F;

constexpr std::string_view kOneWay = "oneway";
constexpr std::string_view kRoadType = "road_type";
constexpr std::string_view kShortcut = "shortcut";
constexpr std::string_view kNoRoad = "none";

/// A grid index wrapped into 0 to 24 (0x82d9b0c8, 0x82d9ec68).
int wrap(int index)
{
    if (index < 0) {
        index += (static_cast<int>(static_cast<unsigned>(-1 - index) / kGridCells) + 1) * kGridCells;
    }
    return index >= kGridCells ? index % kGridCells : index;
}

/// The unwrapped grid index of a coordinate, truncated toward zero as
/// fctiwz does.
int gridIndex(float coordinate)
{
    return static_cast<int>((coordinate + kGridOffset) * (1.0F / kCellSize));
}

/// A cell's place in the grid, rows along z (0x82d9d0b0).
std::size_t cellIndex(int row, int column)
{
    return static_cast<std::size_t>(row) * kGridCells + static_cast<std::size_t>(column);
}

float squaredDistance(const glm::vec3& a, const glm::vec3& b)
{
    const glm::vec3 d = a - b;
    return glm::dot(d, d);
}

/// Whether a way's road_type is refused by `filter`: 0x82d9c0b8 for
/// NoShortcut (verified); the other two by the same pattern (inferred).
bool refusedType(std::string_view type, NavFilter filter)
{
    switch (filter) {
    case NavFilter::NoShortcutOrNone:
        return type == kShortcut || type == kNoRoad;
    case NavFilter::NoNone:
        return type == kNoRoad;
    case NavFilter::NoShortcut:
        return type == kShortcut;
    case NavFilter::Any:
        return false;
    }
    return false;
}

} // namespace

NavLocator::NavLocator(const NavFile& nav)
    : m_nav(nav)
    , m_cells(static_cast<std::size_t>(kGridCells * kGridCells))
{
    for (int row = 0; row < kGridCells; ++row) {
        for (int column = 0; column < kGridCells; ++column) {
            // 0x82d9b128: the centre of each cell, at height 0.
            m_cells[cellIndex(row, column)].centre
                = glm::vec3((static_cast<float>(column) + 0.5F) * kCellSize - kGridOffset, 0.0F,
                    (static_cast<float>(row) + 0.5F) * kCellSize - kGridOffset);
        }
    }
    for (std::uint32_t node = 0; node < nav.nodes.size(); ++node) {
        const std::vector<NavLink>& links = nav.nodes[node].links;
        const bool onRoad = std::any_of(links.begin(), links.end(),
            [&](const NavLink& link) { return !nav.ways[link.way].tag(kRoadType).empty(); });
        if (!onRoad) {
            continue;
        }
        m_empty = false;
        const glm::vec3& at = nav.nodes[node].position;
        const int column = gridIndex(at.x);
        const int row = gridIndex(at.z);
        const float acrossX = at.x + kGridOffset - static_cast<float>(column) * kCellSize;
        const float acrossZ = at.z + kGridOffset - static_cast<float>(row) * kCellSize;
        insert(node, column, row);
        const bool west = acrossX < kCellMargin;
        const bool east = acrossX > kCellSize - kCellMargin;
        if (west) {
            insert(node, column - 1, row);
        }
        if (east) {
            insert(node, column + 1, row);
        }
        for (const int nextRow :
            {acrossZ < kCellMargin ? row - 1 : row, acrossZ > kCellSize - kCellMargin ? row + 1 : row}) {
            if (nextRow == row) {
                continue;
            }
            insert(node, column, nextRow);
            if (east) {
                insert(node, column + 1, nextRow);
            }
            if (west) {
                insert(node, column - 1, nextRow);
            }
        }
    }
}

void NavLocator::insert(std::uint32_t node, int column, int row)
{
    m_cells[cellIndex(wrap(row), wrap(column))].nodes.push_back(node);
}

bool NavLocator::acceptsWay(std::uint32_t way, NavFilter filter) const
{
    return !refusedType(m_nav.ways[way].tag(kRoadType), filter);
}

bool NavLocator::acceptsNode(std::uint32_t node, NavFilter filter) const
{
    // 0x82d9c128: a node is accepted when one of its ways is.
    if (filter == NavFilter::Any) {
        return true;
    }
    const std::vector<NavLink>& links = m_nav.nodes[node].links;
    return std::any_of(links.begin(), links.end(), [&](const NavLink& link) { return acceptsWay(link.way, filter); });
}

std::optional<std::uint32_t> NavLocator::search(
    const Cell& cell, const glm::vec3& position, NavFilter filter, bool groundOnly) const
{
    // 0x82d9cf80.
    std::optional<std::uint32_t> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (const std::uint32_t node : cell.nodes) {
        glm::vec3 at = m_nav.nodes[node].position;
        if (groundOnly) {
            at.y = position.y;
        }
        const float d = squaredDistance(at, position);
        if (d < bestDistance && acceptsNode(node, filter)) {
            best = node;
            bestDistance = d;
        }
    }
    return best;
}

std::optional<std::uint32_t> NavLocator::find(const glm::vec3& position, NavFilter filter, bool groundOnly) const
{
    // 0x82d9d070.
    if (m_empty) {
        return std::nullopt;
    }
    const Cell& own = m_cells[cellIndex(wrap(gridIndex(position.z)), wrap(gridIndex(position.x)))];
    if (const std::optional<std::uint32_t> found = search(own, position, filter, groundOnly)) {
        return found;
    }
    const Cell* closest = nullptr;
    float closestDistance = std::numeric_limits<float>::max();
    for (const Cell& cell : m_cells) {
        if (cell.nodes.empty()) {
            continue;
        }
        glm::vec3 centre = cell.centre;
        if (groundOnly) {
            centre.y = position.y;
        }
        const float d = squaredDistance(centre, position);
        if (d < closestDistance) {
            closest = &cell;
            closestDistance = d;
        }
    }
    return closest ? search(*closest, position, filter, groundOnly) : std::nullopt;
}

std::optional<std::uint32_t> NavLocator::nearest(const glm::vec3& position, NavFilter filter) const
{
    return find(position, filter, false);
}

std::optional<std::uint32_t> NavLocator::nearestOnGround(glm::vec2 position, NavFilter filter) const
{
    return find(glm::vec3(position.x, 0.0F, position.y), filter, true);
}

NavPosition::NavPosition(const NavLocator& locator, NavFilter filter)
    : m_locator(locator)
    , m_filter(filter)
{
}

NavPosition::Found NavPosition::searchAround(std::uint32_t node, int depth, std::optional<std::uint32_t> excludeWay,
    const glm::vec3& position, const MapCoverage* coverage) const
{
    // 0x82d9cb18.
    const NavFile& nav = m_locator.network();
    Found best{node, squaredDistance(nav.nodes[node].position, position)};
    for (const NavLink& link : nav.nodes[node].links) {
        if ((excludeWay && link.way == *excludeWay) || !m_locator.acceptsWay(link.way, m_filter)) {
            continue;
        }
        const std::vector<std::uint32_t>& nodes = nav.ways[link.way].nodes;
        const int at = static_cast<int>(link.position);
        for (int i = at - kSearchBefore; i < at + kSearchAfter; ++i) {
            if (i < 0 || i >= static_cast<int>(nodes.size()) || i == at) {
                continue;
            }
            const std::uint32_t other = nodes[static_cast<std::size_t>(i)];
            const float d = squaredDistance(nav.nodes[other].position, position);
            if (d < best.squaredDistance) {
                best = {other, d};
            }
            if (nav.nodes[other].links.size() > 1 && depth > 0) {
                const Found deeper = searchAround(other, depth - 1, link.way, position, coverage);
                if (deeper.squaredDistance < best.squaredDistance) {
                    best = deeper;
                }
            }
        }
    }
    if (coverage) {
        for (const std::uint32_t other : coverage->linkedNodes(node)) {
            const float d = squaredDistance(nav.nodes[other].position, position);
            if (d < best.squaredDistance && m_locator.acceptsNode(other, m_filter)) {
                best = {other, d};
            }
        }
    }
    return best;
}

void NavPosition::update(const glm::vec3& position, const MapCoverage* coverage)
{
    // 0x82d9f678.
    if (position.y < kLowestHeight) {
        m_node.reset();
        return;
    }
    if (m_node) {
        const Found found = searchAround(*m_node, kSearchDepth, std::nullopt, position, coverage);
        if (found.squaredDistance <= kLostSquaredDistance) {
            if (found.node != *m_node) {
                m_previous = m_node;
                m_node = found.node;
            }
            return;
        }
    }
    // 0x82d9ea88.
    m_previous.reset();
    m_node = m_locator.nearest(position, m_filter);
}

RouteTuning RouteTuning::load(const Vfs& vfs)
{
    const Bytes data = vfs.readOrThrow(kRegistryPath);
    pugi::xml_document document;
    if (!document.load_buffer(data.data(), data.size())) {
        throw DataError("{}: not XML", kRegistryPath);
    }
    std::unordered_map<std::string, float> values;
    for (const pugi::xml_node value : document.child("Registry").children("Value")) {
        values.emplace(value.attribute("id").as_string(), value.attribute("value").as_float());
    }
    const auto get = [&](const char* id) {
        const std::string key = std::string(kSatNavSpeeds) + id;
        const auto it = values.find(key);
        if (it == values.end()) {
            throw DataError("{}: no {}", kRegistryPath, key);
        }
        return it->second;
    };
    RouteTuning tuning;
    tuning.mphARoad = get("MPH_ARoad");
    tuning.mphBRoad = get("MPH_BRoad");
    tuning.mphDirtRoad = get("MPH_DirtRoad");
    tuning.mphFreeway = get("MPH_Freeway");
    tuning.mphNone = get("MPH_None");
    tuning.mphShortcut = get("MPH_Shortcut");
    tuning.mphUnknownRoad = get("MPH_UnknownRoad");
    tuning.oneWayMultiplier = get("OneWayMultiplier");
    tuning.shortcutProbability = get("ShortcutProbability");
    tuning.turnDelaySeconds = get("TurnDelaySeconds");
    return tuning;
}

RouteSearch::RouteSearch(const NavFile& nav, const RouteTuning& tuning)
    : m_nav(nav)
    , m_tuning(tuning)
{
}

float RouteSearch::speed(std::uint32_t way, bool shortcutsAllowed) const
{
    // 0x82da1ae0, 0x82da19f8.
    const std::string_view type = m_nav.ways[way].tag(kRoadType);
    float mph = m_tuning.mphUnknownRoad;
    if (type == "a") {
        mph = m_tuning.mphARoad;
    } else if (type == "b") {
        mph = m_tuning.mphBRoad;
    } else if (type == "dirt") {
        mph = m_tuning.mphDirtRoad;
    } else if (type == "freeway") {
        mph = m_tuning.mphFreeway;
    } else if (type == kNoRoad) {
        mph = m_tuning.mphNone;
    } else if (type == kShortcut) {
        if (!shortcutsAllowed) {
            return kClosedShortcutSpeed;
        }
        mph = m_tuning.mphShortcut;
    }
    return mph * kMetresPerSecondPerMph;
}

float RouteSearch::segmentCost(
    std::uint32_t from, std::uint32_t to, const glm::vec3& forward, bool shortcutsAllowed) const
{
    // 0x82d9b760: the first of `from`'s ways with `to` next to it, and
    // whether `to` comes before it there.
    std::optional<std::uint32_t> way;
    bool backwards = false;
    for (const NavLink& link : m_nav.nodes[from].links) {
        const std::vector<std::uint32_t>& nodes = m_nav.ways[link.way].nodes;
        const std::size_t at = link.position;
        if (at + 1 < nodes.size() && nodes[at + 1] == to) {
            way = link.way;
            break;
        }
        if (at > 0 && at - 1 < nodes.size() && nodes[at - 1] == to) {
            way = link.way;
            backwards = true;
            break;
        }
    }
    // 0x82da29c0.
    const float wayspeed = way ? speed(*way, shortcutsAllowed) : m_tuning.mphUnknownRoad * kMetresPerSecondPerMph;
    const glm::vec3 along = m_nav.nodes[to].position - m_nav.nodes[from].position;
    float cost = glm::length(along) / wayspeed;
    if (glm::dot(forward, forward) > 0.0F && glm::dot(forward, along) < 0.0F) {
        cost += m_tuning.turnDelaySeconds;
    }
    if (way && backwards && !m_nav.ways[*way].tag(kOneWay).empty()) {
        cost *= m_tuning.oneWayMultiplier;
    }
    return cost;
}

std::vector<std::uint32_t> RouteSearch::find(
    std::uint32_t start, std::uint32_t goal, const glm::vec3& forward, bool shortcutsAllowed) const
{
    struct State {
        std::uint32_t node = 0;
        /// The node before this one along `way`, or the node itself for
        /// the start.
        std::uint32_t previous = 0;
        /// The way the state was reached along; none for the start.
        std::optional<std::uint32_t> way;
        std::optional<std::size_t> parent;
        float cost = 0.0F;
        float estimate = 0.0F;
        bool open = false;
        /// Bumped whenever the state is pushed again, so that older heap
        /// entries for it are skipped.
        std::uint32_t version = 0;
    };
    struct Entry {
        float priority = 0.0F;
        std::size_t state = 0;
        std::uint32_t version = 0;
        bool operator>(const Entry& other) const { return priority > other.priority; }
    };

    const float estimateSpeed = m_tuning.mphUnknownRoad * kMetresPerSecondPerMph;
    const glm::vec3& target = m_nav.nodes[goal].position;
    const auto estimate
        = [&](std::uint32_t node) { return glm::length(target - m_nav.nodes[node].position) / estimateSpeed; };
    const auto key = [](std::uint32_t node, std::uint32_t previous) {
        return (static_cast<std::uint64_t>(node) << 32U) | previous;
    };
    const auto indexIn = [&](std::uint32_t way, std::uint32_t node) {
        // 0x82da1478: the node's first place along the way.
        const std::vector<std::uint32_t>& nodes = m_nav.ways[way].nodes;
        return static_cast<std::size_t>(std::find(nodes.begin(), nodes.end(), node) - nodes.begin());
    };

    std::vector<State> states;
    std::unordered_map<std::uint64_t, std::size_t> known;
    std::priority_queue<Entry, std::vector<Entry>, std::greater<>> open;
    states.push_back({start, start, std::nullopt, std::nullopt, 0.0F, estimate(start), true, 0});
    known.emplace(key(start, start), 0);
    open.push({states[0].estimate, 0, 0});

    // A step's cost: the segments of `way` from `from` to `to` (0x82d9f22c),
    // the first out of the start with the car's facing.
    const auto stepCost = [&](std::uint32_t way, std::uint32_t from, std::uint32_t to, bool fromStart) {
        const std::vector<std::uint32_t>& nodes = m_nav.ways[way].nodes;
        std::size_t at = indexIn(way, from);
        const std::size_t end = indexIn(way, to);
        float cost = 0.0F;
        bool first = fromStart;
        while (at != end && at < nodes.size() && end < nodes.size()) {
            const std::size_t next = at < end ? at + 1 : at - 1;
            cost += segmentCost(nodes[at], nodes[next], first ? forward : glm::vec3(0.0F), shortcutsAllowed);
            first = false;
            at = next;
        }
        return cost;
    };

    while (!open.empty()) {
        const Entry entry = open.top();
        open.pop();
        if (!states[entry.state].open || states[entry.state].version != entry.version) {
            continue;
        }
        states[entry.state].open = false;
        const State current = states[entry.state];
        if (current.node == goal) {
            // 0x82d9ef54: back from the goal, with the nodes along each
            // step's way between its ends.
            std::vector<std::uint32_t> route{current.node};
            std::optional<std::size_t> at = entry.state;
            while (at && states[*at].parent) {
                const State& step = states[*at];
                const std::uint32_t from = states[*step.parent].node;
                const std::vector<std::uint32_t>& nodes = m_nav.ways[*step.way].nodes;
                const std::size_t a = indexIn(*step.way, from);
                const std::size_t b = indexIn(*step.way, step.node);
                if (a > b + 1) {
                    for (std::size_t i = b + 1; i < a; ++i) {
                        route.push_back(nodes[i]);
                    }
                } else if (b > a + 1) {
                    for (std::size_t i = b - 1; i > a; --i) {
                        route.push_back(nodes[i]);
                    }
                }
                route.push_back(from);
                at = step.parent;
            }
            std::reverse(route.begin(), route.end());
            return route;
        }

        // 0x82d9e838: the next junction, or the goal, each way along each
        // of the node's ways, not going back to the parent's node.
        const std::optional<std::uint32_t> parentNode
            = current.parent ? std::optional<std::uint32_t>(states[*current.parent].node) : std::nullopt;
        const auto consider = [&](std::uint32_t way, std::uint32_t node, std::uint32_t previous) {
            const float cost = current.cost + stepCost(way, current.node, node, !current.parent);
            const auto [it, added] = known.emplace(key(node, previous), states.size());
            if (added) {
                states.push_back({node, previous, way, entry.state, cost, estimate(node), true, 0});
            } else {
                State& existing = states[it->second];
                if (existing.cost <= cost) {
                    return;
                }
                existing.way = way;
                existing.parent = entry.state;
                existing.cost = cost;
                existing.open = true;
                ++existing.version;
            }
            const State& pushed = states[it->second];
            open.push({pushed.cost + pushed.estimate, it->second, pushed.version});
        };
        for (const NavLink& link : m_nav.nodes[current.node].links) {
            const std::vector<std::uint32_t>& nodes = m_nav.ways[link.way].nodes;
            if (nodes.size() <= 1) {
                continue;
            }
            for (const int direction : {1, -1}) {
                std::uint32_t previous = current.node;
                for (auto i = static_cast<std::ptrdiff_t>(link.position) + direction;
                    i >= 0 && i < static_cast<std::ptrdiff_t>(nodes.size()); i += direction) {
                    const std::uint32_t node = nodes[static_cast<std::size_t>(i)];
                    if (parentNode && node == *parentNode) {
                        break;
                    }
                    if (node == goal || m_nav.nodes[node].links.size() > 1) {
                        consider(link.way, node, previous);
                        break;
                    }
                    previous = node;
                }
            }
        }
    }
    return {};
}

Navigation::Navigation(const Vfs& vfs, const std::string& track)
{
    const std::string path = std::format("media/tracks/{}/{}.nav", track, track);
    m_nav = std::make_unique<NavFile>(NavFile::parse(vfs.readOrThrow(path), path));
    m_locator = std::make_unique<NavLocator>(*m_nav);
    m_coverage = std::make_unique<MapCoverage>(*m_nav, RoadDiscoveryTuning::load(vfs));
    m_player = std::make_unique<NavPosition>(*m_locator, NavFilter::NoShortcut);
    m_routeTuning = RouteTuning::load(vfs);
    m_search = std::make_unique<RouteSearch>(*m_nav, m_routeTuning);
}

Navigation::~Navigation() = default;

void Navigation::update(const glm::vec3& carPosition, const glm::vec3& carForward)
{
    m_player->update(carPosition, m_coverage.get());
    m_coverage->update(m_player->node());
    m_carPosition = carPosition;
    m_carForward = carForward;
    if (!m_goal) {
        return;
    }
    if (m_route.empty()) {
        plan();
    } else {
        follow();
    }
}

void Navigation::setDestination(std::optional<glm::vec2> waypoint)
{
    m_route.clear();
    m_progress = 0;
    m_routeDistance = 0.0F;
    m_routeLength = 0.0F;
    m_goal.reset();
    if (!waypoint) {
        return;
    }
    // 0x828af1b8 asks for the nearest node with filter 0.
    m_goal = m_locator->nearestOnGround(*waypoint, NavFilter::NoShortcutOrNone);
    plan();
}

void Navigation::plan()
{
    // 0x82a52ae8 and 0x82a4ddc8: nothing is searched until the car is on
    // the network.
    const std::optional<std::uint32_t> start = m_player->node();
    if (!m_goal || !start) {
        return;
    }
    // 0x82da1980: shortcuts take their own speed for a search when a draw
    // falls below ShortcutProbability (the draw, 0x82dbe768, is taken to be
    // uniform on 0 to 1).
    bool shortcutsAllowed = false;
    if (m_routeTuning.shortcutProbability > 0.0F) {
        std::uniform_real_distribution<float> draw(0.0F, 1.0F);
        shortcutsAllowed = draw(m_random) < m_routeTuning.shortcutProbability;
    }
    m_route = m_search->find(*start, *m_goal, m_carForward, shortcutsAllowed);
    m_progress = 0;
    m_routeDistance = 0.0F;
    m_routeLength = 0.0F;
    if (m_route.empty()) {
        m_goal.reset();
    }
}

void Navigation::follow()
{
    // 0x82a549c0 with a route held (state 3).
    const std::optional<std::uint32_t> node = m_player->node();
    if (!node) {
        return;
    }
    if (*node == *m_goal) {
        m_goal.reset();
        m_route.clear();
        m_progress = 0;
        m_routeDistance = 0.0F;
        m_routeLength = 0.0F;
        return;
    }
    const auto found = std::find(m_route.begin() + static_cast<std::ptrdiff_t>(m_progress), m_route.end(), *node);
    if (found != m_route.end()) {
        m_progress = static_cast<std::size_t>(found - m_route.begin());
        m_routeDistance = remainingDistance();
        if (m_routeLength == 0.0F) {
            m_routeLength = m_routeDistance;
        }
        return;
    }
    if (squaredDistance(m_carPosition, m_nav->nodes[m_route[m_progress]].position) > kLostSquaredDistance) {
        plan();
    }
}

float Navigation::remainingDistance() const
{
    const std::size_t next = m_progress + 1;
    if (next >= m_route.size()) {
        return 0.0F;
    }
    float distance = glm::length(m_carPosition - m_nav->nodes[m_route[next]].position);
    for (std::size_t i = next + 1; i < m_route.size(); ++i) {
        distance += glm::length(m_nav->nodes[m_route[i]].position - m_nav->nodes[m_route[i - 1]].position);
    }
    return distance;
}

std::vector<glm::vec2> Navigation::route() const
{
    std::vector<glm::vec2> points;
    points.reserve(m_route.size());
    for (const std::uint32_t node : m_route) {
        const glm::vec3& at = m_nav->nodes[node].position;
        points.emplace_back(at.x, at.z);
    }
    return points;
}

} // namespace openhorizon::ui
