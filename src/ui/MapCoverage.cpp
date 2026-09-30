#include "MapCoverage.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace openhorizon::ui {

namespace {

constexpr const char* kRegistryPath = "media/gametunablesettings/globalregistry.xml";

/// The tags CMapCoverage looks up when it is built (0x82d9a410).
constexpr std::string_view kDiscoveryZone = "discovery_zone";
constexpr std::string_view kDisplayName = "display_name";
constexpr std::string_view kRoadType = "road_type";
constexpr std::string_view kNoRoad = "none";
constexpr std::string_view kShortcut = "shortcut";
/// The tag the linked-node pass requires on both ways (0x82d98d20).
constexpr std::string_view kOneWay = "oneway";

} // namespace

RoadDiscoveryTuning RoadDiscoveryTuning::load(const Vfs& vfs)
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
        const auto it = values.find(std::string("Activities/RoadDiscovery/") + id);
        if (it == values.end()) {
            throw DataError("{}: no Activities/RoadDiscovery/{}", kRegistryPath, id);
        }
        return it->second;
    };
    RoadDiscoveryTuning tuning;
    tuning.maxSquaredDistanceForLinkedNodes = get("MaxSquaredDistanceForLinkedNodes");
    tuning.unvisitedDistanceThreshold = get("UnvisitedDistanceThreshold");
    return tuning;
}

MapCoverage::MapCoverage(const NavFile& nav, const RoadDiscoveryTuning& tuning)
    : m_nav(nav)
    , m_tuning(tuning)
    , m_visits(nav.nodes.size(), 0)
{
    // 0x82d99950: the discoverable ways grouped by display name, in the
    // order of the names in the value table.
    std::unordered_map<std::string_view, std::size_t> valueOrder;
    for (std::size_t i = 0; i < nav.values.size(); ++i) {
        valueOrder.emplace(nav.values[i], i);
    }
    std::map<std::size_t, Road> byValue;
    for (std::uint32_t way = 0; way < nav.ways.size(); ++way) {
        const std::string_view name = nav.ways[way].tag(kDisplayName);
        if (name.empty() || !discoverable(way)) {
            continue;
        }
        const auto order = valueOrder.find(name);
        Road& road = byValue[order == valueOrder.end() ? nav.values.size() : order->second];
        road.name = std::string(name);
        road.ways.push_back({way, false});
    }
    for (auto& [order, road] : byValue) {
        m_roadIndex.emplace(road.name, m_roads.size());
        m_roads.push_back(std::move(road));
    }
    linkRoadNodes();
    pruneLinks();
    recountRoads();
}

void MapCoverage::recountRoads()
{
    // Roads whose every node already counts start discovered (their counts
    // come from the saved profile in the game).
    m_discovered = 0;
    for (Road& road : m_roads) {
        road.discovered = false;
        for (RoadWay& entry : road.ways) {
            entry.complete = allVisited(entry.way);
        }
        if (std::all_of(road.ways.begin(), road.ways.end(), [](const RoadWay& entry) { return entry.complete; })) {
            road.discovered = true;
            ++m_discovered;
        }
    }
}

void MapCoverage::restoreVisited(const std::vector<bool>& visited)
{
    // CRoadDiscovery's load (0x8259b800) sets each node's count to its bit.
    for (std::size_t node = 0; node < m_visits.size(); ++node) {
        m_visits[node] = node < visited.size() && visited[node] ? 1 : 0;
    }
    m_newRoads.clear();
    m_touched.clear();
    ++m_revision;
    recountRoads();
}

std::vector<bool> MapCoverage::visitedNodes() const
{
    std::vector<bool> visited(m_visits.size());
    for (std::size_t node = 0; node < m_visits.size(); ++node) {
        visited[node] = m_visits[node] > 0;
    }
    return visited;
}

bool MapCoverage::discoverable(std::uint32_t way) const
{
    const std::string_view type = m_nav.ways[way].tag(kRoadType);
    return !type.empty() && type != kNoRoad && type != kShortcut;
}

bool MapCoverage::allVisited(std::uint32_t way) const
{
    const std::vector<std::uint32_t>& nodes = m_nav.ways[way].nodes;
    return std::all_of(nodes.begin(), nodes.end(), [&](std::uint32_t node) { return m_visits[node] > 0; });
}

std::vector<std::uint32_t> MapCoverage::neighbours(std::uint32_t node) const
{
    std::vector<std::uint32_t> result;
    for (const NavLink& link : m_nav.nodes[node].links) {
        const std::vector<std::uint32_t>& nodes = m_nav.ways[link.way].nodes;
        if (link.position != 0) {
            result.push_back(nodes[link.position - 1]);
        }
        if (link.position + 1 < nodes.size()) {
            result.push_back(nodes[link.position + 1]);
        }
    }
    return result;
}

float MapCoverage::squaredDistance(std::uint32_t a, std::uint32_t b) const
{
    const glm::vec3 d = m_nav.nodes[a].position - m_nav.nodes[b].position;
    return glm::dot(d, d);
}

int MapCoverage::visits(std::uint32_t node) const
{
    return node < m_visits.size() ? m_visits[node] : 0;
}

const std::vector<std::uint32_t>& MapCoverage::linkedNodes(std::uint32_t node) const
{
    static const std::vector<std::uint32_t> kNone;
    const auto it = m_links.find(node);
    return it == m_links.end() ? kNone : it->second;
}

std::vector<float> MapCoverage::wayRatios(std::size_t way) const
{
    if (way >= m_nav.ways.size()) {
        return {};
    }
    const std::vector<std::uint32_t>& nodes = m_nav.ways[way].nodes;
    std::vector<float> ratios(nodes.size(), 0.0F);
    if (nodes.size() < 2) {
        return ratios;
    }
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i) {
        ratios[i] = std::max(m_visits[nodes[i]], m_visits[nodes[i + 1]]) > 0 ? 1.0F : 0.0F;
    }
    ratios.back() = ratios[ratios.size() - 2];
    return ratios;
}

void MapCoverage::count(std::uint32_t node, bool onlyIfUnvisited)
{
    // 0x82d94fa8.
    if (!onlyIfUnvisited || m_visits[node] == 0) {
        if (m_visits[node]++ == 0) {
            ++m_revision;
        }
    }
    for (const std::uint32_t next : neighbours(node)) {
        if (m_visits[next] > 0) {
            continue;
        }
        std::vector<std::uint32_t> path{node};
        if (reachesVisited(next, m_tuning.unvisitedDistanceThreshold, path)) {
            count(next, true);
        }
    }
    for (const NavLink& link : m_nav.nodes[node].links) {
        touch(link.way);
    }
}

bool MapCoverage::reachesVisited(std::uint32_t node, float budget, std::vector<std::uint32_t>& path) const
{
    // 0x82d93c48: a depth-first walk that never pops its path, so a node
    // tried once is not tried again from another branch.
    path.push_back(node);
    bool found = false;
    for (const std::uint32_t next : neighbours(node)) {
        if (found) {
            break;
        }
        if (std::find(path.begin(), path.end(), next) != path.end()) {
            continue;
        }
        const float distance = std::sqrt(squaredDistance(node, next));
        if (!(distance < budget)) {
            continue;
        }
        found = m_visits[next] > 0 || reachesVisited(next, budget - distance, path);
    }
    return found;
}

void MapCoverage::walkZone(
    std::uint32_t way, std::uint32_t position, const std::string& zone, std::vector<std::uint32_t>& visited)
{
    // 0x82d94eb8: along the way both ways from `position`, while the nodes
    // stay in the zone.
    touch(way);
    const std::vector<std::uint32_t>& nodes = m_nav.ways[way].nodes;
    for (std::int64_t i = static_cast<std::int64_t>(position) - 1; i >= 0; --i) {
        if (!stepZone(nodes[static_cast<std::size_t>(i)], way, zone, visited)) {
            break;
        }
    }
    for (std::size_t i = position + 1; i < nodes.size(); ++i) {
        if (!stepZone(nodes[i], way, zone, visited)) {
            break;
        }
    }
}

bool MapCoverage::stepZone(
    std::uint32_t node, std::uint32_t fromWay, const std::string& zone, std::vector<std::uint32_t>& visited)
{
    // 0x82d958f0.
    if (std::find(visited.begin(), visited.end(), node) != visited.end()
        || m_nav.nodes[node].tag(kDiscoveryZone) != zone) {
        return false;
    }
    visited.push_back(node);
    count(node, true);
    for (const NavLink& link : m_nav.nodes[node].links) {
        if (link.way != fromWay) {
            walkZone(link.way, link.position, zone, visited);
        }
    }
    return true;
}

void MapCoverage::touch(std::uint32_t way)
{
    // 0x82d944c8.
    if (discoverable(way) && std::find(m_touched.begin(), m_touched.end(), way) == m_touched.end()) {
        m_touched.push_back(way);
    }
}

void MapCoverage::update(std::optional<std::uint32_t> node)
{
    // 0x82d95598.
    m_touched.clear();
    m_newRoads.clear();
    if (!m_enabled || !node || *node >= m_visits.size() || node == m_last) {
        return;
    }
    count(*node, false);
    m_last = node;
    const std::vector<std::uint32_t> linked = linkedNodes(*node);
    for (const std::uint32_t other : linked) {
        count(other, false);
    }
    const std::string_view zone = m_nav.nodes[*node].tag(kDiscoveryZone);
    if (!zone.empty()) {
        const std::string zoneName(zone);
        std::vector<std::uint32_t> visited;
        for (const NavLink& link : m_nav.nodes[*node].links) {
            walkZone(link.way, link.position, zoneName, visited);
        }
    }
    for (const std::uint32_t way : m_touched) {
        const auto it = m_roadIndex.find(std::string(m_nav.ways[way].tag(kDisplayName)));
        if (it == m_roadIndex.end()) {
            continue;
        }
        Road& road = m_roads[it->second];
        if (road.discovered) {
            continue;
        }
        bool changed = false;
        for (RoadWay& entry : road.ways) {
            if (!entry.complete && entry.way == way) {
                if (allVisited(way)) {
                    entry.complete = true;
                    changed = true;
                }
                break;
            }
        }
        if (changed
            && std::all_of(road.ways.begin(), road.ways.end(), [](const RoadWay& entry) { return entry.complete; })) {
            road.discovered = true;
            m_newRoads.push_back(road.name);
            ++m_discovered;
        }
    }
}

void MapCoverage::linkRoadNodes()
{
    // 0x82d98d20.
    for (const Road& road : m_roads) {
        for (std::size_t i = 0; i < road.ways.size(); ++i) {
            const NavWay& first = m_nav.ways[road.ways[i].way];
            if (first.tag(kOneWay).empty()) {
                continue;
            }
            for (std::size_t j = i + 1; j < road.ways.size(); ++j) {
                const NavWay& second = m_nav.ways[road.ways[j].way];
                if (second.tag(kOneWay).empty()) {
                    continue;
                }
                for (const std::uint32_t a : first.nodes) {
                    for (const std::uint32_t b : second.nodes) {
                        if (squaredDistance(a, b) <= m_tuning.maxSquaredDistanceForLinkedNodes) {
                            m_links[a].push_back(b);
                            m_links[b].push_back(a);
                        }
                    }
                }
            }
        }
    }
}

void MapCoverage::pruneLinks()
{
    // 0x82d945a8.
    const auto sharesWay = [&](std::uint32_t a, std::uint32_t b) {
        for (const NavLink& x : m_nav.nodes[a].links) {
            for (const NavLink& y : m_nav.nodes[b].links) {
                if (x.way == y.way) {
                    return true;
                }
            }
        }
        return false;
    };
    for (auto& [key, list] : m_links) {
        std::uint32_t nearest = key;
        float nearestDistance = std::numeric_limits<float>::max();
        for (const std::uint32_t other : list) {
            const float d = squaredDistance(other, key);
            if (d < nearestDistance) {
                nearest = other;
                nearestDistance = d;
            }
        }
        bool changed = true;
        while (changed) {
            changed = false;
            if (list.size() <= 1) {
                break;
            }
            for (std::size_t i = 0; i < list.size() && !changed; ++i) {
                const std::uint32_t a = list[i];
                if (a == nearest) {
                    continue;
                }
                const auto other = m_links.find(a);
                if (other == m_links.end()) {
                    continue;
                }
                std::vector<std::uint32_t>& links = other->second;
                for (std::size_t j = 0; j < links.size() && !changed; ++j) {
                    const std::uint32_t b = links[j];
                    if (b == key || !sharesWay(b, key)) {
                        continue;
                    }
                    if (squaredDistance(key, a) > squaredDistance(b, a)) {
                        const auto back = std::find(links.begin(), links.end(), key);
                        if (back != links.end()) {
                            links.erase(back);
                        }
                        list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                        changed = true;
                    }
                }
            }
        }
    }
}

} // namespace openhorizon::ui
