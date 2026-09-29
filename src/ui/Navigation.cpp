#include "Navigation.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <format>
#include <limits>
#include <string_view>

namespace fh::ui {

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
/// and looks the node up afresh (0x82000d00).
constexpr float kLostSquaredDistance = 10000.0F;
/// Below this height the car is off the network (0x82246a9c).
constexpr float kLowestHeight = -1000.0F;

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

std::optional<std::uint32_t> NavLocator::search(const Cell& cell, const glm::vec3& position, NavFilter filter) const
{
    // 0x82d9cf80.
    std::optional<std::uint32_t> best;
    float bestDistance = std::numeric_limits<float>::max();
    for (const std::uint32_t node : cell.nodes) {
        const float d = squaredDistance(m_nav.nodes[node].position, position);
        if (d < bestDistance && acceptsNode(node, filter)) {
            best = node;
            bestDistance = d;
        }
    }
    return best;
}

std::optional<std::uint32_t> NavLocator::nearest(const glm::vec3& position, NavFilter filter) const
{
    // 0x82d9d070.
    if (m_empty) {
        return std::nullopt;
    }
    const Cell& own = m_cells[cellIndex(wrap(gridIndex(position.z)), wrap(gridIndex(position.x)))];
    if (const std::optional<std::uint32_t> found = search(own, position, filter)) {
        return found;
    }
    const Cell* closest = nullptr;
    float closestDistance = std::numeric_limits<float>::max();
    for (const Cell& cell : m_cells) {
        if (cell.nodes.empty()) {
            continue;
        }
        const float d = squaredDistance(cell.centre, position);
        if (d < closestDistance) {
            closest = &cell;
            closestDistance = d;
        }
    }
    return closest ? search(*closest, position, filter) : std::nullopt;
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

Navigation::Navigation(const Vfs& vfs, const std::string& track)
{
    const std::string path = std::format("media/tracks/{}/{}.nav", track, track);
    m_nav = std::make_unique<NavFile>(NavFile::parse(vfs.readOrThrow(path), path));
    m_locator = std::make_unique<NavLocator>(*m_nav);
    m_coverage = std::make_unique<MapCoverage>(*m_nav, RoadDiscoveryTuning::load(vfs));
    m_player = std::make_unique<NavPosition>(*m_locator, NavFilter::NoShortcut);
}

Navigation::~Navigation() = default;

void Navigation::update(const glm::vec3& carPosition)
{
    m_player->update(carPosition, m_coverage.get());
    m_coverage->update(m_player->node());
}

} // namespace fh::ui
