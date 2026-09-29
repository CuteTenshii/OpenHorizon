#pragma once

#include "TrackShaders.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include "base/Bytes.h"

#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lzx {
class ZipArchive;
} // namespace lzx

namespace fh::world {

/// Geometry of a set of chunks for the game's own track shaders: one batch
/// per surface (effect, textures and material constants), whose vertices keep
/// the inputs of the effect's vertex declaration in its order, converted to
/// the effect entry's renderDeclaration.
///
/// The conversion applies to each vertex what the game applies per draw, so
/// that every batch draws with an identity world matrix and neutral material
/// transforms and batches of many models merge:
/// - positions get the material's position scale and offset (the shaders'
///   positionScale and positionOffset) and then the chunk's placement, in
///   world coordinates;
/// - normals and tangents get the placement's linear part, as the shaders
///   multiply them by WorldMatrix;
/// - every texture coordinate pair gets the material's offset and scale, as
///   every track vertex shader maps each pair it reads through uvOffsetScale
///   (verified on the effects' microcode: `mad oN, rK, c0.zw, c0.xy`).
/// Triangles wind clockwise seen from their front in world coordinates, as in
/// TileMesh.
struct ShadedMesh {
    static constexpr std::uint32_t kNoTexture = 0xFFFFFFFF;
    /// A sampler register the material leaves to the engine (its texture slot
    /// holds 0xFFFFFFFE in the model file): lightmaps it did not bake,
    /// shadow masks and the like.
    static constexpr std::uint32_t kEngineTexture = 0xFFFFFFFE;
    static constexpr std::size_t kSamplers = 16;
    /// Material constants a batch carries at most (Colorado's materials
    /// have up to 7).
    static constexpr std::size_t kMaterialConstants = 15;
    static constexpr std::uint32_t kMergedChunks = 0xFFFFFFFF;

    struct Batch {
        /// Index into TrackShaders.
        std::uint32_t shader = 0;
        /// Texture id per sampler register (the material's slot k feeds
        /// register k), kNoTexture past its slots.
        std::array<std::uint32_t, kSamplers> textures{};
        /// The material's constants (the second group of its table entry),
        /// which the effects name c360_N (pixel) and vc360_N (vertex) for
        /// constant N.
        std::vector<glm::vec4> constants;
        /// Byte offset of the batch's first vertex in ShadedMesh::vertices,
        /// a multiple of kVertexAlignment.
        std::size_t vertexOffset = 0;
        std::uint32_t vertexCount = 0;
        std::uint32_t stride = 0;
        /// Triangle list indices, relative to the batch's first vertex.
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        bool backdrop = false;
        /// For backdrop batches, the chunk the geometry comes from.
        std::uint32_t chunk = kMergedChunks;

        /// The real textures the batch reads, each once.
        std::vector<std::uint32_t> textureIds() const;
    };

    /// Every batch's vertices start at a multiple of this many bytes, so the
    /// renderer can bind each batch's range as a storage buffer.
    static constexpr std::size_t kVertexAlignment = 256;

    Bytes vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    int failedChunks = 0;
    std::vector<std::string> errors;
    /// Materials left out because their shader is not in TrackShaders or its
    /// declaration does not match the part's vertex size.
    int skippedMaterials = 0;

    std::size_t triangleCount() const { return indices.size() / 3; }
    std::vector<std::uint32_t> textureIds() const;
};

/// Decodes and merges `chunks` (indices into `index.chunks()`) for drawing
/// with the track's effects. Safe to call from several threads at once.
/// Without textures for a model, its batches carry kNoTexture.
ShadedMesh buildShadedMesh(const lzx::ZipArchive& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& chunks, const TrackShaders& shaders, const TrackTextures* textures);

} // namespace fh::world
