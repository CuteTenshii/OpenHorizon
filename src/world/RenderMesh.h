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
/// After the position, a vertex holds 4-byte inputs in the order of its
/// shader's input table (see ShaderLayout). Texture coordinates are pairs of
/// unsigned 16-bit fractions that the vertex shader maps through the
/// material's offset and scale.
///
/// Positions are stored with Z negated relative to the world (and to the
/// placements in CollObjs.xml), so this parser flips Z. Index buffers are
/// triangle strips with all-ones restarts; a buffer without restarts whose
/// length is a multiple of three is a triangle list. That last rule is
/// inferred from the data, not from a format definition.
struct RenderMesh {
    struct Material {
        std::string name;
        /// Triangle list indices into the part's positions.
        std::vector<std::uint32_t> triangles;
        /// Index into RenderMesh::materialTable.
        std::uint32_t tableIndex = 0;
        /// Texture coordinate offset (x, y) and scale (z, w).
        glm::vec4 uvOffsetScale{0.0F, 0.0F, 1.0F, 1.0F};
        /// What the track shaders apply to a stored position before the
        /// world matrix (their positionOffset and positionScale constants):
        /// position * scale + offset, in the file's coordinates (Z not yet
        /// negated).
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
        /// Per texture slot, an index into the model's texture list or -1
        /// for textures the engine supplies (lightmaps, shadows). Slot k
        /// feeds the shader's sampler register k.
        std::vector<int> textureSlots;
        /// The second group of shader constants: the pixel shader's, from
        /// c0 (the effects' c360_0 on). For the blended ground shaders the
        /// first two hold the texture scales of their layers: Blend_A and
        /// Blend_B in the first (x, y and z, w), Blend_C and the splat map
        /// in the second. Inferred from the data.
        std::vector<glm::vec4> constants;
        /// The first group of shader constants: the vertex shader's, from
        /// c3 (the effects' vc360_3 on, c360_3 in the bunting effects; c0
        /// to c2 are the material's texture coordinate and position
        /// transforms). Verified over the 322940 table entries of
        /// Colorado's models: the group holds exactly as many constants as
        /// the effect's highest vertex-stage material register less 2, and
        /// none for effects without one; the second group covers the pixel
        /// stage's registers from c0. Such as the waves of the lakes
        /// (vc360_3: amplitude in metres, phase speed; (0.05, 1, 0, 0) on
        /// Colorado's rivers), the sway of flags and trees. Where the
        /// executable uploads them was not traced.
        std::vector<glm::vec4> vertexConstants;
    };

    /// World-space bounding box (Z already flipped).
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    std::vector<Part> parts;
    /// Empty when the file's tail does not have the expected layout; the
    /// geometry is still usable then.
    std::vector<MaterialInfo> materialTable;
    /// Shader paths, e.g. "shaders\track\h_diff_1.fx".
    std::vector<std::string> shaders;

    std::size_t triangleCount() const;

    /// Texture coordinates of vertex `vertex` of `part`, read from the pair
    /// at byte `offset` and mapped through `material`'s offset and scale.
    /// Returns (0, 0) when the pair lies outside the vertex.
    static glm::vec2 texcoord(const Part& part, const Material& material, std::uint32_t vertex, int offset);
    /// The texture coordinate pair at byte `offset` of vertex `vertex` as
    /// stored, as 16-bit fractions of 1; (0, 0) outside the vertex.
    static glm::vec2 rawTexcoord(const Part& part, std::uint32_t vertex, int offset);
    /// The four bytes at byte `offset` of vertex `vertex` (a vertex colour)
    /// read as a big-endian number, or 0 outside the vertex.
    static std::uint32_t colour(const Part& part, std::uint32_t vertex, int offset);
};

/// What the first bytes of a render model say about it, enough to index a
/// world without decoding whole files.
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

/// Parses a whole file; throws DataError, naming `source`, on any structural
/// mismatch.
RenderMesh parse(ByteView data, const std::string& source);

/// Parses the header and first part name from the start of a file; returns
/// nothing if the data is not a render model.
std::optional<RenderMeshHeader> readHeader(ByteView prefix);

/// Appends the triangles of one index buffer to `out` as a triangle list,
/// dropping degenerate ones. `restart` is the all-ones value of the index
/// width. A buffer without restarts whose length is a multiple of three is
/// read as a list; anything else as strips, in which every other triangle
/// has its winding flipped back.
void appendTriangles(std::span<const std::uint32_t> indices, std::uint32_t restart, std::vector<std::uint32_t>& out);

/// Level of detail from a part name ("..._LOD01_..." gives 1), or -1 when the
/// name carries none (e.g. "NOLOD" meshes). The token must start the name or
/// follow an underscore; letters match ignoring case.
int lodLevel(std::string_view partName);

/// The part name without its LOD token and trailing chunk numbers, in lower
/// case: "Redstone_Area02_Road_LOD01_02" and "Redstone_Area02_Road_LOD02_11"
/// both give "redstone_area02_road". Each LOD level of such a group covers the
/// same area, though its chunks are cut differently.
std::string lodGroupKey(std::string_view partName);

} // namespace rendermesh
} // namespace openhorizon::world
