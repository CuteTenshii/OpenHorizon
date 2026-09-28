#include "TrackCollision.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"
#include "world/Archive.h"

#include <lzx/zip.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_map>

namespace fh::collision {

namespace {

/// Slack for points on cell edges and for cracks between adjacent triangles.
constexpr float kEdgeSlack = 1e-3F;
constexpr float kBarycentricSlack = 1e-6F;
/// Contacts closer than this are the same contact seen from two tiles.
constexpr float kSameContact = 1e-4F;
/// Top of the vertical line verticalHits() scans; well above any terrain.
constexpr float kVerticalTop = 20000.0F;

glm::vec2 xz(const glm::vec3& v)
{
    return {v.x, v.z};
}

/// Walks the cells of a grid (origin, cell size, columns, rows) crossed by
/// the XZ segment from `a` to `b`, in order, calling
/// `visit(column, row, tEnter, tExit)` with the parameters (0 at `a`, 1 at
/// `b`) where the segment enters and leaves the cell, until it returns
/// false. A segment of zero length visits the cell holding its point.
template <typename Visit>
void walkGrid(glm::vec2 origin, glm::vec2 cellSize, std::uint32_t columns, std::uint32_t rows, glm::vec2 a,
    glm::vec2 b, Visit&& visit)
{
    const glm::vec2 d = b - a;
    const glm::vec2 lo = origin;
    const glm::vec2 hi = origin + cellSize * glm::vec2(static_cast<float>(columns), static_cast<float>(rows));
    float tMin = 0.0F;
    float tMax = 1.0F;
    for (int axis = 0; axis < 2; ++axis) {
        if (std::abs(d[axis]) < 1e-12F) {
            if (a[axis] < lo[axis] || a[axis] > hi[axis]) {
                return;
            }
            continue;
        }
        float t0 = (lo[axis] - a[axis]) / d[axis];
        float t1 = (hi[axis] - a[axis]) / d[axis];
        if (t0 > t1) {
            std::swap(t0, t1);
        }
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) {
            return;
        }
    }

    const glm::vec2 start = a + d * tMin;
    std::array<std::int64_t, 2> cell{};
    std::array<std::int64_t, 2> step{};
    std::array<float, 2> tNext{};
    std::array<float, 2> tDelta{};
    const std::array<std::int64_t, 2> count{columns, rows};
    for (int axis = 0; axis < 2; ++axis) {
        const float position = (start[axis] - origin[axis]) / cellSize[axis];
        cell[axis] = std::clamp<std::int64_t>(static_cast<std::int64_t>(std::floor(position)), 0, count[axis] - 1);
        if (std::abs(d[axis]) < 1e-12F) {
            step[axis] = 0;
            tNext[axis] = std::numeric_limits<float>::infinity();
            tDelta[axis] = std::numeric_limits<float>::infinity();
            continue;
        }
        step[axis] = d[axis] > 0.0F ? 1 : -1;
        const float boundary
            = origin[axis] + static_cast<float>(cell[axis] + (step[axis] > 0 ? 1 : 0)) * cellSize[axis];
        tNext[axis] = (boundary - a[axis]) / d[axis];
        tDelta[axis] = cellSize[axis] / std::abs(d[axis]);
    }

    float tEnter = tMin;
    while (true) {
        const int axis = tNext[0] <= tNext[1] ? 0 : 1;
        const float tExit = std::min(tNext[axis], tMax);
        if (!visit(static_cast<std::uint32_t>(cell[0]), static_cast<std::uint32_t>(cell[1]), tEnter, tExit)) {
            return;
        }
        if (tExit >= tMax) {
            return;
        }
        cell[axis] += step[axis];
        if (cell[axis] < 0 || cell[axis] >= count[axis]) {
            return;
        }
        tEnter = tExit;
        tNext[axis] += tDelta[axis];
    }
}

/// Two-sided ray and triangle intersection; the distance along the unit
/// direction, if the ray meets the triangle within [0, maxDistance].
std::optional<float> intersect(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
    const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 e1 = b - a;
    const glm::vec3 e2 = c - a;
    const glm::vec3 p = glm::cross(direction, e2);
    const float det = glm::dot(e1, p);
    if (std::abs(det) < 1e-12F) {
        return std::nullopt;
    }
    const float inverse = 1.0F / det;
    const glm::vec3 s = origin - a;
    const float u = glm::dot(s, p) * inverse;
    if (u < -kBarycentricSlack || u > 1.0F + kBarycentricSlack) {
        return std::nullopt;
    }
    const glm::vec3 q = glm::cross(s, e1);
    const float v = glm::dot(direction, q) * inverse;
    if (v < -kBarycentricSlack || u + v > 1.0F + kBarycentricSlack) {
        return std::nullopt;
    }
    const float t = glm::dot(e2, q) * inverse;
    if (t < 0.0F || t > maxDistance) {
        return std::nullopt;
    }
    return t;
}

/// The point of triangle abc closest to p, and whether it lies inside the
/// face rather than on an edge or corner.
std::pair<glm::vec3, bool> closestPoint(const glm::vec3& p, const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec3 ab = b - a;
    const glm::vec3 ac = c - a;
    const glm::vec3 ap = p - a;
    const float d1 = glm::dot(ab, ap);
    const float d2 = glm::dot(ac, ap);
    if (d1 <= 0.0F && d2 <= 0.0F) {
        return {a, false};
    }
    const glm::vec3 bp = p - b;
    const float d3 = glm::dot(ab, bp);
    const float d4 = glm::dot(ac, bp);
    if (d3 >= 0.0F && d4 <= d3) {
        return {b, false};
    }
    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0F && d1 >= 0.0F && d3 <= 0.0F) {
        return {a + ab * (d1 / (d1 - d3)), false};
    }
    const glm::vec3 cp = p - c;
    const float d5 = glm::dot(ab, cp);
    const float d6 = glm::dot(ac, cp);
    if (d6 >= 0.0F && d5 <= d6) {
        return {c, false};
    }
    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0F && d2 >= 0.0F && d6 <= 0.0F) {
        return {a + ac * (d2 / (d2 - d6)), false};
    }
    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0F && (d4 - d3) >= 0.0F && (d5 - d6) >= 0.0F) {
        return {b + (c - b) * ((d4 - d3) / ((d4 - d3) + (d5 - d6))), false};
    }
    const float denominator = 1.0F / (va + vb + vc);
    return {a + ab * (vb * denominator) + ac * (vc * denominator), true};
}

bool passes(const FizTile& tile, const FizTriangle& triangle, const QueryFilter& filter)
{
    return (triangle.flags & filter.requiredFlags) == filter.requiredFlags
        && tile.present(triangle, filter.activeRoute);
}

/// Grid cells of `tile` overlapping the XZ rectangle [lo, hi], as inclusive
/// column and row ranges; nothing if the rectangle misses the grid.
std::optional<std::array<std::uint32_t, 4>> cellRange(const FizTile& tile, glm::vec2 lo, glm::vec2 hi)
{
    const glm::vec2 first = glm::floor((lo - tile.boundsMin) / tile.cellSize);
    const glm::vec2 last = glm::floor((hi - tile.boundsMin) / tile.cellSize);
    if (last.x < 0.0F || last.y < 0.0F || first.x >= static_cast<float>(tile.cellsX)
        || first.y >= static_cast<float>(tile.cellsZ)) {
        return std::nullopt;
    }
    return std::array<std::uint32_t, 4>{static_cast<std::uint32_t>(std::max(first.x, 0.0F)),
        static_cast<std::uint32_t>(std::max(first.y, 0.0F)),
        static_cast<std::uint32_t>(std::min(last.x, static_cast<float>(tile.cellsX - 1))),
        static_cast<std::uint32_t>(std::min(last.y, static_cast<float>(tile.cellsZ - 1)))};
}

/// Distinct triangles listed in the cells of `tile` overlapping [lo, hi].
std::vector<std::uint16_t> trianglesIn(const FizTile& tile, glm::vec2 lo, glm::vec2 hi)
{
    std::vector<std::uint16_t> result;
    const auto range = cellRange(tile, lo, hi);
    if (!range) {
        return result;
    }
    for (std::uint32_t z = (*range)[1]; z <= (*range)[3]; ++z) {
        for (std::uint32_t x = (*range)[0]; x <= (*range)[2]; ++x) {
            const auto cell = tile.cell(x, z);
            result.insert(result.end(), cell.begin(), cell.end());
        }
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

/// The tile id of an archive entry named "<id>.fiz", in any folder.
std::optional<std::uint32_t> fizId(std::string_view name)
{
    const std::size_t slash = name.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    if (!endsWithNoCase(name, ".fiz")) {
        return std::nullopt;
    }
    name.remove_suffix(4);
    std::uint32_t id = 0;
    const auto [end, error] = std::from_chars(name.data(), name.data() + name.size(), id);
    if (error != std::errc() || end != name.data() + name.size() || name.empty()) {
        return std::nullopt;
    }
    return id;
}

enum class Ownership { Owned, NotOwned, Orphan };

} // namespace

TrackCollision::TrackCollision(CollisionIndex index, SurfaceTable surfaces, TileSource source)
    : m_index(std::move(index))
    , m_surfaces(std::move(surfaces))
    , m_source(std::move(source))
{
}

TrackCollision::~TrackCollision() = default;

std::unique_ptr<TrackCollision> TrackCollision::open(const Vfs& vfs, std::string_view track, int ribbon)
{
    const std::string indexPath = CollisionIndex::pathOf(track, ribbon);
    CollisionIndex index = CollisionIndex::parse(vfs.readOrThrow(indexPath), indexPath);
    SurfaceTable surfaces = SurfaceTable::load(vfs);

    const std::string mountPoint = std::format("media/tracks/{}/bin", track);
    std::shared_ptr<const lzx::ZipArchive> archive = vfs.archiveAt(mountPoint);
    if (!archive) {
        throw DataError("{}: no archive mounted there", mountPoint);
    }
    // bin.zip stores each tile several times with identical contents; the
    // first copy will do.
    auto entryOf = std::make_shared<std::unordered_map<std::uint32_t, std::size_t>>();
    const auto& entries = archive->entries();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (const std::optional<std::uint32_t> id = fizId(entries[i].name)) {
            entryOf->try_emplace(*id, i);
        }
    }
    std::size_t missing = 0;
    for (const CollisionIndex::Tile& tile : index.tiles()) {
        missing += entryOf->contains(tile.id) ? 0 : 1;
    }
    if (missing > 0) {
        logWarning("collision: {} of the {} tiles of {} are not in {}", missing, index.tiles().size(), indexPath,
            mountPoint);
    }
    TileSource source = [archive, entryOf](std::uint32_t id) {
        const auto found = entryOf->find(id);
        if (found == entryOf->end()) {
            throw DataError("{}.fiz: not in the track's archive", id);
        }
        return world::readEntry(*archive, archive->entries()[found->second]);
    };
    return std::make_unique<TrackCollision>(std::move(index), std::move(surfaces), std::move(source));
}

std::shared_ptr<const FizTile> TrackCollision::tile(std::size_t tileIndex) const
{
    if (tileIndex >= m_index.tiles().size()) {
        throw DataError("collision tile index {} out of range ({} tiles)", tileIndex, m_index.tiles().size());
    }
    std::promise<std::shared_ptr<const FizTile>> promise;
    std::shared_future<std::shared_ptr<const FizTile>> future;
    bool loader = false;
    {
        const std::lock_guard lock(m_mutex);
        if (const auto failed = m_failed.find(tileIndex); failed != m_failed.end()) {
            throw DataError("{}", failed->second);
        }
        if (const auto cached = m_cache.find(tileIndex); cached != m_cache.end()) {
            future = cached->second;
        } else {
            future = promise.get_future().share();
            m_cache.emplace(tileIndex, future);
            loader = true;
        }
    }
    if (loader) {
        const std::uint32_t id = m_index.tiles()[tileIndex].id;
        try {
            const Bytes data = m_source(id);
            promise.set_value(std::make_shared<const FizTile>(parseFizTile(data, std::format("{}.fiz", id))));
        } catch (const std::exception& e) {
            {
                const std::lock_guard lock(m_mutex);
                m_cache.erase(tileIndex);
                m_failed.emplace(tileIndex, e.what());
            }
            logWarning("collision: {}", e.what());
            promise.set_exception(std::current_exception());
        }
    }
    return future.get();
}

std::shared_ptr<const FizTile> TrackCollision::tryTile(std::size_t tileIndex) const
{
    try {
        return tile(tileIndex);
    } catch (const std::exception&) {
        return nullptr;
    }
}

std::optional<std::string> TrackCollision::tileError(std::size_t tileIndex) const
{
    const std::lock_guard lock(m_mutex);
    if (const auto failed = m_failed.find(tileIndex); failed != m_failed.end()) {
        return failed->second;
    }
    return std::nullopt;
}

std::size_t TrackCollision::loadAround(glm::vec2 centre, float radius) const
{
    const std::vector<std::size_t> indices
        = m_index.tilesOverlapping(centre - glm::vec2(radius), centre + glm::vec2(radius));
    std::atomic<std::size_t> loaded{0};
    world::parallelFor(indices.size(), [&](std::size_t i) {
        if (tryTile(indices[i])) {
            loaded.fetch_add(1, std::memory_order_relaxed);
        }
    });
    return loaded.load();
}

std::future<std::size_t> TrackCollision::loadAroundAsync(glm::vec2 centre, float radius) const
{
    return std::async(std::launch::async, [this, centre, radius] { return loadAround(centre, radius); });
}

void TrackCollision::evictOutside(glm::vec2 centre, float radius)
{
    const std::lock_guard lock(m_mutex);
    for (auto it = m_cache.begin(); it != m_cache.end();) {
        const auto [min, max] = m_index.tileBounds(m_index.tiles()[it->first]);
        const bool near = min.x <= centre.x + radius && max.x >= centre.x - radius && min.y <= centre.y + radius
            && max.y >= centre.y - radius;
        it = near ? std::next(it) : m_cache.erase(it);
    }
}

void TrackCollision::evictAll()
{
    const std::lock_guard lock(m_mutex);
    m_cache.clear();
}

std::size_t TrackCollision::residentCount() const
{
    const std::lock_guard lock(m_mutex);
    return m_cache.size();
}

std::vector<TrackCollision::Candidate> TrackCollision::resident(const std::vector<std::size_t>& indices) const
{
    std::vector<Candidate> result;
    result.reserve(indices.size());
    for (const std::size_t index : indices) {
        if (std::shared_ptr<const FizTile> tile = tryTile(index)) {
            result.push_back({index, std::move(tile)});
        }
    }
    return result;
}

bool TrackCollision::owns(std::size_t tileIndex, glm::vec2 point, const std::vector<Candidate>& candidates) const
{
    if (const std::optional<std::size_t> owner = m_index.tileAt(point.x, point.y)) {
        return *owner == tileIndex;
    }
    // No tile has this cell, so only neighbours' margins reach it; which of
    // them reports the geometry is settled by the caller's deduplication.
    for (const Candidate& candidate : candidates) {
        if (candidate.index == tileIndex) {
            return true;
        }
    }
    return false;
}

void TrackCollision::rayTile(const Candidate& candidate, const glm::vec3& origin, const glm::vec3& direction,
    float maxDistance, float fromDistance, float toDistance, const QueryFilter& filter,
    std::optional<RayHit>& best) const
{
    const FizTile& tile = *candidate.tile;
    const float from = std::max(fromDistance - kEdgeSlack, 0.0F);
    const float to = std::min(toDistance + kEdgeSlack, maxDistance);
    const glm::vec3 a = origin + direction * from;
    const glm::vec3 b = origin + direction * to;
    walkGrid(tile.boundsMin, tile.cellSize, tile.cellsX, tile.cellsZ, xz(a), xz(b),
        [&](std::uint32_t x, std::uint32_t z, float, float tExit) {
            for (const std::uint16_t index : tile.cell(x, z)) {
                const FizTriangle& triangle = tile.triangles[index];
                if (!passes(tile, triangle, filter)) {
                    continue;
                }
                const std::optional<float> t = intersect(origin, direction, maxDistance,
                    tile.positions[triangle.vertices[0]], tile.positions[triangle.vertices[1]],
                    tile.positions[triangle.vertices[2]]);
                if (!t || (best && *t >= best->distance)) {
                    continue;
                }
                RayHit hit;
                hit.distance = *t;
                hit.point = origin + direction * *t;
                hit.frontFace = glm::dot(triangle.normal, direction) <= 0.0F;
                hit.normal = hit.frontFace ? triangle.normal : -triangle.normal;
                hit.surface = triangle.surface;
                hit.flags = triangle.flags;
                hit.tile = candidate.index;
                hit.triangle = index;
                best = hit;
            }
            // Cells come in order along the ray, so nothing further on can
            // be nearer than a hit before this cell's exit.
            return !(best && best->distance <= from + (to - from) * tExit);
        });
}

std::optional<RayHit> TrackCollision::raycast(
    const glm::vec3& origin, const glm::vec3& direction, float maxDistance, const QueryFilter& filter) const
{
    const float length = glm::length(direction);
    if (!(length > 0.0F) || !std::isfinite(length) || !(maxDistance > 0.0F) || !std::isfinite(maxDistance)) {
        return std::nullopt;
    }
    const glm::vec3 unit = direction / length;
    const glm::vec3 end = origin + unit * maxDistance;
    std::optional<RayHit> best;
    walkGrid(m_index.origin(), m_index.cellSize(), m_index.columns(), m_index.rows(), xz(origin), xz(end),
        [&](std::uint32_t column, std::uint32_t row, float tEnter, float tExit) {
            const float from = tEnter * maxDistance;
            const float to = tExit * maxDistance;
            std::vector<std::size_t> indices;
            if (const std::optional<std::size_t> own = m_index.tileAtCell(column, row)) {
                indices.push_back(*own);
            } else {
                const glm::vec2 p0 = xz(origin + unit * from);
                const glm::vec2 p1 = xz(origin + unit * to);
                indices = m_index.tilesOverlapping(glm::min(p0, p1), glm::max(p0, p1));
            }
            for (const Candidate& candidate : resident(indices)) {
                rayTile(candidate, origin, unit, maxDistance, from, to, filter, best);
            }
            return !(best && best->distance <= to + kEdgeSlack);
        });
    return best;
}

std::vector<SphereContact> TrackCollision::sphereContacts(
    const glm::vec3& centre, float radius, const QueryFilter& filter) const
{
    std::vector<SphereContact> contacts;
    if (!(radius > 0.0F) || !std::isfinite(radius)) {
        return contacts;
    }
    const glm::vec2 lo = xz(centre) - glm::vec2(radius);
    const glm::vec2 hi = xz(centre) + glm::vec2(radius);
    const std::vector<Candidate> candidates = resident(m_index.tilesOverlapping(lo, hi));
    std::vector<SphereContact> orphans;
    const float radiusSquared = radius * radius;
    for (const Candidate& candidate : candidates) {
        const FizTile& tile = *candidate.tile;
        for (const std::uint16_t index : trianglesIn(tile, lo, hi)) {
            const FizTriangle& triangle = tile.triangles[index];
            if (!passes(tile, triangle, filter)) {
                continue;
            }
            const glm::vec3& a = tile.positions[triangle.vertices[0]];
            const glm::vec3& b = tile.positions[triangle.vertices[1]];
            const glm::vec3& c = tile.positions[triangle.vertices[2]];
            const float height = glm::dot(centre - a, triangle.normal);
            if (height > radius || height < -radius) {
                continue;
            }
            const auto [point, inside] = closestPoint(centre, a, b, c);
            const glm::vec3 offset = centre - point;
            const float distanceSquared = glm::dot(offset, offset);
            if (distanceSquared > radiusSquared) {
                continue;
            }
            SphereContact contact;
            contact.point = point;
            contact.surface = triangle.surface;
            contact.flags = triangle.flags;
            if (inside) {
                contact.normal = triangle.normal;
                contact.depth = radius - height;
            } else {
                // Beside the face from behind: the neighbouring face, whose
                // front the centre is on, gives the contact instead.
                if (height <= 0.0F) {
                    continue;
                }
                const float distance = std::sqrt(distanceSquared);
                contact.normal = distance > 1e-6F ? offset / distance : triangle.normal;
                contact.depth = radius - distance;
            }
            const glm::vec2 at = xz(point);
            if (!owns(candidate.index, at, candidates)) {
                continue;
            }
            if (m_index.tileAt(at.x, at.y)) {
                contacts.push_back(contact);
            } else {
                orphans.push_back(contact);
            }
        }
    }
    for (const SphereContact& orphan : orphans) {
        const bool seen = std::any_of(contacts.begin(), contacts.end(), [&](const SphereContact& c) {
            return glm::length(c.point - orphan.point) < kSameContact && glm::dot(c.normal, orphan.normal) > 0.9999F
                && std::abs(c.depth - orphan.depth) < kSameContact;
        });
        if (!seen) {
            contacts.push_back(orphan);
        }
    }
    std::sort(contacts.begin(), contacts.end(),
        [](const SphereContact& l, const SphereContact& r) { return l.depth > r.depth; });
    return contacts;
}

std::vector<RayHit> TrackCollision::verticalHits(float x, float z, const QueryFilter& filter) const
{
    std::vector<RayHit> hits;
    const glm::vec3 origin(x, kVerticalTop, z);
    const glm::vec3 down(0.0F, -1.0F, 0.0F);
    const float span = 2.0F * kVerticalTop;
    std::vector<std::size_t> indices;
    if (const std::optional<std::size_t> own = m_index.tileAt(x, z)) {
        indices.push_back(*own);
    } else {
        indices = m_index.tilesOverlapping(glm::vec2(x, z), glm::vec2(x, z));
    }
    for (const Candidate& candidate : resident(indices)) {
        const FizTile& tile = *candidate.tile;
        for (const std::uint16_t index : trianglesIn(tile, glm::vec2(x, z), glm::vec2(x, z))) {
            const FizTriangle& triangle = tile.triangles[index];
            if (!passes(tile, triangle, filter)) {
                continue;
            }
            const std::optional<float> t = intersect(origin, down, span, tile.positions[triangle.vertices[0]],
                tile.positions[triangle.vertices[1]], tile.positions[triangle.vertices[2]]);
            if (!t) {
                continue;
            }
            RayHit hit;
            hit.distance = *t;
            hit.point = origin + down * *t;
            hit.frontFace = triangle.normal.y >= 0.0F;
            hit.normal = hit.frontFace ? triangle.normal : -triangle.normal;
            hit.surface = triangle.surface;
            hit.flags = triangle.flags;
            hit.tile = candidate.index;
            hit.triangle = index;
            hits.push_back(hit);
        }
    }
    std::sort(hits.begin(), hits.end(), [](const RayHit& l, const RayHit& r) { return l.point.y > r.point.y; });
    // Neighbouring triangles meet under the line on shared edges, and
    // neighbouring tiles share triangles; one hit per surface and height.
    hits.erase(std::unique(hits.begin(), hits.end(),
                   [](const RayHit& l, const RayHit& r) {
                       return std::abs(l.point.y - r.point.y) < kSameContact && l.surface == r.surface
                           && l.frontFace == r.frontFace;
                   }),
        hits.end());
    return hits;
}

std::optional<RayHit> TrackCollision::groundBelow(
    const glm::vec3& point, float maxDrop, const QueryFilter& filter) const
{
    return raycast(point, glm::vec3(0.0F, -1.0F, 0.0F), maxDrop, filter);
}

void TrackCollision::forEachTriangle(glm::vec2 lo, glm::vec2 hi, const QueryFilter& filter,
    const std::function<void(std::size_t, const FizTile&, std::uint16_t)>& visit) const
{
    const std::vector<Candidate> candidates = resident(m_index.tilesOverlapping(lo, hi));
    std::vector<std::array<float, 9>> orphans;
    for (const Candidate& candidate : candidates) {
        const FizTile& tile = *candidate.tile;
        for (const std::uint16_t index : trianglesIn(tile, lo, hi)) {
            const FizTriangle& triangle = tile.triangles[index];
            if (!passes(tile, triangle, filter)) {
                continue;
            }
            glm::vec3 min(std::numeric_limits<float>::infinity());
            glm::vec3 max(-std::numeric_limits<float>::infinity());
            glm::vec3 sum(0.0F);
            std::array<glm::vec3, 3> corners{};
            for (int k = 0; k < 3; ++k) {
                corners[k] = tile.positions[triangle.vertices[k]];
                min = glm::min(min, corners[k]);
                max = glm::max(max, corners[k]);
                sum += corners[k];
            }
            if (min.x > hi.x || max.x < lo.x || min.z > hi.y || max.z < lo.y) {
                continue;
            }
            // The centroid lies in the triangle, so the tile of the centroid's
            // cell holds it; that tile alone reports it.
            const glm::vec2 centroid = xz(sum / 3.0F);
            if (!owns(candidate.index, centroid, candidates)) {
                continue;
            }
            if (!m_index.tileAt(centroid.x, centroid.y)) {
                std::sort(corners.begin(), corners.end(), [](const glm::vec3& l, const glm::vec3& r) {
                    return std::tie(l.x, l.y, l.z) < std::tie(r.x, r.y, r.z);
                });
                const std::array<float, 9> key{corners[0].x, corners[0].y, corners[0].z, corners[1].x, corners[1].y,
                    corners[1].z, corners[2].x, corners[2].y, corners[2].z};
                if (std::find(orphans.begin(), orphans.end(), key) != orphans.end()) {
                    continue;
                }
                orphans.push_back(key);
            }
            visit(candidate.index, tile, index);
        }
    }
}

} // namespace fh::collision
