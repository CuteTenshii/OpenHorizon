#include "WorldCollision.h"

#include "base/Log.h"
#include "collision/SurfaceTable.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace openhorizon::physics {

namespace {

constexpr float kTileSize = 500.0F;
constexpr float kCellSize = 8.0F;
/// surfaceTypes.xml categories of movable things: GenCollObj (loose car
/// parts, brake markers) and Smashable.
constexpr std::uint8_t kLooseObjectCategory = 8;
constexpr std::uint8_t kSmashableCategory = 12;
/// Scattered copies at least this tall are trees and get a trunk.
constexpr float kTreeHeight = 4.0F;
/// Hysteresis beyond the load radius, so driving along a tile edge does not
/// reload it repeatedly.
constexpr float kDropMargin = 250.0F;

/// Möller–Trumbore, accepting hits from either side.
bool intersect(const glm::vec3& origin, const glm::vec3& direction, const glm::vec3& a, const glm::vec3& edge1,
    const glm::vec3& edge2, float& t)
{
    const glm::vec3 p = glm::cross(direction, edge2);
    const float det = glm::dot(edge1, p);
    if (std::abs(det) < 1e-9F) {
        return false;
    }
    const float inverse = 1.0F / det;
    const glm::vec3 s = origin - a;
    const float u = glm::dot(s, p) * inverse;
    if (u < 0.0F || u > 1.0F) {
        return false;
    }
    const glm::vec3 q = glm::cross(s, edge1);
    const float v = glm::dot(direction, q) * inverse;
    if (v < 0.0F || u + v > 1.0F) {
        return false;
    }
    t = glm::dot(edge2, q) * inverse;
    return t >= 0.0F;
}

/// Closest point on triangle abc to p (Ericson, Real-Time Collision
/// Detection, 5.1.5).
glm::vec3 closestPoint(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return a;
    }
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return b;
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return a + ab * (d1 / (d1 - d3));
    }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return c;
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return a + ac * (d2 / (d2 - d6));
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        return b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6)));
    }
    const float denominator = 1.0F / (va + vb + vc);
    return a + ab * (vb * denominator) + ac * (vc * denominator);
}

} // namespace

WorldCollision::WorldCollision(const world::TrackWorld& world, JobPool& jobs, const Vfs& vfs, std::string_view track)
    : m_world(world)
    , m_jobs(jobs)
    , m_grid(world.index, kTileSize)
    , m_alive(std::make_shared<std::atomic<bool>>(true))
{
    try {
        m_fiz = collision::TrackCollision::open(vfs, track);
        logInfo("collision: {} mesh tiles, {} surface types", m_fiz->index().tiles().size(),
            m_fiz->surfaces().surfaces().size());
    } catch (const std::exception& e) {
        logWarning("collision: no collision meshes for {} ({}); using render geometry", track, e.what());
    }
}

WorldCollision::~WorldCollision()
{
    // Tile jobs write into m_results; wait out any that are running.
    m_alive->store(false);
    m_jobs.clearAndWait();
    if (m_fizLoading.valid()) {
        m_fizLoading.wait();
    }
}

std::uint64_t WorldCollision::trunkCellKey(int x, int z)
{
    return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(z);
}

std::shared_ptr<const WorldCollision::Tile> WorldCollision::buildTile(
    const world::TileMesh& mesh, glm::vec2 origin, std::vector<Trunk> trunks)
{
    auto tile = std::make_shared<Tile>();
    tile->cellSize = kCellSize;
    tile->trunks = std::move(trunks);
    for (std::uint32_t i = 0; i < tile->trunks.size(); ++i) {
        const Trunk& trunk = tile->trunks[i];
        const int x0 = static_cast<int>(std::floor((trunk.centre.x - trunk.radius) / kCellSize));
        const int x1 = static_cast<int>(std::floor((trunk.centre.x + trunk.radius) / kCellSize));
        const int z0 = static_cast<int>(std::floor((trunk.centre.y - trunk.radius) / kCellSize));
        const int z1 = static_cast<int>(std::floor((trunk.centre.y + trunk.radius) / kCellSize));
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                tile->trunkCells[trunkCellKey(x, z)].push_back(i);
            }
        }
    }
    const float* v = mesh.vertices.data();
    const auto position = [v](std::uint32_t index) {
        const float* p = v + static_cast<std::size_t>(index) * world::TileMesh::kFloatsPerVertex;
        return glm::vec3(p[0], p[1], p[2]);
    };
    glm::vec2 lo(std::numeric_limits<float>::max());
    glm::vec2 hi(std::numeric_limits<float>::lowest());
    for (const world::TileMesh::Batch& batch : mesh.batches) {
        if (batch.shading == world::TileMesh::Shading::Water || batch.backdrop) {
            continue;
        }
        for (std::uint32_t i = batch.firstIndex; i + 2 < batch.firstIndex + batch.indexCount; i += 3) {
            const glm::vec3 a = position(mesh.indices[i]);
            const glm::vec3 b = position(mesh.indices[i + 1]);
            const glm::vec3 c = position(mesh.indices[i + 2]);
            const glm::vec3 n = glm::cross(b - a, c - a);
            const float area = glm::length(n);
            if (area < 1e-6F) {
                continue;
            }
            tile->triangles.push_back({a, b - a, c - a, n / area});
            for (const glm::vec3& p : {a, b, c}) {
                lo = glm::min(lo, glm::vec2(p.x, p.z));
                hi = glm::max(hi, glm::vec2(p.x, p.z));
            }
        }
    }
    if (tile->triangles.empty()) {
        tile->origin = origin;
        tile->cellsX = tile->cellsZ = 1;
        tile->cellStart.assign(2, 0);
        return tile;
    }
    tile->origin = lo;
    tile->cellsX = std::max(1, static_cast<int>(std::ceil((hi.x - lo.x) / kCellSize)));
    tile->cellsZ = std::max(1, static_cast<int>(std::ceil((hi.y - lo.y) / kCellSize)));
    const std::size_t cellCount = static_cast<std::size_t>(tile->cellsX) * static_cast<std::size_t>(tile->cellsZ);

    // Counting sort of triangle references into cells.
    const auto cellRange = [&](const Triangle& t, int& x0, int& z0, int& x1, int& z1) {
        const glm::vec3 b = t.a + t.edge1;
        const glm::vec3 c = t.a + t.edge2;
        const float minX = std::min({t.a.x, b.x, c.x});
        const float maxX = std::max({t.a.x, b.x, c.x});
        const float minZ = std::min({t.a.z, b.z, c.z});
        const float maxZ = std::max({t.a.z, b.z, c.z});
        x0 = std::clamp(static_cast<int>((minX - lo.x) / kCellSize), 0, tile->cellsX - 1);
        x1 = std::clamp(static_cast<int>((maxX - lo.x) / kCellSize), 0, tile->cellsX - 1);
        z0 = std::clamp(static_cast<int>((minZ - lo.y) / kCellSize), 0, tile->cellsZ - 1);
        z1 = std::clamp(static_cast<int>((maxZ - lo.y) / kCellSize), 0, tile->cellsZ - 1);
    };
    std::vector<std::uint32_t> counts(cellCount + 1, 0);
    for (const Triangle& t : tile->triangles) {
        int x0 = 0;
        int z0 = 0;
        int x1 = 0;
        int z1 = 0;
        cellRange(t, x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                ++counts[static_cast<std::size_t>(z) * static_cast<std::size_t>(tile->cellsX)
                    + static_cast<std::size_t>(x)];
            }
        }
    }
    tile->cellStart.assign(cellCount + 1, 0);
    for (std::size_t i = 0; i < cellCount; ++i) {
        tile->cellStart[i + 1] = tile->cellStart[i] + counts[i];
    }
    tile->cellTriangles.resize(tile->cellStart[cellCount]);
    std::vector<std::uint32_t> fill(tile->cellStart.begin(), tile->cellStart.end() - 1);
    for (std::uint32_t index = 0; index < tile->triangles.size(); ++index) {
        int x0 = 0;
        int z0 = 0;
        int x1 = 0;
        int z1 = 0;
        cellRange(tile->triangles[index], x0, z0, x1, z1);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const std::size_t cell = static_cast<std::size_t>(z) * static_cast<std::size_t>(tile->cellsX)
                    + static_cast<std::size_t>(x);
                tile->cellTriangles[fill[cell]++] = index;
            }
        }
    }
    return tile;
}

void WorldCollision::update(const glm::vec3& focus, float radius)
{
    if (m_fiz) {
        // One batch of collision mesh tiles loading at a time.
        if (!m_fizLoading.valid()
            || m_fizLoading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            if (m_fizLoading.valid()) {
                m_fizLoading.get();
            }
            m_fiz->evictOutside(glm::vec2(focus.x, focus.z), radius + kDropMargin);
            m_fizLoading = m_fiz->loadAroundAsync(glm::vec2(focus.x, focus.z), radius);
        }
    }
    {
        const std::lock_guard lock(m_resultMutex);
        for (auto& [index, tile] : m_results) {
            m_tiles[index] = std::move(tile);
            m_building.erase(index);
        }
        m_results.clear();
    }
    const auto& tiles = m_grid.tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const float distance = m_grid.distanceTo(tiles[i], focus.x, focus.z);
        if (distance > radius + kDropMargin) {
            m_tiles.erase(i);
            continue;
        }
        if (distance > radius || m_tiles.contains(i) || m_building.contains(i)) {
            continue;
        }
        m_building[i] = true;
        std::vector<std::uint32_t> chunks;
        std::vector<Trunk> trunks;
        for (std::uint32_t chunk : m_grid.chunksAt(tiles[i], 0.0F)) {
            const world::WorldChunk& c = m_world.index.chunks()[chunk];
            if (c.backdrop || (chunk < m_excluded.size() && m_excluded[chunk])) {
                continue;
            }
            if (!c.scattered) {
                chunks.push_back(chunk);
                continue;
            }
            const float height = c.boundsMax.y - c.placement.position.y;
            if (height < kTreeHeight) {
                continue;
            }
            // A trunk is a small fraction of the crown's spread.
            const float spread = std::min(c.boundsMax.x - c.boundsMin.x, c.boundsMax.z - c.boundsMin.z);
            Trunk trunk;
            trunk.centre = glm::vec2(c.placement.position.x, c.placement.position.z);
            trunk.radius = std::clamp(spread * 0.05F, 0.15F, 0.6F);
            trunk.bottom = c.placement.position.y - 1.0F;
            trunk.top = c.placement.position.y + std::min(height, 6.0F);
            trunks.push_back(trunk);
        }
        const glm::vec2 origin(tiles[i].boundsMin.x, tiles[i].boundsMin.z);
        // Collision near the car is needed before anything is drawn there.
        m_jobs.submit(distance - 1000.0F,
            [this, i, origin, chunks = std::move(chunks), trunks = std::move(trunks), alive = m_alive]() mutable {
            if (!alive->load()) {
                return;
            }
            const world::TileMesh mesh = world::buildTileMesh(
                *m_world.archive, m_world.index, chunks, m_world.textures ? &*m_world.textures : nullptr);
            std::shared_ptr<const Tile> tile = buildTile(mesh, origin, std::move(trunks));
            const std::lock_guard lock(m_resultMutex);
            m_results.emplace_back(i, std::move(tile));
        });
    }
}

bool WorldCollision::readyAt(const glm::vec3& point) const
{
    // Big terrain chunks reach over neighbouring tiles, so every tile whose
    // bounds cover the point must be in.
    const auto& tiles = m_grid.tiles();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const auto& tile = tiles[i];
        const bool covers = point.x >= tile.boundsMin.x && point.x <= tile.boundsMax.x && point.z >= tile.boundsMin.z
            && point.z <= tile.boundsMax.z;
        if (covers && !m_tiles.contains(i)) {
            return false;
        }
    }
    return true;
}

std::size_t WorldCollision::triangleCount() const
{
    std::size_t count = 0;
    for (const auto& [index, tile] : m_tiles) {
        count += tile->triangles.size();
    }
    return count;
}

std::size_t WorldCollision::trunkCount() const
{
    std::size_t count = 0;
    for (const auto& [index, tile] : m_tiles) {
        count += tile->trunks.size();
    }
    return count;
}

template <typename Visit> void WorldCollision::forEachTriangleNear(glm::vec3 lo, glm::vec3 hi, Visit&& visit) const
{
    for (const auto& [index, tile] : m_tiles) {
        if (tile->triangles.empty()) {
            continue;
        }
        const float tileMaxX = tile->origin.x + static_cast<float>(tile->cellsX) * tile->cellSize;
        const float tileMaxZ = tile->origin.y + static_cast<float>(tile->cellsZ) * tile->cellSize;
        if (hi.x < tile->origin.x || lo.x > tileMaxX || hi.z < tile->origin.y || lo.z > tileMaxZ) {
            continue;
        }
        const int x0 = std::clamp(static_cast<int>((lo.x - tile->origin.x) / tile->cellSize), 0, tile->cellsX - 1);
        const int x1 = std::clamp(static_cast<int>((hi.x - tile->origin.x) / tile->cellSize), 0, tile->cellsX - 1);
        const int z0 = std::clamp(static_cast<int>((lo.z - tile->origin.y) / tile->cellSize), 0, tile->cellsZ - 1);
        const int z1 = std::clamp(static_cast<int>((hi.z - tile->origin.y) / tile->cellSize), 0, tile->cellsZ - 1);
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const std::size_t cell = static_cast<std::size_t>(z) * static_cast<std::size_t>(tile->cellsX)
                    + static_cast<std::size_t>(x);
                for (std::uint32_t k = tile->cellStart[cell]; k < tile->cellStart[cell + 1]; ++k) {
                    visit(tile->triangles[tile->cellTriangles[k]]);
                }
            }
        }
    }
}

void WorldCollision::fillSurface(int surface, GroundHit& hit) const
{
    hit.surface = surface;
    hit.grip = 1.0F;
    hit.drag = 0.0F;
    if (!m_fiz || surface < 0) {
        return;
    }
    const collision::SurfaceType* type = m_fiz->surfaces().find(static_cast<std::uint8_t>(surface));
    if (!type || !type->hasParameters) {
        return;
    }
    hit.grip = type->frictionScale;
    // Rolling drag as a fiftieth of the velocity-friction coefficient (sand's
    // 5 gives a tenth of the load). Inferred, not from the game's code.
    hit.drag = std::clamp(type->velocityFriction.longitudinalCoeff * 0.02F, 0.0F, 0.25F);
}

bool WorldCollision::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    GroundHit& hit) const
{
    if (m_fiz) {
        if (const std::optional<collision::RayHit> fiz = m_fiz->raycast(origin, direction, maxDistance, m_filter)) {
            hit.point = fiz->point;
            hit.normal = fiz->normal;
            hit.distance = fiz->distance;
            fillSurface(fiz->surface, hit);
            return true;
        }
    }
    const glm::vec3 end = origin + direction * maxDistance;
    float best = maxDistance;
    const Triangle* bestTriangle = nullptr;
    forEachTriangleNear(glm::min(origin, end), glm::max(origin, end), [&](const Triangle& t) {
        float distance = 0.0F;
        if (intersect(origin, direction, t.a, t.edge1, t.edge2, distance) && distance < best) {
            best = distance;
            bestTriangle = &t;
        }
    });
    if (!bestTriangle) {
        return false;
    }
    hit.point = origin + direction * best;
    // Faces are hit from either side; the normal points back at the ray.
    hit.normal = glm::dot(bestTriangle->normal, direction) > 0.0F ? -bestTriangle->normal : bestTriangle->normal;
    hit.distance = best;
    fillSurface(-1, hit);
    return true;
}

void WorldCollision::renderContacts(
    const glm::vec3& centre, float radius, bool wallsOnly, std::vector<SphereContact>& out) const
{
    const glm::vec3 extent(radius);
    forEachTriangleNear(centre - extent, centre + extent, [&](const Triangle& t) {
        // Ground-like faces are left to the collision mesh where it exists;
        // the render ground there can sit a few centimetres off it.
        if (wallsOnly && std::abs(t.normal.y) > 0.7F) {
            return;
        }
        const glm::vec3 p = closestPoint(centre, t.a, t.a + t.edge1, t.a + t.edge2);
        const glm::vec3 offset = centre - p;
        const float distance2 = glm::dot(offset, offset);
        if (distance2 >= radius * radius) {
            return;
        }
        const float distance = std::sqrt(distance2);
        SphereContact contact;
        contact.point = p;
        if (distance > 1e-5F) {
            contact.normal = offset / distance;
        } else {
            contact.normal = t.normal;
        }
        contact.depth = radius - distance;
        out.push_back(contact);
    });
}

void WorldCollision::trunkContacts(const glm::vec3& centre, float radius, std::vector<SphereContact>& out) const
{
    const int x0 = static_cast<int>(std::floor((centre.x - radius) / kCellSize));
    const int x1 = static_cast<int>(std::floor((centre.x + radius) / kCellSize));
    const int z0 = static_cast<int>(std::floor((centre.z - radius) / kCellSize));
    const int z1 = static_cast<int>(std::floor((centre.z + radius) / kCellSize));
    for (const auto& [index, tile] : m_tiles) {
        if (tile->trunks.empty()) {
            continue;
        }
        for (int z = z0; z <= z1; ++z) {
            for (int x = x0; x <= x1; ++x) {
                const auto cell = tile->trunkCells.find(trunkCellKey(x, z));
                if (cell == tile->trunkCells.end()) {
                    continue;
                }
                for (std::uint32_t k : cell->second) {
                    const Trunk& trunk = tile->trunks[k];
                    if (centre.y + radius < trunk.bottom || centre.y - radius > trunk.top) {
                        continue;
                    }
                    const glm::vec2 offset = glm::vec2(centre.x, centre.z) - trunk.centre;
                    const float distance = glm::length(offset);
                    const float reach = radius + trunk.radius;
                    if (distance >= reach) {
                        continue;
                    }
                    SphereContact contact;
                    const glm::vec2 direction = distance > 1e-4F ? offset / distance : glm::vec2(1.0F, 0.0F);
                    contact.normal = glm::vec3(direction.x, 0.0F, direction.y);
                    contact.point = glm::vec3(trunk.centre.x + direction.x * trunk.radius, centre.y,
                        trunk.centre.y + direction.y * trunk.radius);
                    contact.depth = reach - distance;
                    out.push_back(contact);
                }
            }
        }
    }
}

void WorldCollision::sphereContacts(const glm::vec3& centre, float radius, std::vector<SphereContact>& out) const
{
    const std::size_t first = out.size();
    bool meshCovers = false;
    if (m_fiz) {
        const collision::SurfaceTable& surfaces = m_fiz->surfaces();
        for (const collision::SphereContact& c : m_fiz->sphereContacts(centre, radius, m_filter)) {
            // Loose objects and smashables are knocked over, not driven into.
            const collision::SurfaceType* type = surfaces.find(c.surface);
            if (type && (type->category == kLooseObjectCategory || type->category == kSmashableCategory)) {
                continue;
            }
            out.push_back({c.point, c.normal, c.depth, c.surface});
        }
        // Inside the drivable area render geometry only adds the props.
        meshCovers = m_fiz->groundBelow(centre + glm::vec3(0.0F, radius, 0.0F), 20.0F, m_filter).has_value();
    }
    renderContacts(centre, radius, meshCovers, out);
    trunkContacts(centre, radius, out);
    std::sort(out.begin() + static_cast<std::ptrdiff_t>(first), out.end(),
        [](const SphereContact& a, const SphereContact& b) { return a.depth > b.depth; });
}

std::optional<float> WorldCollision::groundHeight(float x, float z, float fromY) const
{
    GroundHit hit;
    if (raycast({x, fromY, z}, {0.0F, -1.0F, 0.0F}, fromY + 500.0F, hit)) {
        return hit.point.y;
    }
    return std::nullopt;
}

} // namespace openhorizon::physics
