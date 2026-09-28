#pragma once

#include "base/JobPool.h"
#include "car/CarPhysics.h"
#include "collision/TrackCollision.h"
#include "world/TrackWorld.h"
#include "world/WorldTiles.h"

#include <glm/glm.hpp>

#include <atomic>
#include <future>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::physics {

/// A point where a sphere overlaps the world.
struct SphereContact {
    glm::vec3 point{0.0F};
    /// Out of the surface, towards the sphere's centre.
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float depth = 0.0F;
    /// Surface type (surfaceTypes.xml), or -1 for render geometry.
    int surface = -1;
};

/// The world's solid surfaces around a moving point, for the cars.
///
/// The track's own collision meshes (`.fiz` tiles, see collision::
/// TrackCollision) come first: they carry a surface type per triangle, so
/// grass, dirt and tarmac grip differently, and they hold the walls and
/// invisible barriers that fence the drivable area. They cover only that
/// area, and not the props standing in it, so the render geometry fills in:
/// the finest level of detail of the terrain, roads and placed props in the
/// 500 m tiles near the focus, built on worker threads into a grid of small
/// cells. Inside the drivable area only its steep faces (walls, signs,
/// buildings) count; outside it, all of it does.
///
/// Procedural copies (trees, bushes, rocks) are left out of the render
/// geometry, since their models include foliage a car would otherwise hit in
/// mid-air; instead each tall copy (a tree) gets a vertical cylinder for its
/// trunk, and low ones (bushes, scrub) stay drive-through.
class WorldCollision final : public GroundQuery {
public:
    /// Opens the track's collision meshes too; without them (a track that
    /// lacks its .col index) only render geometry is used.
    WorldCollision(const world::TrackWorld& world, JobPool& jobs, const Vfs& vfs, std::string_view track);
    ~WorldCollision() override;
    WorldCollision(const WorldCollision&) = delete;
    WorldCollision& operator=(const WorldCollision&) = delete;

    /// Loads tiles within `radius` of `focus` and drops far ones. Call once
    /// per frame from the game thread.
    void update(const glm::vec3& focus, float radius = 350.0F);
    /// True once the tile under `point` has been built.
    bool readyAt(const glm::vec3& point) const;
    /// The race route being driven, whose barriers are then solid; -1 in
    /// free roam.
    void setActiveRoute(int route) { m_filter.activeRoute = route; }
    const collision::SurfaceTable* surfaces() const { return m_fiz ? &m_fiz->surfaces() : nullptr; }

    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, GroundHit& hit) const override;
    /// Appends the surfaces a sphere overlaps, deepest first.
    void sphereContacts(const glm::vec3& centre, float radius, std::vector<SphereContact>& out) const;
    /// Height of the ground below (or above, within 500 m) a point, if any.
    std::optional<float> groundHeight(float x, float z, float fromY = 2000.0F) const;

    std::size_t triangleCount() const;
    std::size_t trunkCount() const;

private:
    struct Triangle {
        glm::vec3 a;
        glm::vec3 edge1;
        glm::vec3 edge2;
        glm::vec3 normal;
    };
    /// One tile's triangles in a grid of square cells.
    /// A tree trunk: a vertical cylinder from `bottom` up to `top`.
    struct Trunk {
        glm::vec2 centre{0.0F};
        float radius = 0.3F;
        float bottom = 0.0F;
        float top = 0.0F;
    };
    struct Tile {
        glm::vec2 origin{0.0F};
        int cellsX = 0;
        int cellsZ = 0;
        float cellSize = 8.0F;
        std::vector<Triangle> triangles;
        /// Per cell, the start of its run in `cellTriangles` (size cells+1).
        std::vector<std::uint32_t> cellStart;
        std::vector<std::uint32_t> cellTriangles;
        std::vector<Trunk> trunks;
        /// Trunks by 8 m cell of the world grid, keyed by trunkCellKey().
        std::unordered_map<std::uint64_t, std::vector<std::uint32_t>> trunkCells;
    };

    static std::shared_ptr<const Tile> buildTile(
        const world::TileMesh& mesh, glm::vec2 origin, std::vector<Trunk> trunks);
    static std::uint64_t trunkCellKey(int x, int z);
    template <typename Visit> void forEachTriangleNear(glm::vec3 lo, glm::vec3 hi, Visit&& visit) const;
    void renderContacts(const glm::vec3& centre, float radius, bool wallsOnly, std::vector<SphereContact>& out) const;
    void trunkContacts(const glm::vec3& centre, float radius, std::vector<SphereContact>& out) const;
    void fillSurface(int surface, GroundHit& hit) const;

    const world::TrackWorld& m_world;
    JobPool& m_jobs;
    world::WorldTileGrid m_grid;
    std::unordered_map<std::size_t, std::shared_ptr<const Tile>> m_tiles;
    std::unordered_map<std::size_t, bool> m_building;

    std::mutex m_resultMutex;
    std::vector<std::pair<std::size_t, std::shared_ptr<const Tile>>> m_results;
    std::shared_ptr<std::atomic<bool>> m_alive;

    std::unique_ptr<collision::TrackCollision> m_fiz;
    collision::QueryFilter m_filter;
    std::future<std::size_t> m_fizLoading;
};

} // namespace fh::physics
