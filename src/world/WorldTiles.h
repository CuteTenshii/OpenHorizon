#pragma once

#include "RenderMesh.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace openhorizon::world {

/// Which event props (Placement::eventProp) a WorldTileGrid includes; other chunks always are.
class EventPropFilter {
public:
    /// None.
    EventPropFilter() = default;
    static EventPropFilter all();
    /// Props the route gate shows for `routeId` (0 in free roam, else Tracks.RouteId). Props with an eventTag are
    /// left to ActivityProps; how the game combines both gates was not traced.
    static EventPropFilter route(int routeId);

    bool includes(const WorldChunk& chunk) const;
    bool operator==(const EventPropFilter&) const = default;

private:
    bool m_all = false;
    int m_routeId = -1;
};

/// Chunks bucketed into square XZ tiles, re-merged only when the camera crosses a tile's LOD band edge.
class WorldTileGrid {
public:
    struct Tile {
        /// Chunks whose centres lie in [origin + tileSize·x, origin + tileSize·(x + 1)) per axis, origin at the
        /// footprint's minimum.
        int cellX = 0;
        int cellZ = 0;
        /// Union of the tile's chunk bounds.
        glm::vec3 boundsMin{0.0F};
        glm::vec3 boundsMax{0.0F};
        /// Indices into WorldIndex::chunks(): the chunks drawn.
        std::vector<std::uint32_t> chunks;
        /// Every chunk in the tile, drawn or left out by the filters.
        std::vector<std::uint32_t> allChunks;
        /// Sorted, distinct band edges of the tile's chunks.
        std::vector<float> edges;
    };

    /// `hidden` is indexed like WorldIndex::chunks(). Keeps a reference to `index`.
    WorldTileGrid(const WorldIndex& index, float tileSize, const EventPropFilter& eventProps = {},
        const std::vector<bool>& hidden = {});

    const std::vector<Tile>& tiles() const { return m_tiles; }
    float tileSize() const { return m_tileSize; }

    /// The cell holding world point `x`, `z`.
    std::pair<int, int> cellAt(float x, float z) const;
    /// Nothing if no chunk's centre lies in the cell.
    std::optional<std::size_t> tileAt(int cellX, int cellZ) const;

    /// Returns the tiles whose chunks changed, which alone need rebuilding.
    std::vector<std::size_t> update(const EventPropFilter& eventProps, const std::vector<bool>& hidden);

    /// Horizontal distance from `x`, `z` to the tile's bounds (0 inside).
    float distanceTo(const Tile& tile, float x, float z) const;
    /// The tile's distance interval; the chunk set changes only when this does.
    int stateAt(const Tile& tile, float distance) const;
    /// Chunks drawn when the tile is `distance` metres away.
    std::vector<std::uint32_t> chunksAt(const Tile& tile, float distance) const;
    /// Chunks whose band ends beyond `distance`: all a camera at least that far from each chunk may draw.
    std::vector<std::uint32_t> chunksBeyond(const Tile& tile, float distance) const;
    const WorldIndex& index() const { return m_index; }

private:
    const WorldIndex& m_index;
    float m_tileSize = 1.0F;
    glm::vec2 m_origin{0.0F};
    std::vector<Tile> m_tiles;
    /// Tile index by cell, keyed by cellKey().
    std::unordered_map<std::uint64_t, std::size_t> m_tileByCell;
    /// Set once the constructor has filled in every tile.
    bool m_built = false;
};

/// One tile state's geometry: interleaved position, normal, two UV pairs and the colour bytes (R, G, B, A) in one
/// float; one batch per surface, backdrop one per chunk for zone culling (WorldChunk::visibleFrom).
/// Triangles wind clockwise from the front in world space, mirroring placements reordered to match.
struct TileMesh {
    static constexpr int kFloatsPerVertex = 11;
    /// Float index of the normal in a vertex.
    static constexpr int kNormal = 3;
    /// Float index of the first texture coordinate pair in a vertex.
    static constexpr int kTexcoord = 6;
    /// Float index of the second texture coordinate pair in a vertex.
    static constexpr int kSecondTexcoord = 8;
    /// Float index of the vertex colour's bytes in a vertex.
    static constexpr int kColour = 10;
    /// Batch texture of geometry without a known diffuse texture.
    static constexpr std::uint32_t kNoTexture = 0xFFFFFFFF;
    /// Batch chunk of geometry merged from several chunks.
    static constexpr std::uint32_t kMergedChunks = 0xFFFFFFFF;

    /// How a batch's textures combine, from its shader's samplers.
    enum class Shading : std::uint8_t {
        /// One diffuse texture, alpha-tested.
        Plain,
        /// Blend_A/B/C mixed by a splat map (red weighs B, green C, the rest A), times an occlusion map.
        Splat,
        /// Blend_A and Blend_B mixed by the vertex colour's alpha, times an occlusion map.
        VertexBlend,
        /// Lakes: animated normal maps and a reflection, drawn translucent.
        Water,
    };
    /// Indices into Batch::layers.
    enum Layer : std::uint8_t { LayerB, LayerC, SplatMap, OcclusionMap, LayerCount };

    struct Batch {
        /// The diffuse texture, Blend_A for ground, or the first normal map for water.
        std::uint32_t texture = kNoTexture;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        /// Geometry of backdrop chunks (WorldChunk::backdrop).
        bool backdrop = false;
        /// The backdrop batch's chunk (WorldIndex::chunks()); kMergedChunks otherwise.
        std::uint32_t chunk = kMergedChunks;
        Shading shading = Shading::Plain;
        /// kNoTexture where unused. The splat and occlusion maps read the second texture coordinates.
        std::array<std::uint32_t, LayerCount> layers{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
        /// Blend_A/B/C repeats per unit of the first texture coordinates (u, v each).
        std::array<float, 6> layerScales{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
        /// Covers exactly [firstIndex, firstIndex + indexCount), chunk by chunk.
        std::vector<ChunkRun> runs;

        /// Every texture the batch draws with.
        std::vector<std::uint32_t> textures() const;
    };

    /// One model file that went into the mesh, or failed to.
    struct Model {
        /// Index into WorldIndex::chunks().
        std::uint32_t chunk = 0;
        std::uint32_t triangles = 0;
        /// Distinct textures of its surfaces, in first-use order.
        std::vector<std::uint32_t> textures;
        /// Empty unless the model could not be read.
        std::string error;
    };

    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<Model> models;
    /// Chunks that failed to decode; their errors are in `errors`.
    int failedChunks = 0;
    std::vector<std::string> errors;

    std::size_t vertexCount() const { return vertices.size() / kFloatsPerVertex; }
    std::size_t triangleCount() const { return indices.size() / 3; }
    /// Every texture the batches draw with, each once, in first-use order.
    std::vector<std::uint32_t> textures() const;
};

/// Neither drawn nor picked: "light_pollution.fx" (a 1 km glow plane), "Placeholder001" (abandoned terrain pieces)
/// and "CrowdTERR" (flat markers of where spectators stand).
bool isNotSurface(const RenderMesh& mesh, const RenderMesh::Material& material);

/// Thread-safe on one archive. Normals are area-weighted per part; unknown textures go to a kNoTexture batch.
/// Undecodable chunks are reported in the mesh, not thrown.
TileMesh buildTileMesh(const lzx::ZipArchive& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& chunks, const TrackTextures* textures = nullptr);

} // namespace openhorizon::world
