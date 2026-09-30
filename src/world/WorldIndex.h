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

namespace fh::world {

/// World coordinates.
///
/// The world uses the coordinates of the track's placement files
/// (CollObjs.xml, GameObjs.xml, the route files and the zone grid): X east,
/// Y up, and Z as those files give it. Render model files store positions
/// with Z negated relative to that, so the model parser (RenderMesh) negates
/// Z as it reads, and every position, bound and normal in this library is in
/// world coordinates. Placement matrices (Placement::rows) are the exception:
/// they apply to a model's coordinates as stored, and Placement::apply()
/// undoes the parser's negation first. Negating Z mirrors the geometry, so
/// triangles read from a file wind the other way round in world space.

/// The triangles of one chunk inside a merged batch: `indexCount` indices
/// from `firstIndex` of the mesh's index list. A builder appends a chunk's
/// triangles to each batch in one go, so the renderer can pick chunks one
/// by one out of merged geometry.
struct ChunkRun {
    /// Index into WorldIndex::chunks().
    std::uint32_t chunk = 0;
    std::uint32_t firstIndex = 0;
    std::uint32_t indexCount = 0;

    bool operator==(const ChunkRun&) const = default;
};

/// Records that `count` indices from `first` belong to `chunk`, extending
/// the last run when it is the same chunk's.
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

/// One render model drawn in the world: a world-space model of a track's
/// bin.zip, or one placement of a prop modelled in its own local space. It
/// carries the range of camera distances at which it is drawn.
struct WorldChunk {
    /// Index into lzx::ZipArchive::entries() of the archive the index was
    /// built from. bin.zip stores many names more than once, so entries are
    /// addressed by position.
    std::uint32_t entry = 0;
    /// World bounds (for a placed prop, of its transformed model bounds).
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    /// Level of detail from the part name, or -1 for meshes without levels.
    std::int8_t lod = -1;
    /// Chunks whose names differ only in LOD level and chunk number share a
    /// group; the group's levels decide each chunk's distance band.
    std::uint32_t group = 0;
    float bandStart = 0.0F;
    float bandEnd = std::numeric_limits<float>::infinity();
    /// Part of the map-wide far terrain (`TERR_UberLOD_*`), which should only
    /// show where no other geometry covers the same ground: it lies within a
    /// few metres of the detailed ground where both exist (up to 14 m above
    /// it at the festival), and alone beyond the drivable area.
    bool backdrop = false;
    /// For chunks of zone files' draw records, in an index built with
    /// visibility data: the zones (see ZoneGrid) whose files list the draw,
    /// in increasing order. The game draws the chunk only while the camera
    /// is in one of them (verified: the per-draw instances are built from
    /// the draw list of the camera's zone alone, 0x8241FFA8, and nothing
    /// else is drawn, 0x824701B8), which for the backdrop keeps it away from
    /// where the detailed ground lies below it. Empty when drawn from
    /// everywhere: procedural copies, which the zone files do not list, and
    /// prop levels that are in no zone file and borrow a sibling level's
    /// placement.
    std::vector<std::uint16_t> zones;
    /// A prop drawn where `placement` puts it; world-space models are drawn
    /// as stored.
    bool placed = false;
    Placement placement;
    /// A placed copy from a procedural set (see ScatterSet) rather than a
    /// zone file's placement.
    bool scattered = false;

    /// Whether the chunk is drawn while the camera is in `zone`; -1 (outside
    /// every zone, or no zone data) draws it.
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

/// The render models of a track, found by reading only the header of every
/// `*.rmb.bin` in its archive. Duplicate copies of a model are indexed once.
/// Models centred on the origin are props in their own local space, and so
/// are models whose draw records move them away from where their file puts
/// them (props modelled around a distant pivot: the dam's buildings,
/// gondolas, mining towers): each of their placements (see TrackPlacements)
/// becomes a chunk of its own, and those never placed are only counted.
/// Boundary shells, shadow-casting stand-ins and the crude scene copy used
/// for reflections are left out. With the track's zone grid, chunks of zone
/// files' draws carry the zones they are drawn from, and backdrop levels no
/// zone lists (which the game never draws) are left out.
///
/// All coordinates are world coordinates, described at the top of this file.
class WorldIndex {
public:
    /// Distance (metres) from which each level replaces the finer one, for
    /// chunks whose placement gives no ranges (the world-space models). A
    /// group's finest level starts at 0 and its coarsest level has no end.
    /// Not the game's: a viewer choice kept from forza-map-manager. The game
    /// takes each draw's own ranges from its zone record (0x82E192C0), but
    /// which level a world-space draw is comes from flags whose source was
    /// not traced (see TODO.md).
    static constexpr float kLodStart[] = {0.0F, 400.0F, 1500.0F, 4000.0F};

    /// Reads every render model header of `archive` on all hardware threads,
    /// and places props where `placements` says (without it, props are left
    /// out). With `placements` and `zones`, limits the backdrop to the zones
    /// that list it. Model files that cannot be read are skipped. Returns
    /// nothing if `cancel` was set while it ran. `progress` is called from
    /// worker threads, one call at a time.
    static std::optional<WorldIndex> build(const lzx::ZipArchive& archive, const Progress& progress = {},
        const std::atomic<bool>* cancel = nullptr, const TrackPlacements* placements = nullptr,
        const ZoneGrid* zones = nullptr);

    /// An index of exactly `chunks`, kept as given (distance bands included),
    /// without a zone grid; for showing a few chunks on their own, such as a
    /// single model in a preview.
    static WorldIndex fromChunks(std::vector<WorldChunk> chunks);

    const std::vector<WorldChunk>& chunks() const { return m_chunks; }
    /// Props (local-space models) with no placement, which are not drawn.
    int localModelCount() const { return m_localModels; }
    /// Chunks that are placements of props, from zone files and procedural
    /// sets together.
    std::size_t placedCount() const;
    /// Chunks that are procedural copies.
    std::size_t scatteredCount() const;
    /// The zone grid the index was built with, or nullptr.
    const ZoneGrid* zoneGrid() const { return m_zoneGrid ? &*m_zoneGrid : nullptr; }
    /// Bounds of all chunks in the XZ plane.
    Footprint footprint() const;

    /// Assigns distance bands: a placed prop's from the distances its
    /// placement gives, where it gives them; otherwise from the LOD levels
    /// present in the chunk's group. Meshes without levels are drawn up to
    /// 1 km, or 15 times their diagonal for big ones. Exposed for tests;
    /// build() calls it.
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

} // namespace fh::world
