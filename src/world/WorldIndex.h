#pragma once

#include "Archive.h"
#include "TrackPlacements.h"
#include "ZoneGrid.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace openhorizon::world {

/// World coordinates are the placement files' (X east, Y up). Render models store Z negated and RenderMesh flips it,
/// which reverses triangle winding; Placement::rows apply to stored coordinates, so Placement::apply() unflips first.

/// One chunk's indices inside a merged batch, so the renderer can pick chunks out of merged geometry.
struct ChunkRun {
    /// Index into WorldIndex::chunks().
    std::uint32_t chunk = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;

    bool operator==(const ChunkRun&) const = default;
};

/// Extends the last run when it is the same chunk's and contiguous.
inline void appendChunkRun(std::vector<ChunkRun>& runs, std::uint32_t chunk, std::uint32_t first, std::uint32_t count)
{
    if (count == 0) {
        return;
    }
    if (!runs.empty() && runs.back().chunk == chunk && runs.back().firstIndex + runs.back().indexCount == first) {
        runs.back().indexCount += count;
        return;
    }
    runs.push_back({chunk, first, count});
}

/// A world-space model of bin.zip, or one placement of a local-space prop, with its camera distance band.
struct WorldChunk {
    /// Index into lzx::ZipArchive::entries(): bin.zip repeats names, so entries go by position.
    std::uint32_t entry = 0;
    /// World bounds (for a placed prop, of its transformed model bounds).
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    /// Level of detail from the part name, or -1 for meshes without levels.
    std::int8_t lod = -1;
    /// Shared by chunks whose names differ only in LOD level and chunk number; its levels set the bands.
    std::uint32_t group = 0;
    float bandStart = 0.0F;
    float bandEnd = std::numeric_limits<float>::infinity();
    /// Map-wide far terrain (`TERR_UberLOD_*`), only meant where no other geometry covers the ground: it lies up to
    /// 14 m above the detailed ground where both exist.
    bool backdrop = false;
    /// Sorted zones whose files list the draw; the game draws only the camera zone's list (verified, 0x8241FFA8,
    /// 0x824701B8). Empty means everywhere: procedural copies and prop levels borrowing a sibling's placement.
    std::vector<std::uint16_t> zones;
    /// Drawn where `placement` puts it; world-space models are drawn as stored.
    bool placed = false;
    Placement placement;
    /// A copy from a procedural set (see ScatterSet), not a zone file's placement.
    bool scattered = false;

    /// Zone -1 (outside every zone, or no zone data) draws everything.
    bool visibleFrom(int zone) const
    {
        return zones.empty() || zone < 0 || std::binary_search(zones.begin(), zones.end(), zone);
    }
};

/// An axis-aligned rectangle in the XZ plane (x = world X, y = world Z).
struct Footprint {
    glm::vec2 min{0.0F};
    glm::vec2 max{0.0F};

    glm::vec2 centre() const { return (min + max) * 0.5F; }
};

/// A track's render models from the headers of every `*.rmb.bin`, duplicates once. Origin-centred models and those
/// whose draw records move them are props: one chunk per placement. Boundary shells, shadow stand-ins, the reflection
/// copy and backdrop levels no zone lists (never drawn by the game) are left out.
class WorldIndex {
public:
    /// Metres from which each level replaces the finer one for world-space models. Guessed (forza-map-manager): the
    /// game reads ranges from zone records (0x82E192C0) by flags not yet traced (TODO.md).
    static constexpr float kLodStart[] = {0.0F, 400.0F, 1500.0F, 4000.0F};

    /// Without `placements` props are left out. Unreadable models are skipped; nothing if `cancel` was set.
    /// `progress` is called from worker threads, one call at a time.
    static std::optional<WorldIndex> build(const lzx::ZipArchive& archive, const Progress& progress = {},
        const std::atomic<bool>* cancel = nullptr, const TrackPlacements* placements = nullptr,
        const ZoneGrid* zones = nullptr);

    /// Exactly `chunks`, bands as given, no zone grid; for previews.
    static WorldIndex fromChunks(std::vector<WorldChunk> chunks);

    const std::vector<WorldChunk>& chunks() const { return m_chunks; }
    /// Props (local-space models) with no placement, which are not drawn.
    int localModelCount() const { return m_localModels; }
    /// Prop placements, zone files and procedural sets together.
    std::size_t placedCount() const;
    /// Chunks that are procedural copies.
    std::size_t scatteredCount() const;
    /// The zone grid the index was built with, or nullptr.
    const ZoneGrid* zoneGrid() const { return m_zoneGrid ? &*m_zoneGrid : nullptr; }
    /// Bounds of all chunks in the XZ plane.
    Footprint footprint() const;

    /// From the placement's distances where given, else the group's LOD levels; meshes without levels draw to
    /// 1 km or 15 diagonals, whichever is larger. Public for tests.
    void assignBands();

private:
    struct LocalModel;
    void placeProps(const std::unordered_map<std::uint32_t, LocalModel>& models, const TrackPlacements& placements,
        const std::function<std::uint32_t(const std::string&)>& groupOf);
    void assignZones(const lzx::ZipArchive& archive, const TrackPlacements& placements);

    std::vector<WorldChunk> m_chunks;
    std::vector<std::string> m_groupKeys;
    int m_localModels = 0;
    std::optional<ZoneGrid> m_zoneGrid;
};

} // namespace openhorizon::world
