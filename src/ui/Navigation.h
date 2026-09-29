#pragma once

#include "MapCoverage.h"
#include "Minimap.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <optional>
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

    bool acceptsNode(std::uint32_t node, NavFilter filter) const;
    bool acceptsWay(std::uint32_t way, NavFilter filter) const;

    const NavFile& network() const { return m_nav; }

private:
    struct Cell {
        glm::vec3 centre{0.0F};
        std::vector<std::uint32_t> nodes;
    };

    std::optional<std::uint32_t> search(const Cell& cell, const glm::vec3& position, NavFilter filter) const;
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

/// The player's navigation: the track's road network, the car's place on
/// it and the roads discovered, updated each frame the world runs, as the
/// game's CNavigationComponent does (0x826c4bf8 sets it up, 0x826c21d8
/// updates the coverage every frame).
///
/// Inferred: the player's network position uses NavFilter::NoShortcut. The
/// entry (0x8252d610) chooses it unless the session's type byte is 7, which
/// picks NoShortcutOrNone; which session that is was not identified, and
/// free roam and races are taken not to be it.
class Navigation {
public:
    /// Throws DataError when the network or the tunables cannot be read.
    Navigation(const Vfs& vfs, const std::string& track);
    ~Navigation();
    Navigation(const Navigation&) = delete;
    Navigation& operator=(const Navigation&) = delete;

    /// One frame with the player's car at `carPosition`.
    void update(const glm::vec3& carPosition);

    const NavFile& network() const { return *m_nav; }
    const MapCoverage& coverage() const { return *m_coverage; }
    std::optional<std::uint32_t> playerNode() const { return m_player->node(); }

private:
    std::unique_ptr<NavFile> m_nav;
    std::unique_ptr<NavLocator> m_locator;
    std::unique_ptr<MapCoverage> m_coverage;
    std::unique_ptr<NavPosition> m_player;
};

} // namespace fh::ui
