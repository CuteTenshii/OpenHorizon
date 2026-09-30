#include "WorldIndex.h"

#include "RenderMesh.h"
#include "ScatterSet.h"
#include "TrackTextures.h"

#include <glm/geometric.hpp>

#include <lzx/zip.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <map>
#include <unordered_set>

namespace openhorizon::world {

namespace {

/// Models whose centre lies this close to the origin are in local space.
constexpr float kLocalSpaceRadius = 5.0F;
/// How far a transform may be from the world-space one (no move, Z mirrored)
/// and still count as it.
constexpr float kWorldTransformTolerance = 0.01F;
/// Meshes without LOD levels are drawn up to this distance, or further for
/// big ones (terrain cubes), so small detail such as cables fades out.
constexpr float kNoLodMinRange = 1000.0F;
constexpr float kNoLodRangePerMetre = 15.0F;
/// How many draw records away a prop level without a zone placement looks
/// for a sibling level to borrow the transform of.
constexpr int kNeighbourReach = 3;

/// Whether a draw's placement leaves its model where the model file puts it:
/// at the origin, not turned, with the file's Z mirrored as every
/// world-space model is.
bool isWorldSpaceTransform(const Placement& placement)
{
    constexpr std::array<float, 9> kWorldRows{1, 0, 0, 0, 1, 0, 0, 0, -1};
    if (glm::length(placement.position) > kWorldTransformTolerance) {
        return false;
    }
    for (std::size_t k = 0; k < kWorldRows.size(); ++k) {
        if (std::abs(placement.rows[k] - kWorldRows[k]) > kWorldTransformTolerance) {
            return false;
        }
    }
    return true;
}

bool contains(std::string_view text, std::string_view part)
{
    return text.find(part) != std::string_view::npos;
}

/// A "_CAGE" token, optionally numbered, that ends the name or is followed
/// by an underscore. `lower` is in lower case.
bool hasCageToken(std::string_view lower)
{
    for (std::size_t at = lower.find("_cage"); at != std::string_view::npos; at = lower.find("_cage", at + 1)) {
        std::size_t end = at + 5;
        while (end < lower.size() && lower[end] >= '0' && lower[end] <= '9') {
            ++end;
        }
        if (end == lower.size() || lower[end] == '_') {
            return true;
        }
    }
    return false;
}

/// Meshes that are not visible world surface: boundary walls
/// ("UberLOD23_CAGE_LOD00", "TERR_UberLOD_Patch10_CAGE003_LOD00"), a shell
/// around the whole map ("Underground_NOLOD"), shadow-casting stand-ins
/// ("FT02_ShadowCaster_LOD01", "SHADOWBOX_MOUNTAINS_AREA04_LOD01",
/// "Shadow_Terrain_Area04_LOD02"), and a crude copy of the scene
/// ("TERR_CUBE_MainTown_Area3_12", "TERR_CUBE_Plains_Area04_4"): ground,
/// roads and buildings as plain blocks, most likely what the game renders
/// into its reflection cube maps. Every surface of that copy exists in full
/// detail elsewhere; drawn, its buildings stick out as grey blocks.
bool isHelperGeometry(std::string_view partName)
{
    const std::string lower = toLower(partName);
    return hasCageToken(lower) || lower.starts_with("underground") || contains(lower, "shadowcaster")
        || contains(lower, "shadowbox") || lower.starts_with("shadow_") || lower.starts_with("terr_cube_");
}

/// The far terrain of the whole map ("TERR_UberLOD_Patch07_LOD00",
/// "TERR_UberLOD_Patch18").
bool isBackdrop(std::string_view partName)
{
    return contains(toLower(partName), "uberlod");
}

/// The distance band a placement's ranges give level `lod`, if they give
/// one: a level ends where the next begins, and the last where the prop
/// stops being drawn. Meshes without levels are drawn up to the furthest.
std::optional<std::pair<float, float>> placementBand(const Placement& placement, int lod)
{
    const auto& ranges = placement.ranges;
    if (lod < 0) {
        const float furthest = *std::max_element(ranges.begin(), ranges.end());
        return furthest > 0.0F ? std::optional<std::pair<float, float>>({0.0F, furthest}) : std::nullopt;
    }
    if (lod >= static_cast<int>(ranges.size())) {
        return std::nullopt;
    }
    const float start = lod == 0 ? 0.0F : ranges[static_cast<std::size_t>(lod) - 1];
    const float end = ranges[static_cast<std::size_t>(lod)];
    if (start < 0.0F || end <= start) {
        return std::nullopt;
    }
    return std::pair<float, float>{start, end};
}

std::int8_t clampedLod(std::string_view partName)
{
    return static_cast<std::int8_t>(std::clamp(rendermesh::lodLevel(partName), -1, 3));
}

} // namespace

/// A prop model found in the archive, to be placed by the draw records.
struct WorldIndex::LocalModel {
    std::uint32_t entry = 0;
    RenderMeshHeader header;
};

std::optional<WorldIndex> WorldIndex::build(const lzx::ZipArchive& archive, const Progress& progress,
    const std::atomic<bool>* cancel, const TrackPlacements* placements, const ZoneGrid* zones)
{
    const auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
    WorldIndex index;
    std::unordered_map<std::string, std::uint32_t> groups;
    const auto groupOf = [&index, &groups](const std::string& partName) {
        std::string key = rendermesh::lodGroupKey(partName);
        const auto [it, inserted] = groups.try_emplace(key, static_cast<std::uint32_t>(index.m_groupKeys.size()));
        if (inserted) {
            index.m_groupKeys.push_back(std::move(key));
        }
        return it->second;
    };
    // Some props are modelled around a pivot far from their centre and look
    // like world-space models; their draw records move them, which
    // world-space models' never do.
    std::unordered_set<std::uint32_t> movedObjects;
    if (placements != nullptr) {
        for (std::size_t d = 0; d < placements->drawCount(); ++d) {
            const Placement* placement = placements->placement(d);
            if (placement != nullptr && !placements->isScatterTemplate(d) && !isWorldSpaceTransform(*placement)) {
                movedObjects.insert(placements->drawObject(d));
            }
        }
    }
    // bin.zip stores most models several times under the same name, with
    // identical contents at different offsets; one copy is enough.
    const std::vector<std::size_t> models = distinctEntries(archive, ".rmb.bin");
    const auto& entries = archive.entries();
    std::vector<std::optional<RenderMeshHeader>> headers(models.size());
    parallelFor(
        models.size(),
        [&](std::size_t m) {
            if (cancelled()) {
                return;
            }
            try {
                headers[m] = rendermesh::readHeader(
                    readEntryPrefix(archive, entries[models[m]], rendermesh::kHeaderBytes));
            } catch (const DataError&) {
                headers[m].reset();
            }
        },
        progress);
    if (cancelled()) {
        return std::nullopt;
    }

    // Props by render object number, placed once every header is read.
    std::unordered_map<std::uint32_t, LocalModel> localModels;
    for (std::size_t m = 0; m < models.size(); ++m) {
        const std::optional<RenderMeshHeader>& header = headers[m];
        if (!header || isHelperGeometry(header->firstPartName)) {
            continue;
        }
        const auto entry = static_cast<std::uint32_t>(models[m]);
        const glm::vec3 centre = (header->boundsMin + header->boundsMax) / 2.0F;
        const std::optional<std::uint32_t> object = TrackTextures::objectNumber(entries[entry].name);
        if (std::hypot(centre.x, centre.z) < kLocalSpaceRadius || (object && movedObjects.contains(*object))) {
            if (object) {
                localModels.insert_or_assign(*object, LocalModel{entry, *header});
            } else {
                ++index.m_localModels;
            }
            continue;
        }
        WorldChunk chunk;
        chunk.entry = entry;
        chunk.boundsMin = header->boundsMin;
        chunk.boundsMax = header->boundsMax;
        chunk.lod = clampedLod(header->firstPartName);
        chunk.group = groupOf(header->firstPartName);
        chunk.backdrop = isBackdrop(header->firstPartName);
        index.m_chunks.push_back(std::move(chunk));
    }
    if (placements != nullptr && zones != nullptr) {
        index.m_zoneGrid = *zones;
        index.assignZones(archive, *placements);
    }
    if (placements != nullptr) {
        index.placeProps(localModels, *placements, groupOf);
    } else {
        index.m_localModels += static_cast<int>(localModels.size());
    }
    index.assignBands();
    return index;
}

void WorldIndex::placeProps(const std::unordered_map<std::uint32_t, LocalModel>& models,
    const TrackPlacements& placements, const std::function<std::uint32_t(const std::string&)>& groupOf)
{
    const auto modelOf = [&](std::size_t draw) -> const LocalModel* {
        if (draw >= placements.drawCount()) {
            return nullptr;
        }
        const auto it = models.find(placements.drawObject(draw));
        return it == models.end() ? nullptr : &it->second;
    };
    std::unordered_set<std::uint32_t> placedObjects;
    const auto addChunk = [&](const LocalModel& model, const Placement& placement, int lod, bool scattered) {
        WorldChunk chunk;
        chunk.scattered = scattered;
        chunk.entry = model.entry;
        chunk.lod = static_cast<std::int8_t>(lod);
        chunk.group = groupOf(model.header.firstPartName);
        chunk.placed = true;
        chunk.placement = placement;
        const glm::vec3& lo = model.header.boundsMin;
        const glm::vec3& hi = model.header.boundsMax;
        for (int corner = 0; corner < 8; ++corner) {
            const glm::vec3 p = placement.apply(glm::vec3((corner & 1) != 0 ? hi.x : lo.x,
                (corner & 2) != 0 ? hi.y : lo.y, (corner & 4) != 0 ? hi.z : lo.z));
            chunk.boundsMin = corner == 0 ? p : glm::min(chunk.boundsMin, p);
            chunk.boundsMax = corner == 0 ? p : glm::max(chunk.boundsMax, p);
        }
        m_chunks.push_back(std::move(chunk));
    };
    // Each level of detail of a prop is a draw record of its own, next to the
    // prop's other levels and with the same transform. Some levels (mostly
    // LOD00) are in no zone file; they take a neighbouring level's
    // transform, so the prop does not vanish up close.
    const std::size_t draws = placements.drawCount();
    for (std::size_t d = 0; d < draws; ++d) {
        const LocalModel* model = modelOf(d);
        // Templates of the procedural sets sit at the world origin; the sets
        // place their copies below.
        if (model == nullptr || placements.isScatterTemplate(d)) {
            continue;
        }
        const Placement* placement = placements.placement(d);
        if (placement == nullptr) {
            const std::string key = rendermesh::lodGroupKey(model->header.firstPartName);
            for (int step = 1; step <= kNeighbourReach && placement == nullptr; ++step) {
                for (const std::ptrdiff_t n :
                    {static_cast<std::ptrdiff_t>(d) - step, static_cast<std::ptrdiff_t>(d) + step}) {
                    if (n < 0 || static_cast<std::size_t>(n) >= draws) {
                        continue;
                    }
                    const auto neighbour = static_cast<std::size_t>(n);
                    const LocalModel* other = modelOf(neighbour);
                    const Placement* candidate = placements.placement(neighbour);
                    if (other != nullptr && candidate != nullptr && !placements.isScatterTemplate(neighbour)
                        && rendermesh::lodGroupKey(other->header.firstPartName) == key) {
                        placement = candidate;
                        break;
                    }
                }
            }
        }
        if (placement == nullptr) {
            continue;
        }
        addChunk(*model, *placement, clampedLod(model->header.firstPartName), false);
        if (m_zoneGrid) {
            m_chunks.back().zones = placements.zonesListing(d);
        }
        placedObjects.insert(placements.drawObject(d));
    }
    // Copies placed by procedural sets. A mesh's levels are its template
    // draws, finest first; the set's distances are given per level, so the
    // level is the slot rather than the level named in the model file.
    for (const ScatterSet& set : placements.scatterSets()) {
        for (const ScatterSet::Instance& instance : set.instances) {
            const std::vector<std::uint32_t>& levels = set.meshDraws[instance.mesh];
            for (std::size_t level = 0; level < levels.size(); ++level) {
                if (const LocalModel* model = modelOf(levels[level])) {
                    addChunk(*model, instance.placement, static_cast<int>(level), true);
                    placedObjects.insert(placements.drawObject(levels[level]));
                }
            }
        }
    }
    for (const auto& [object, model] : models) {
        if (!placedObjects.contains(object)) {
            ++m_localModels;
        }
    }
}

WorldIndex WorldIndex::fromChunks(std::vector<WorldChunk> chunks)
{
    WorldIndex index;
    index.m_chunks = std::move(chunks);
    return index;
}

void WorldIndex::assignZones(const lzx::ZipArchive& archive, const TrackPlacements& placements)
{
    std::unordered_map<std::uint32_t, std::vector<std::size_t>> drawsOfObject;
    for (std::size_t d = 0; d < placements.drawCount(); ++d) {
        drawsOfObject[placements.drawObject(d)].push_back(d);
    }
    // Only the finest level of the backdrop is in any zone's list; the
    // coarser ones are never drawn, and the finest serves every distance.
    std::vector<bool> unlisted(m_chunks.size(), false);
    for (std::size_t i = 0; i < m_chunks.size(); ++i) {
        WorldChunk& chunk = m_chunks[i];
        const std::optional<std::uint32_t> object = TrackTextures::objectNumber(archive.entries()[chunk.entry].name);
        if (!object) {
            continue;
        }
        if (const auto draws = drawsOfObject.find(*object); draws != drawsOfObject.end()) {
            for (const std::size_t draw : draws->second) {
                const std::vector<std::uint16_t>& listing = placements.zonesListing(draw);
                chunk.zones.insert(chunk.zones.end(), listing.begin(), listing.end());
            }
        }
        std::sort(chunk.zones.begin(), chunk.zones.end());
        chunk.zones.erase(std::unique(chunk.zones.begin(), chunk.zones.end()), chunk.zones.end());
        unlisted[i] = chunk.backdrop && chunk.zones.empty();
    }
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_chunks.size(); ++i) {
        if (unlisted[i]) {
            continue;
        }
        if (kept != i) {
            m_chunks[kept] = std::move(m_chunks[i]);
        }
        ++kept;
    }
    m_chunks.resize(kept);
}

std::size_t WorldIndex::placedCount() const
{
    return static_cast<std::size_t>(
        std::count_if(m_chunks.begin(), m_chunks.end(), [](const WorldChunk& c) { return c.placed; }));
}

std::size_t WorldIndex::scatteredCount() const
{
    return static_cast<std::size_t>(
        std::count_if(m_chunks.begin(), m_chunks.end(), [](const WorldChunk& c) { return c.scattered; }));
}

void WorldIndex::assignBands()
{
    std::map<std::uint32_t, std::vector<int>> levelsByGroup;
    for (const WorldChunk& chunk : m_chunks) {
        if (chunk.lod >= 0) {
            levelsByGroup[chunk.group].push_back(chunk.lod);
        }
    }
    for (auto& [group, levels] : levelsByGroup) {
        std::sort(levels.begin(), levels.end());
        levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    }
    constexpr float kInfinity = std::numeric_limits<float>::infinity();
    for (WorldChunk& chunk : m_chunks) {
        if (chunk.placed) {
            if (const auto band = placementBand(chunk.placement, chunk.lod)) {
                chunk.bandStart = band->first;
                chunk.bandEnd = band->second;
                continue;
            }
        }
        if (chunk.lod < 0) {
            const float diagonal = glm::length(chunk.boundsMax - chunk.boundsMin);
            chunk.bandStart = 0.0F;
            chunk.bandEnd = std::max(kNoLodMinRange, diagonal * kNoLodRangePerMetre);
            continue;
        }
        const std::vector<int>& levels = levelsByGroup[chunk.group];
        const auto it = std::find(levels.begin(), levels.end(), static_cast<int>(chunk.lod));
        const bool finest = it == levels.begin();
        const bool coarsest = std::next(it) == levels.end();
        chunk.bandStart = finest ? 0.0F : kLodStart[chunk.lod];
        chunk.bandEnd = coarsest ? kInfinity : kLodStart[*std::next(it)];
    }
}

Footprint WorldIndex::footprint() const
{
    if (m_chunks.empty()) {
        return {};
    }
    Footprint result{{m_chunks.front().boundsMin.x, m_chunks.front().boundsMin.z},
        {m_chunks.front().boundsMax.x, m_chunks.front().boundsMax.z}};
    for (const WorldChunk& chunk : m_chunks) {
        result.min = glm::min(result.min, glm::vec2(chunk.boundsMin.x, chunk.boundsMin.z));
        result.max = glm::max(result.max, glm::vec2(chunk.boundsMax.x, chunk.boundsMax.z));
    }
    return result;
}

} // namespace openhorizon::world
