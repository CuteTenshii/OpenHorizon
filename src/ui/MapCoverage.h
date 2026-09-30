#pragma once

#include "Minimap.h"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::ui {

/// The road discovery tunables (Activities/RoadDiscovery/* in
/// media/gametunablesettings/globalregistry.xml; registered at 0x8328d228
/// and 0x8328d2b8).
struct RoadDiscoveryTuning {
    /// Nodes of two one-way ways of the same road closer than this (squared
    /// metres) count as one place (read at 0x82d98d60).
    float maxSquaredDistanceForLinkedNodes = 0.0F;
    /// An undriven node is counted too when a driven node can be reached
    /// from it within this many metres (read at 0x82d95084).
    float unvisitedDistanceThreshold = 0.0F;

    /// Throws DataError when a value is missing.
    static RoadDiscoveryTuning load(const Vfs& vfs);
};

/// Which roads the player has driven: the game's CMapCoverage
/// (Navigation, constructor 0x82d9a410, set up by 0x82d99950, updated every
/// frame by 0x82d95598 from the navigation component's update 0x826c21d8).
///
/// Verified in the executable:
/// - Each node of the road network has a visit count (+12, an array the
///   road discovery game state keeps, 0x82598198). The node the player's
///   car is on (see NavPosition) is counted each time it changes
///   (0x82d94fa8), and so is every node linked to it.
/// - Counting a node also counts, once, each undriven neighbour along its
///   ways from which a counted node can be reached within
///   UnvisitedDistanceThreshold metres without going back through it
///   (0x82d93c48); that spreads on from each node so counted.
/// - A node with a discovery_zone tag counts, once, every node reachable
///   from it along the ways through nodes of the same zone (0x82d94eb8,
///   0x82d958f0).
/// - Linked nodes (0x82d98d20, pruned by 0x82d945a8): within each road
///   (the ways sharing a display_name), every pair of nodes on two
///   different ways that both carry a oneway tag and lie within
///   MaxSquaredDistanceForLinkedNodes of each other, both carriageways of a
///   divided road. A link from A to K is dropped when A is also linked to a
///   node B that shares a way with K and lies closer to A than K does,
///   except for K's nearest link.
/// - A way can be discovered when its road_type is set and is neither
///   "none" nor "shortcut" (0x82d90c68). A road, the discoverable ways of
///   one display_name, is discovered once every node of each of its ways
///   has been counted (0x82d90d18); roadCount() is the number of roads
///   (+64) and roadsDiscovered() the number discovered (+72), which the
///   "ROADS DRIVEN" notification (IDS_RoadDiscovered_Title, message
///   "{0}/{1}" from the Activities table, posted by 0x826bf7a0) shows as
///   discovered/total when a road is completed.
/// - Discovery runs while the CEnableMapDiscovery behaviour has enabled it
///   (+108, 0x82626458); the free roam, career race, multiplayer and E3
///   flows all enter global.enable_map_discovery and none enters
///   disable_map_discovery.
///
/// Inferred: roads are ordered by where their name comes in the network's
/// value table, as the game's map keys them by the value record's address.
class MapCoverage {
public:
    /// Keeps a reference to `nav`, which must outlive the coverage.
    MapCoverage(const NavFile& nav, const RoadDiscoveryTuning& tuning);

    void setEnabled(bool enabled) { m_enabled = enabled; }
    bool enabled() const { return m_enabled; }

    /// One frame of discovery with the node the player's car is on.
    void update(std::optional<std::uint32_t> node);

    /// How many times node `node` has been counted.
    int visits(std::uint32_t node) const;

    /// The colour ratio of each point of way `way` for the map's coloured
    /// lines (CMapEntityWay's colour source, 0x82d91f00): 1 where the
    /// segment from the point to the next has a counted end, 0 elsewhere,
    /// the last point taking the value of the one before. Empty for a way
    /// index out of range.
    std::vector<float> wayRatios(std::size_t way) const;

    /// The number of ways of the network the coverage was built for.
    std::size_t wayCount() const { return m_nav.ways.size(); }

    /// Changes whenever a node is counted for the first time, which is when
    /// wayRatios() can change.
    std::uint64_t revision() const { return m_revision; }

    /// Whether each node has been counted, for the profile (CRoadDiscovery
    /// keeps one bit per node).
    std::vector<bool> visitedNodes() const;
    /// Sets each node's count to 1 or 0 from `visited`, as loading the
    /// profile does, and recounts the discovered roads without reporting
    /// them as newly discovered. Nodes past its end are set to 0.
    void restoreVisited(const std::vector<bool>& visited);

    std::size_t roadCount() const { return m_roads.size(); }
    std::size_t roadsDiscovered() const { return m_discovered; }
    /// The display names of the roads discovered in the last update.
    const std::vector<std::string>& newlyDiscovered() const { return m_newRoads; }

    /// The nodes linked to `node` (see above); empty when none.
    const std::vector<std::uint32_t>& linkedNodes(std::uint32_t node) const;

private:
    struct RoadWay {
        std::uint32_t way = 0;
        bool complete = false;
    };
    struct Road {
        std::string name;
        std::vector<RoadWay> ways;
        bool discovered = false;
    };

    bool discoverable(std::uint32_t way) const;
    bool allVisited(std::uint32_t way) const;
    /// The nodes before and after `node` on each way through it
    /// (0x82da9cc8).
    std::vector<std::uint32_t> neighbours(std::uint32_t node) const;
    float squaredDistance(std::uint32_t a, std::uint32_t b) const;
    void count(std::uint32_t node, bool onlyIfUnvisited);
    bool reachesVisited(std::uint32_t node, float budget, std::vector<std::uint32_t>& path) const;
    void walkZone(
        std::uint32_t way, std::uint32_t position, const std::string& zone, std::vector<std::uint32_t>& visited);
    bool stepZone(
        std::uint32_t node, std::uint32_t fromWay, const std::string& zone, std::vector<std::uint32_t>& visited);
    void touch(std::uint32_t way);
    /// Marks the roads whose ways are all counted as discovered.
    void recountRoads();
    void linkRoadNodes();
    void pruneLinks();

    const NavFile& m_nav;
    RoadDiscoveryTuning m_tuning;
    std::vector<int> m_visits;
    std::vector<Road> m_roads;
    /// Road index by display name.
    std::map<std::string, std::size_t> m_roadIndex;
    std::map<std::uint32_t, std::vector<std::uint32_t>> m_links;
    std::vector<std::uint32_t> m_touched;
    std::vector<std::string> m_newRoads;
    std::optional<std::uint32_t> m_last;
    std::size_t m_discovered = 0;
    std::uint64_t m_revision = 0;
    bool m_enabled = true;
};

} // namespace openhorizon::ui
