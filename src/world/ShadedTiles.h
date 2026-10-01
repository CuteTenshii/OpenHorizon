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

namespace openhorizon::world {

/// Chunks for the game's track shaders: one batch per surface, vertices in the effect's renderDeclaration.
/// Vertices get the per-draw transforms baked in (position scale/offset, placement, uvOffsetScale; verified on
/// the microcode: `mad oN, rK, c0.zw, c0.xy`), so batches draw with identity matrices and merge. Winding as TileMesh.
///
/// Exception: placed chunks whose effect reads vertex material constants (readsVertexMaterial) stay in model
/// space, one batch each, drawn with Batch::model; e.g. the flag shader (anim_flag_diff_spec_norm_3_lm, 0x1770
/// of its fxobj) divides by position x, the distance from the pole in metres.
struct ShadedMesh {
    static constexpr std::uint32_t kNoTexture = 0xFFFFFFFF;
    /// A sampler register left to the engine (lightmaps, shadow masks); the model file's slot value.
    static constexpr std::uint32_t kEngineTexture = 0xFFFFFFFE;
    static constexpr std::size_t kSamplers = 16;
    /// Most material constants a batch carries: the pixel shader's, then the vertex shader's.
    static constexpr std::size_t kMaterialConstants = 15;
    /// Where the vertex constants start in Batch::constants, and the register the first loads. Colorado's
    /// materials have up to 7 of each kind (pixel c0-c6, vertex c3-c9).
    static constexpr std::size_t kFirstVertexConstant = 8;
    static constexpr std::uint32_t kFirstVertexRegister = 3;
    static constexpr std::uint32_t kMergedChunks = 0xFFFFFFFF;

    struct Batch {
        /// Index into TrackShaders.
        std::uint32_t shader = 0;
        /// Texture id per sampler register, kNoTexture past the material's slots.
        std::array<std::uint32_t, kSamplers> textures{};
        /// The pixel shader's constants (c360_N), then from kFirstVertexConstant the vertex shader's (vc360_N,
        /// c360_N in bunting) offset by kFirstVertexRegister. Only as long as the material needs.
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
        /// Identity, or for a chunk kept in model space its placement (Placement::apply()).
        glm::mat4 model{1.0F};
        /// Chunk by chunk, covering exactly [firstIndex, firstIndex + indexCount); firstIndex is ShadedMesh's.
        std::vector<ChunkRun> runs;

        /// The real textures the batch reads, each once.
        std::vector<std::uint32_t> textureIds() const;
    };

    /// Batch vertex alignment, so each batch's range binds as a storage buffer.
    static constexpr std::size_t kVertexAlignment = 256;

    Bytes vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    int failedChunks = 0;
    std::vector<std::string> errors;
    /// Materials whose shader is missing or whose declaration does not match the vertex size.
    int skippedMaterials = 0;

    std::size_t triangleCount() const { return indices.size() / 3; }
    std::vector<std::uint32_t> textureIds() const;
};

/// Thread-safe. Without textures for a model, its batches carry kNoTexture.
ShadedMesh buildShadedMesh(const lzx::ZipArchive& archive, const WorldIndex& index,
    const std::vector<std::uint32_t>& chunks, const TrackShaders& shaders, const TrackTextures* textures);

} // namespace openhorizon::world
