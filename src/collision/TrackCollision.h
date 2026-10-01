#pragma once

#include "CollisionIndex.h"
#include "FizTile.h"
#include "SurfaceTable.h"

#include "base/Bytes.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::collision {

/// Which triangles a query sees.
struct QueryFilter {
    /// TriangleFlags bits a triangle must all have. The default leaves out
    /// camera-only geometry.
    std::uint8_t requiredFlags = TriangleFlags::kSolid;
    /// The race route being driven, whose barriers are then present; -1
    /// (free roam) leaves out every route's barriers, kAllRoutes keeps all.
    int activeRoute = -1;
};

struct RayHit {
    glm::vec3 point{0.0F};
    /// The triangle's normal, turned to face the ray's origin.
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float distance = 0.0F;
    std::uint8_t surface = 0;
    std::uint8_t flags = 0;
    /// Whether the ray hit the triangle's outward side.
    bool frontFace = true;
    /// Index into CollisionIndex::tiles() and into the tile's triangles.
    std::size_t tile = 0;
    std::uint16_t triangle = 0;
};

/// Where a sphere overlaps a collision triangle.
struct SphereContact {
    /// The closest point of the triangle to the sphere's centre.
    glm::vec3 point{0.0F};
    /// Direction to push the sphere out: the face normal when the centre
    /// projects inside the triangle, otherwise from the point towards the
    /// centre.
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    /// How far the sphere reaches past the surface along `normal`.
    float depth = 0.0F;
    std::uint8_t surface = 0;
    std::uint8_t flags = 0;
};

/// A track's collision: the `.col` index, its `.fiz` tiles (cached on demand) and
/// the surface table, with ray and sphere queries in world coordinates.
/// Queries load missing tiles on the calling thread; loadAround() keeps that off a
/// frame. Tiles that fail to decode are logged once, then ignored. Thread-safe.
class TrackCollision {
public:
    /// Returns the bytes of the tile with the given id.
    using TileSource = std::function<Bytes(std::uint32_t id)>;

    TrackCollision(CollisionIndex index, SurfaceTable surfaces, TileSource source);
    ~TrackCollision();
    TrackCollision(const TrackCollision&) = delete;
    TrackCollision& operator=(const TrackCollision&) = delete;

    /// Opens a track (a folder of media/tracks) for `ribbon`; tiles are read when
    /// first needed. Throws DataError if a file is missing or unreadable.
    static std::unique_ptr<TrackCollision> open(const Vfs& vfs, std::string_view track, int ribbon = 0);

    const CollisionIndex& index() const { return m_index; }
    const SurfaceTable& surfaces() const { return m_surfaces; }

    /// The tile at `tileIndex` (into index().tiles()), loading it if needed.
    /// Throws DataError if it cannot be read or decoded, also on later calls.
    std::shared_ptr<const FizTile> tile(std::size_t tileIndex) const;
    /// As tile(), but nullptr if it cannot be read.
    std::shared_ptr<const FizTile> tryTile(std::size_t tileIndex) const;
    /// The error of a tile that failed to load, if it did.
    std::optional<std::string> tileError(std::size_t tileIndex) const;

    /// Loads every tile whose bounds come within `radius` of the XZ point
    /// `centre` (x = world X, y = world Z), on all hardware threads. Returns
    /// how many of them are resident.
    std::size_t loadAround(glm::vec2 centre, float radius) const;
    /// loadAround() on a thread of its own. The TrackCollision must outlive
    /// the returned future's completion.
    std::future<std::size_t> loadAroundAsync(glm::vec2 centre, float radius) const;
    /// Drops the cached tiles whose bounds are farther than `radius` from
    /// `centre` in X or Z.
    void evictOutside(glm::vec2 centre, float radius);
    void evictAll();
    std::size_t residentCount() const;

    /// The nearest triangle along the ray from `origin` in `direction` (need
    /// not be unit length) within `maxDistance`, hit from either side.
    std::optional<RayHit> raycast(
        const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryFilter& filter = {}) const;
    /// The triangles within `radius` of `centre`, each once, deepest first.
    /// One-sided: a centre behind a face is pushed out in front, behind and beside it gives none.
    std::vector<SphereContact> sphereContacts(
        const glm::vec3& centre, float radius, const QueryFilter& filter = {}) const;
    /// Every surface the vertical line through (x, z) crosses, highest
    /// first; for bridges over roads and the like.
    std::vector<RayHit> verticalHits(float x, float z, const QueryFilter& filter = {}) const;
    /// The highest surface at or below `point` within `maxDrop`.
    std::optional<RayHit> groundBelow(const glm::vec3& point, float maxDrop = 1000.0F,
        const QueryFilter& filter = {}) const;

    /// Calls `visit(tileIndex, tile, triangleIndex)` once for each distinct
    /// triangle whose XZ bounds overlap the rectangle [lo, hi] (triangles
    /// shared by neighbouring tiles are reported by one of them).
    void forEachTriangle(glm::vec2 lo, glm::vec2 hi, const QueryFilter& filter,
        const std::function<void(std::size_t, const FizTile&, std::uint16_t)>& visit) const;

private:
    struct Candidate {
        std::size_t index = 0;
        std::shared_ptr<const FizTile> tile;
    };

    /// Loaded tiles among `indices`, in the same order.
    std::vector<Candidate> resident(const std::vector<std::size_t>& indices) const;
    /// Whether tile `tileIndex` reports geometry at world XZ `point`: the tile of the
    /// point's cell, or any of `candidates` where no tile has it (caller deduplicates).
    bool owns(std::size_t tileIndex, glm::vec2 point, const std::vector<Candidate>& candidates) const;
    void rayTile(const Candidate& candidate, const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        float fromDistance, float toDistance, const QueryFilter& filter, std::optional<RayHit>& best) const;

    CollisionIndex m_index;
    SurfaceTable m_surfaces;
    TileSource m_source;

    mutable std::mutex m_mutex;
    mutable std::map<std::size_t, std::shared_future<std::shared_ptr<const FizTile>>> m_cache;
    mutable std::map<std::size_t, std::string> m_failed;
};

} // namespace openhorizon::collision
