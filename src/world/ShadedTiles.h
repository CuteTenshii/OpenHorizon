#pragma once

#include "TrackShaders.h"
#include "TrackTextures.h"
#include "WorldIndex.h"

#include "base/Bytes.h"

#include <glm/mat4x4.hpp>
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
///
/// Placed chunks whose effect reads vertex material constants
/// (TrackShaders::Entry::readsVertexMaterial) are the exception: their
/// positions, normals and tangents stay in the model file's coordinates (Z as
/// stored, position scale and offset applied), each chunk in batches of its
/// own, whose Batch::model the renderer draws them with. Their shaders move
/// the vertices in the model's space, where the game draws them: the flags'
/// vertex shader (anim_flag_diff_spec_norm_3_lm, vertex shader at 0x1770 of
/// its fxobj) turns positionScale times the position plus positionOffset
/// (slots 80, 81 and 95) by the sines and cosines of its wave (slots 97 to
/// 118), about the model's origin at the foot of the pole, and divides by the
/// position's x (slot 107), the distance from the pole in metres (0 to 2.8
/// in the flag models). Given world positions, hundreds of metres from the
/// origin, it stretched each flag into a sheet many metres wide.
struct ShadedMesh {
    static constexpr std::uint32_t kNoTexture = 0xFFFFFFFF;
    /// A sampler register the material leaves to the engine (its texture slot
    /// holds 0xFFFFFFFE in the model file): lightmaps it did not bake,
    /// shadow masks and the like.
    static constexpr std::uint32_t kEngineTexture = 0xFFFFFFFE;
    static constexpr std::size_t kSamplers = 16;
    /// Material constants a batch carries at most: the pixel shader's
    /// first, then the vertex shader's (see Batch::constants).
    static constexpr std::size_t kMaterialConstants = 15;
    /// Where the vertex shader's constants start in Batch::constants, and
    /// the register the first of them loads (see
    /// RenderMesh::MaterialInfo::vertexConstants). Colorado's materials
    /// have up to 7 of each kind (pixel c0 to c6, vertex c3 to c9).
    static constexpr std::size_t kFirstVertexConstant = 8;
    static constexpr std::uint32_t kFirstVertexRegister = 3;
    static constexpr std::uint32_t kMergedChunks = 0xFFFFFFFF;

    struct Batch {
        /// Index into TrackShaders.
        std::uint32_t shader = 0;
        /// Texture id per sampler register (the material's slot k feeds
        /// register k), kNoTexture past its slots.
        std::array<std::uint32_t, kSamplers> textures{};
        /// The material's constants: the pixel shader's (the second group of
        /// its table entry), which the effects name c360_N for constant N,
        /// and from kFirstVertexConstant the vertex shader's (the first
        /// group), which they name vc360_N (c360_N in the bunting effects)
        /// for constant N - kFirstVertexRegister. Only as long as the
        /// material needs.
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
        /// Takes the batch's vertices to world coordinates: the identity for
        /// batches already in them, and for a chunk kept in its model's
        /// space (see ShadedMesh) its placement (Placement::apply()).
        glm::mat4 model{1.0F};
        /// The batch's indices chunk by chunk, in order, covering exactly
        /// [firstIndex, firstIndex + indexCount); firstIndex is ShadedMesh's.
        std::vector<ChunkRun> runs;

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
