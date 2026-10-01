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

/// Activities/RoadDiscovery/* in globalregistry.xml (0x8328d228, 0x8328d2b8).
struct RoadDiscoveryTuning {
    /// Squared metres within which one-way nodes link (0x82d98d60).
    float maxSquaredDistanceForLinkedNodes = 0.0F;
    /// Metres from an undriven node to a driven one to count it (0x82d95084).
    float unvisitedDistanceThreshold = 0.0F;

    /// Throws DataError when a value is missing.
    static RoadDiscoveryTuning load(const Vfs& vfs);
};

/// Which roads the player has driven: CMapCoverage (constructor 0x82d9a410,
/// setup 0x82d99950, per-frame update 0x82d95598 from 0x826c21d8).
///
/// Verified in the executable:
/// - The node under the car and its linked nodes are counted on change
///   (visit counts +12, 0x82598198; 0x82d94fa8).
/// - Counting also counts, once, undriven neighbours that reach a counted
///   node within UnvisitedDistanceThreshold without going back (0x82d93c48).
/// - A discovery_zone node counts every node of its zone (0x82d94eb8,
///   0x82d958f0).
/// - Links (0x82d98d20, pruned by 0x82d945a8): nodes of two oneway ways of
///   one display_name within MaxSquaredDistanceForLinkedNodes; A to K is
///   dropped when a nearer B on K's way is linked, except K's nearest link.
/// - Ways with road_type other than "none" and "shortcut" are discoverable
///   (0x82d90c68); a road is discovered when all their nodes are counted
///   (0x82d90d18). Counts +64 and +72 feed "ROADS DRIVEN"
///   (IDS_RoadDiscovered_Title, "{0}/{1}", 0x826bf7a0).
/// - Enabled by CEnableMapDiscovery (+108, 0x82626458), which every flow
///   enters.
///
/// Inferred: roads are ordered by their name's place in the value table.
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

    /// Per point of `way` (0x82d91f00): 1 when the segment to the next point
    /// has a counted end, else 0; the last copies the one before.
    std::vector<float> wayRatios(std::size_t way) const;

    std::size_t wayCount() const { return m_nav.ways.size(); }

    /// Changes when a node is first counted, the only time wayRatios() can.
    std::uint64_t revision() const { return m_revision; }

    /// For the profile, which keeps one bit per node (CRoadDiscovery).
    std::vector<bool> visitedNodes() const;
    /// Counts become 1 or 0 (0 past the end); restored roads are not
    /// reported as newly discovered.
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
    /// 0x82da9cc8.
    std::vector<std::uint32_t> neighbours(std::uint32_t node) const;
    float squaredDistance(std::uint32_t a, std::uint32_t b) const;
    void count(std::uint32_t node, bool onlyIfUnvisited);
    bool reachesVisited(std::uint32_t node, float budget, std::vector<std::uint32_t>& path) const;
    void walkZone(
        std::uint32_t way, std::uint32_t position, const std::string& zone, std::vector<std::uint32_t>& visited);
    bool stepZone(
        std::uint32_t node, std::uint32_t fromWay, const std::string& zone, std::vector<std::uint32_t>& visited);
    void touch(std::uint32_t way);
    void recountRoads();
    void linkRoadNodes();
    void pruneLinks();

    const NavFile& m_nav;
    RoadDiscoveryTuning m_tuning;
    std::vector<int> m_visits;
    std::vector<Road> m_roads;
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
