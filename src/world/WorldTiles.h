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

namespace fh::world {

/// Which event props (Placement::eventProp) a WorldTileGrid includes; other
/// chunks are always included.
class EventPropFilter {
public:
    /// None.
    EventPropFilter() = default;
    static EventPropFilter all();
    /// The props the game's route gate shows while route `routeId` is loaded
    /// (Placement::onRoute): 0 in free roam, a race's Tracks.RouteId during
    /// the race. Props with an eventTag are left out whatever their routes:
    /// the game also shows and hides them by name, which the activation
    /// zones do here (ActivityProps), and how its route and name gates
    /// combine for a prop that has both was not traced.
    static EventPropFilter route(int routeId);

    bool includes(const WorldChunk& chunk) const;
    bool operator==(const EventPropFilter&) const = default;

private:
    bool m_all = false;
    int m_routeId = -1;
};

/// The world's chunks bucketed into square tiles of the XZ plane, so a
/// renderer can merge a tile's chunks into one draw and re-merge only when
/// the camera crosses one of the tile's LOD band edges.
class WorldTileGrid {
public:
    struct Tile {
        /// The tile's cell: tile (x, z) covers the chunks whose centres lie
        /// in [origin + tileSize·x, origin + tileSize·(x + 1)) along X, and
        /// likewise along Z, with the origin at the index footprint's minimum.
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

    /// Leaves out the event props that `eventProps` does not include, and
    /// the chunks `hidden` marks (indexed like WorldIndex::chunks()). The
    /// grid keeps a reference to `index`.
    WorldTileGrid(const WorldIndex& index, float tileSize, const EventPropFilter& eventProps = {},
        const std::vector<bool>& hidden = {});

    const std::vector<Tile>& tiles() const { return m_tiles; }
    float tileSize() const { return m_tileSize; }

    /// The cell holding world point `x`, `z`.
    std::pair<int, int> cellAt(float x, float z) const;
    /// The index into tiles() of the tile of cell (`cellX`, `cellZ`), or
    /// nothing if no chunk's centre lies in it.
    std::optional<std::size_t> tileAt(int cellX, int cellZ) const;

    /// Applies new filters: which event props to draw, and which chunks to
    /// leave out. Returns the tiles whose chunks changed, which alone need
    /// building again.
    std::vector<std::size_t> update(const EventPropFilter& eventProps, const std::vector<bool>& hidden);

    /// Horizontal distance from `x`, `z` to the tile's bounds (0 inside).
    float distanceTo(const Tile& tile, float x, float z) const;
    /// Which of the tile's distance intervals `distance` falls in; the chunk
    /// set changes only when this does.
    int stateAt(const Tile& tile, float distance) const;
    /// Chunks drawn when the tile is `distance` metres away.
    std::vector<std::uint32_t> chunksAt(const Tile& tile, float distance) const;
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

/// Geometry of one tile state, ready for upload: interleaved position
/// (x, y, z), normal (x, y, z), first and second texture coordinates (u, v
/// each) and the vertex colour's four bytes (in file order: R, G, B, A) in
/// one float's space, and triangle-list indices grouped into one batch per
/// surface (shading and textures), with backdrop terrain in batches of its
/// own, one per chunk, so each can be shown or hidden by the camera's zone
/// (WorldChunk::visibleFrom). Triangles wind clockwise seen from their front
/// in world coordinates, which is how the model files' winding comes out
/// after the parser's Z flip; triangles of placements that mirror their
/// model are reordered to match. The normals point to the front.
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
        /// Ground: Blend_A, Blend_B and Blend_C mixed by a splat map (red
        /// weighs B, green C, the rest A), times an ambient occlusion map.
        Splat,
        /// Ground: Blend_A and Blend_B mixed by the vertex colour's alpha,
        /// times an ambient occlusion map.
        VertexBlend,
        /// Lakes: animated normal maps and a reflection, drawn translucent.
        Water,
    };
    /// Indices into Batch::layers.
    enum Layer : std::uint8_t { LayerB, LayerC, SplatMap, OcclusionMap, LayerCount };

    struct Batch {
        /// The diffuse texture, Blend_A for ground, or the first normal map
        /// for water.
        std::uint32_t texture = kNoTexture;
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        /// Geometry of backdrop chunks (WorldChunk::backdrop).
        bool backdrop = false;
        /// For backdrop batches, the chunk (index into WorldIndex::chunks())
        /// the geometry comes from; kMergedChunks otherwise.
        std::uint32_t chunk = kMergedChunks;
        Shading shading = Shading::Plain;
        /// The other textures of ground shading (see Layer); kNoTexture
        /// where unused. The splat and occlusion maps read the second
        /// texture coordinates.
        std::array<std::uint32_t, LayerCount> layers{kNoTexture, kNoTexture, kNoTexture, kNoTexture};
        /// How often Blend_A, Blend_B and Blend_C repeat per unit of the first
        /// texture coordinates (u, v each).
        std::array<float, 6> layerScales{1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};

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

/// Materials that are not visible surfaces:
/// - a lighting effect, such as the glow of the town's lights over the night
///   sky ("light_pollution.fx" on a 1 km plane), which drawn as a surface
///   becomes a large opaque wall;
/// - the placeholder material ("Placeholder001") of an abandoned set of
///   terrain pieces ("Area04", "TERR_Zone1_Area1_00"), textured "THIS OBJECT
///   DOES NOT HAVE A FORZA MATERIAL" and partly below the ground. On
///   Colorado 63 models use it and only it;
/// - crowd areas ("CrowdTERR" on "Plane004_LOD00" and the like): flat
///   polygons just above the ground, textured with an orange tile grid,
///   that mark where spectators stand. On Colorado 48 models use it and
///   only it.
/// Such materials are neither drawn nor picked.
bool isNotSurface(const RenderMesh& mesh, const RenderMesh::Material& material);

/// Decodes and merges `chunks` (indices into `index.chunks()`) on the calling
/// thread. Safe to call from several threads at once on the same archive.
/// Normals are averaged per part from its triangles, weighted by area.
/// Without `textures`, or for materials whose diffuse texture or texture
/// coordinates are unknown, geometry goes into a kNoTexture batch. Chunks
/// that cannot be decoded are counted and reported in the mesh, not thrown.
TileMesh buildTileMesh(const lzx::ZipArchive& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& chunks, const TrackTextures* textures = nullptr);

} // namespace fh::world
