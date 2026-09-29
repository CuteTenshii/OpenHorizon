#pragma once

#include "MapCoverage.h"
#include "Minimap.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::ui {

/// Which nodes and ways a network position may use, by their ways'
/// road_type (the modes of 0x82d9c128 and 0x82d9c190, against "shortcut"
/// and "none" as set up at 0x82d9fdd0).
enum class NavFilter : std::uint8_t {
    /// Nodes with a way that is neither a shortcut nor "none"; ways that
    /// are neither.
    NoShortcutOrNone = 0,
    /// Nodes with a way that is not "none"; ways that are not "none".
    NoNone = 1,
    /// Nodes with a way that is not a shortcut; ways that are not
    /// shortcuts.
    NoShortcut = 2,
    Any = 3,
};

/// Finds the node nearest a place: the grid the navigation system builds
/// over the road network (the object at 0x82d9fdd0, grid built by
/// 0x82d9f860, searched by 0x82d9d070).
///
/// Verified: the candidates are the nodes with at least one way that has a
/// road_type (0x82d9eb10). The grid is 25 x 25 cells of 400 m starting at
/// -5000 m on x and z (the globals at 0x83398c34 and 0x83398c38, 1/400 at
/// 0x8359ddfc) whose indices wrap round modulo 25 (0x82d9b0c8, 0x82d9ec68);
/// a node also goes in the neighbouring cells, diagonals included, when it
/// lies within a fifth of a cell (80 m) of their edge. A search looks in
/// the cell of the place and, when that holds no node the filter accepts,
/// in the non-empty cell whose centre (at height 0) is nearest. Distances
/// are squared and three-dimensional (vsubfp128 and vmsum3fp128).
class NavLocator {
public:
    /// Keeps a reference to `nav`, which must outlive the locator.
    explicit NavLocator(const NavFile& nav);

    std::optional<std::uint32_t> nearest(const glm::vec3& position, NavFilter filter) const;
    /// As nearest(), for a place whose height is not known: distances are
    /// measured on the ground plane only.
    std::optional<std::uint32_t> nearestOnGround(glm::vec2 position, NavFilter filter) const;

    bool acceptsNode(std::uint32_t node, NavFilter filter) const;
    bool acceptsWay(std::uint32_t way, NavFilter filter) const;

    const NavFile& network() const { return m_nav; }

private:
    struct Cell {
        glm::vec3 centre{0.0F};
        std::vector<std::uint32_t> nodes;
    };

    std::optional<std::uint32_t> search(
        const Cell& cell, const glm::vec3& position, NavFilter filter, bool groundOnly) const;
    std::optional<std::uint32_t> find(const glm::vec3& position, NavFilter filter, bool groundOnly) const;
    void insert(std::uint32_t node, int column, int row);

    const NavFile& m_nav;
    std::vector<Cell> m_cells;
    bool m_empty = true;
};

/// Where a car is on the road network: the node it is at, tracked from
/// frame to frame (the network position the vehicle's navigation entry
/// holds at +8, created by 0x82d9f618 and updated by 0x82d9f678).
///
/// Verified: below 1000 m under the ground (the height's constant at
/// 0x82246a9c) the car is off the network. Without a node, the nearest one
/// is looked up (NavLocator). With one, the new node is the nearest to the
/// car of the node itself, the nodes up to ten places before and nine
/// after it along each of its ways the filter accepts, the same around
/// each junction met there once more (0x82d9cb18 with a depth of 1), and
/// its linked nodes the filter accepts (MapCoverage::linkedNodes); when
/// that is more than 100 m away (a squared distance above 10000, the
/// constant at 0x82000d00) the lookup starts afresh.
///
/// Inferred: the car's place is its body origin (the entry's position
/// source was not traced further than a virtual call).
class NavPosition {
public:
    NavPosition(const NavLocator& locator, NavFilter filter);

    /// `coverage` supplies the linked nodes; it may be null.
    void update(const glm::vec3& position, const MapCoverage* coverage);

    std::optional<std::uint32_t> node() const { return m_node; }
    std::optional<std::uint32_t> previous() const { return m_previous; }

private:
    struct Found {
        std::uint32_t node = 0;
        float squaredDistance = 0.0F;
    };
    Found searchAround(std::uint32_t node, int depth, std::optional<std::uint32_t> excludeWay,
        const glm::vec3& position, const MapCoverage* coverage) const;

    const NavLocator& m_locator;
    NavFilter m_filter;
    std::optional<std::uint32_t> m_node;
    std::optional<std::uint32_t> m_previous;
};

/// The sat-nav's route costs: Navigation/RoutingHeuristicSpeeds/SatNav/*
/// in media/gametunablesettings/globalregistry.xml (registered at
/// 0x833fb7ec to 0x833fb8c8). The route planner (0x82a55e10) takes this
/// set unless its flag is 1, when it takes the StreetRace one; the
/// navigation component (0x826c4bf8) passes 1 for sessions of type 3 and
/// 11, which free roam is taken not to be (inferred).
struct RouteTuning {
    float mphARoad = 0.0F;
    float mphBRoad = 0.0F;
    float mphDirtRoad = 0.0F;
    float mphFreeway = 0.0F;
    float mphNone = 0.0F;
    float mphShortcut = 0.0F;
    float mphUnknownRoad = 0.0F;
    float oneWayMultiplier = 0.0F;
    float shortcutProbability = 0.0F;
    float turnDelaySeconds = 0.0F;

    /// Throws DataError when a value is missing.
    static RouteTuning load(const Vfs& vfs);
};

/// The route search: the executable's A* (set up by 0x82d9e690, stepped
/// by 0x82d9eeb8) over the road network with the sat-nav's cost policy
/// (vtable 0x82247c48 on the speeds of 0x82da8c18).
///
/// Verified against the executable:
/// - A search state is a node together with the node before it along the
///   way it was reached by (the pair at +20 and +24 that the open and
///   closed lists compare), so the same junction reached from two sides is
///   two states; the goal is reached when a state's node is the goal node.
/// - A state's successors (0x82d9e838): along each way through its node
///   that has more than one node, in both directions, the first node that
///   is a junction (more than one link) or the goal; a direction ends,
///   without a successor, at the way's end or at the node of the state's
///   parent.
/// - The cost of a step is the sum, over the segments of the way between
///   the two nodes, of the segment's length (three-dimensional, between
///   the nodes' positions) over the speed of the way the segment lies on
///   (0x82da29c0; the way is the first of the first node's ways that has
///   the second node next to it, 0x82d9b760). The speed is the MPH tunable
///   of the way's road_type (a, b, dirt, freeway, none, shortcut, anything
///   else Unknown) times 0.44704 (0x82005aec) in metres per second
///   (0x82da1ae0, 0x82da19f8, 0x82da2800); a shortcut when shortcuts are
///   not allowed has a speed of 0.01 (0x82000ccc), and a way with no
///   road_type the Unknown speed. A segment run against the order of a way
///   tagged oneway costs OneWayMultiplier times as much.
/// - The first segment out of the start adds TurnDelaySeconds when it
///   heads against the car's facing: the search keeps the facing it was
///   started with (0x82a4de0c passes the vehicle's, 0x82d9bc90) and gives it
///   to the first segment's cost only.
/// - The estimate is the straight distance to the goal over the Unknown
///   speed (0x82da18b0). The open list is a heap on cost plus estimate
///   (0x82d9c328); a successor whose state is already open or closed with
///   no greater cost is dropped, otherwise it replaces it.
/// - The route is the nodes from the start to the goal, the nodes along
///   each way between the steps' nodes included (0x82d9efbc).
///
/// Inferred: the sign test on the first segment is the facing's dot
/// product with the segment (the vector instructions at 0x82da2a74 were
/// not decoded); the facing is the car's forward axis (the vehicle
/// source's virtual at +12 asks for axis 2, 0x8251c978). Every way is
/// allowed: the cost policy refuses ways only in its demo and first-time
/// modes (0x82da1bb0), and the sat-nav's planner is taken to run in the
/// ordinary one (0x82a54060 passes 0).
class RouteSearch {
public:
    /// Keeps a reference to `nav`, which must outlive the search.
    RouteSearch(const NavFile& nav, const RouteTuning& tuning);

    /// The nodes of the cheapest route from `start` to `goal`, both
    /// included; empty when the goal cannot be reached. `forward` is the
    /// car's facing (zero for none); `shortcutsAllowed` whether shortcut
    /// ways take their own speed this search (0x82da1980 draws it against
    /// ShortcutProbability).
    std::vector<std::uint32_t> find(
        std::uint32_t start, std::uint32_t goal, const glm::vec3& forward, bool shortcutsAllowed) const;

    /// A way's speed in metres per second.
    float speed(std::uint32_t way, bool shortcutsAllowed) const;
    /// The cost in seconds of the segment from node `from` to its
    /// neighbour `to`.
    float segmentCost(std::uint32_t from, std::uint32_t to, const glm::vec3& forward, bool shortcutsAllowed) const;

private:
    const NavFile& m_nav;
    RouteTuning m_tuning;
};

/// The player's navigation: the track's road network, the car's place on
/// it and the roads discovered, updated each frame the world runs, as the
/// game's CNavigationComponent does (0x826c4bf8 sets it up, 0x826c21d8
/// updates the coverage every frame), and the sat-nav route to the
/// waypoint set on the map (the route planner of 0x82a55e10, updated by
/// 0x82a549c0).
///
/// Inferred: the player's network position uses NavFilter::NoShortcut. The
/// entry (0x8252d610) chooses it unless the session's type byte is 7, which
/// picks NoShortcutOrNone; which session that is was not identified, and
/// free roam and races are taken not to be it.
///
/// The route, verified against the planner: a waypoint becomes the node
/// nearest to it that NavFilter::NoShortcutOrNone accepts (the map
/// overlay's 0x828af1b8); the search runs from the car's node to it with
/// the car's facing (0x82a4ddc8) once the car has a node; a search that
/// fails clears the waypoint (0x82a54998). While a route is held the car's
/// node is looked for along it from the car's last place on it onwards,
/// which becomes its place when found (0x82a54b64); when it is not found
/// and the car is more than 100 m from its last place (a squared distance
/// above 10000, 0x82000d00) the route is searched again from the car
/// (0x82a52ae8); when the car's node is the waypoint's the car has arrived
/// and the waypoint is cleared (0x82a540d0).
///
/// Differences: the game steps the search at most 100 times a frame
/// (0x82a54a18) and so may take a few frames; here it runs to its end at
/// once, with the same result. The map's markers carry no height, so the
/// waypoint's node is the nearest on the ground plane (the game measures
/// from the marker's position in three dimensions).
class Navigation {
public:
    /// Throws DataError when the network or the tunables cannot be read.
    Navigation(const Vfs& vfs, const std::string& track);
    ~Navigation();
    Navigation(const Navigation&) = delete;
    Navigation& operator=(const Navigation&) = delete;

    /// One frame with the player's car at `carPosition` facing along
    /// `carForward`.
    void update(const glm::vec3& carPosition, const glm::vec3& carForward);

    /// Sets the waypoint (the map's SET ROUTE) and searches the route to it
    /// from the car's last place, or clears both (CLEAR ROUTE).
    void setDestination(std::optional<glm::vec2> waypoint);
    /// The waypoint's node; empty when there is no waypoint (none was set,
    /// the car arrived or the search failed).
    std::optional<std::uint32_t> destination() const { return m_goal; }
    /// The route's nodes from the car's node when it was searched to the
    /// waypoint's node; empty while there is none.
    const std::vector<std::uint32_t>& routeNodes() const { return m_route; }
    /// The route's nodes as ground points (world x and z).
    std::vector<glm::vec2> route() const;
    /// The index in the route of the car's last place on it.
    std::size_t routeProgress() const { return m_progress; }
    /// Metres left to the waypoint (the planner's +624, which the HUD's
    /// sat-nav readout shows, 0x8266cf20): the distance from the car to the
    /// route's node after its place, plus the lengths of the route's
    /// segments from there to the waypoint's node (0x82a4d4a8). It is worked
    /// out whenever the car's node is found along the route (0x82a54cbc)
    /// and kept otherwise; 0 from a new search until then (0x82a52b10) and
    /// while there is no route. Verified: which points it sums. Inferred:
    /// that the lengths are three-dimensional (the vector instructions were
    /// not decoded; the fourth component is set to 0 before them) and that
    /// the car's place is its body origin.
    float routeDistance() const { return m_routeDistance; }
    /// routeDistance() as first worked out after a search (the planner's
    /// +620, set only while it is 0), which the map's route reveal is timed
    /// by (0x828b6f0c).
    float routeLength() const { return m_routeLength; }

    const NavFile& network() const { return *m_nav; }
    const NavLocator& locator() const { return *m_locator; }
    const MapCoverage& coverage() const { return *m_coverage; }
    /// For restoring the roads driven from the profile.
    MapCoverage& coverage() { return *m_coverage; }
    std::optional<std::uint32_t> playerNode() const { return m_player->node(); }

private:
    /// Searches the route from the car's node; clears the waypoint when
    /// there is no route.
    void plan();
    void follow();
    /// 0x82a4d4a8 from the car's place on the route.
    float remainingDistance() const;

    std::unique_ptr<NavFile> m_nav;
    std::unique_ptr<NavLocator> m_locator;
    std::unique_ptr<MapCoverage> m_coverage;
    std::unique_ptr<NavPosition> m_player;
    std::unique_ptr<RouteSearch> m_search;
    RouteTuning m_routeTuning;
    std::mt19937 m_random;
    glm::vec3 m_carPosition{0.0F};
    glm::vec3 m_carForward{0.0F};
    std::optional<std::uint32_t> m_goal;
    std::vector<std::uint32_t> m_route;
    std::size_t m_progress = 0;
    float m_routeDistance = 0.0F;
    float m_routeLength = 0.0F;
};

} // namespace fh::ui
