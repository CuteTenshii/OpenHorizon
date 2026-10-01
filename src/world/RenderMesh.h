#pragma once

#include "base/Bytes.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::world {

/// A render model file (`*.rmb.bin` in a track's bin.zip).
///
/// Layout, big-endian, as far as it is decoded:
///
///     0x00  u32 6
///     0x04  f32 min x, y, z, pad       bounding box
///     0x14  f32 max x, y, z, pad
///     0x24  f32[16]                     transform (identity in every file seen)
///     0x74  u32 part count
///     0xB0  parts
///
///     part      u32 nameLength, name, u32 3, u32 vertexCount, u32 stride,
///               u32 0, vertices (each starts with f32 x, y, z), u32 1,
///               u32 materialCount, u32 1, materials; between two parts a
///               48-byte bounds block and u32 1
///     material  u32 2, u32 nameLength, name, u32 0, u32 ?, u32 table index,
///               u32 1, 48 bytes (f32[4] position offset, f32[4] position
///               scale, f32[4] texture coordinate offset u, v and scale u, v),
///               u32 4, u32 0, u32 indexCount, u32 indexWidth (2 or 4),
///               indices, u32 endTag, u32 1
///     tail      u32 1, 1, 1, u32 count, per table entry: u32 3, u32 shader,
///               u32 0, twice (u32 1, u32 n, n × f32[4] shader constants:
///               the vertex shader's, then the pixel shader's), u32 1,
///               u32 slotCount, slotCount × s32 texture slot (-2:
///               supplied by the engine); then u32 1, u32 shaderCount, shader
///               paths (u32 length, text), then compiled shader code
///
/// After the position, a vertex holds 4-byte inputs in its shader's input order (see ShaderLayout); texture
/// coordinates are u16 fractions mapped through the material's offset and scale. Positions store Z negated.
/// Index buffers are strips with all-ones restarts; one without restarts and a multiple of three long is a
/// triangle list (inferred).
struct RenderMesh {
    struct Material {
        std::string name;
        /// Triangle list indices into the part's positions.
        std::vector<std::uint32_t> triangles;
        /// Index into RenderMesh::materialTable.
        std::uint32_t tableIndex = 0;
        /// Texture coordinate offset (x, y) and scale (z, w).
        glm::vec4 uvOffsetScale{0.0F, 0.0F, 1.0F, 1.0F};
        /// The track shaders' positionOffset and positionScale: position * scale + offset before the world matrix,
        /// in file coordinates (Z not negated).
        glm::vec3 positionOffset{0.0F};
        glm::vec3 positionScale{1.0F};
    };
    struct Part {
        std::string name;
        std::uint32_t stride = 0;
        /// World-space positions (Z already flipped).
        std::vector<glm::vec3> positions;
        /// The vertices as stored, `stride` bytes each.
        Bytes vertexData;
        std::vector<Material> materials;
    };
    /// Shader and textures of a material, from the table after the parts.
    struct MaterialInfo {
        /// Index into RenderMesh::shaders.
        std::uint32_t shader = 0;
        /// Per sampler register, an index into the model's texture list, or -1 for engine textures (lightmaps,
        /// shadows).
        std::vector<int> textureSlots;
        /// The pixel shader's constants from c0 (the effects' c360_0). Blended ground shaders hold their layer
        /// texture scales in the first two: Blend_A, Blend_B, then Blend_C and the splat map. Inferred.
        std::vector<glm::vec4> constants;
        /// The vertex shader's constants from c3 (vc360_3, c360_3 in bunting; c0-c2 are the material transforms),
        /// e.g. lake waves (amplitude in metres, phase speed) and flag sway. Verified on Colorado's models.
        std::vector<glm::vec4> vertexConstants;
    };

    /// World-space bounding box (Z already flipped).
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    std::vector<Part> parts;
    /// Empty when the file's tail has an unknown layout; the geometry is still usable.
    std::vector<MaterialInfo> materialTable;
    /// Shader paths, e.g. "shaders\track\h_diff_1.fx".
    std::vector<std::string> shaders;

    std::size_t triangleCount() const;

    /// The pair at byte `offset` mapped through `material`'s offset and scale; (0, 0) outside the vertex.
    static glm::vec2 texcoord(const Part& part, const Material& material, std::uint32_t vertex, int offset);
    /// The pair at byte `offset` as stored, in fractions of 1; (0, 0) outside the vertex.
    static glm::vec2 rawTexcoord(const Part& part, std::uint32_t vertex, int offset);
    /// The vertex colour at byte `offset`, big-endian, or 0 outside the vertex.
    static std::uint32_t colour(const Part& part, std::uint32_t vertex, int offset);
};

/// Enough of a render model to index a world without decoding whole files.
struct RenderMeshHeader {
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    std::uint32_t partCount = 0;
    /// Name of the first part, e.g. "Redstone_Area02_Road_LOD02_01".
    std::string firstPartName;
};

namespace rendermesh {

/// Bytes from the start of a file that readHeader() needs.
inline constexpr std::size_t kHeaderBytes = 0x200;

/// Throws DataError, naming `source`, on any structural mismatch.
RenderMesh parse(ByteView data, const std::string& source);

/// Nothing if the data is not a render model.
std::optional<RenderMeshHeader> readHeader(ByteView prefix);

/// Appends one index buffer to `out` as a triangle list, dropping degenerates. `restart` is the all-ones value of
/// the index width; strips get every other triangle's winding flipped back.
void appendTriangles(std::span<const std::uint32_t> indices, std::uint32_t restart, std::vector<std::uint32_t>& out);

/// "..._LOD01_..." gives 1; -1 without a token (e.g. "NOLOD"). The token starts the name or follows an underscore,
/// any case.
int lodLevel(std::string_view partName);

/// Lower-case name without LOD token and chunk numbers: "Redstone_Area02_Road_LOD01_02" gives
/// "redstone_area02_road". Each LOD of a group covers the same area, cut into different chunks.
std::string lodGroupKey(std::string_view partName);

} // namespace rendermesh
} // namespace openhorizon::world
