#pragma once

#include "base/Bytes.h"

#include <glm/mat4x4.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::carmodel {

/// A car render model file (`*.carbin`): a car's exterior or cockpit, a
/// wheel, or a brake rotor or caliper.
///
/// The game's object serialisation, big-endian, unpadded and unaligned.
/// Every nested object ("section") starts with a u32 version. Strings are a
/// u32 length and the bytes; arrays a section (u32 version 1), a u32 count
/// and the elements. Layout from the loader (CCarModelResource); verified:
/// every non-stripped carbin on the disc parses to its last byte. Facts
/// marked "inferred" come from the data alone.
///
///     preamble   depends on the kind of file (first u32 = its version):
///       car      u32 5, 256 bytes, f32[4], 48 bytes, f32[4], u32 n,
///                n × light (32 bytes: f32 x, y, z, s16 direction x, y, z,
///                9 bytes, 3 bytes, 2 bytes). Only the plain exterior
///                (`<car>.carbin`) has lights; the rest is zero in `_lod0`.
///       wheel    u32 3, s32, 64 bytes (f32[16]: widths and radii of the
///                rim and tyre, inferred)
///       brake    u32 1
///     header     u32 17, u32 16,
///                array of nodes (u32 2, string name, s32, f32[16],
///                f32[16]; the node layout is inferred: attachment points
///                such as exhaust tips, only in the plain exterior),
///                u32 3 + 256 bytes (steering wheel and hand positions in
///                cockpits, inferred), s32, s32,
///                15 × (f32[4], f32[4], f32),
///                f32 lighting minimum, f32 lighting range (see
///                Vertex::lighting),
///                s32, array of strings
///     model      u32 4,
///                draw lists: u32 4, u32 lodCount, lodCount × list of
///                translucent meshes, lodCount × list of opaque meshes; a
///                list is u32 n, n × (u16 part, u16 mesh)
///                array of parts
///                array of u32 1 + 16-byte objects (always empty here),
///                array of strings, array of strings (both always empty)
///     part       u32 5, f32[3] position, f32[3] minimum, f32[3] maximum
///                (the box is always centred on the position), f32 (1.0 but
///                for six parts, 0.1), f32[3] vertex centroid relative to
///                the position, f32 (0 to 1.5, unknown), glass section
///                (u32 1, array of 16-byte entries, array of u16), u8 flag,
///                s32 0,
///                mesh group: u32 1, string name, vertex buffer (LOD 1 and
///                up), array of meshes,
///                vertex buffer (LOD 0), u32 1, u8 hasMorphs, morph vertex
///                buffer, second morph vertex buffer
///     mesh       u32 3, u8 flag, f32[16] texture coordinate factors
///                (below), u32 2, string material name, u32 LOD level,
///                u32 primitive (Xenos: 4 triangle list, 6 triangle strip),
///                u32 0, u32 1 + f32[12] (offset 0, scale 1 and texture
///                transform 0 0 1 1 in every file), index buffer, u32 0
///     vertices   u32 3, u32 count, u32 stride, u32 flags (bit 0: a u32
///                hash follows, bit 1: the data is left out, as in the
///                `stripped_` files), count × stride bytes
///     indices    u32 4, u32 flags (as above), u32 count, u32 width (2 or
///                4), count × width bytes; strips restart at 0xFFFF or
///                0xFFFFFF
///
/// A mesh of LOD 0 indexes the part's LOD 0 vertex buffer, any other mesh
/// the first (verified). Material names are techniques of
/// `media/shaders/cars/shaders_v16.fxobj` (carmodel::techniqueName); the
/// separate `<name>.fxobj` effects are showroom only (`CustomCarShaders.xml`).
///
/// Positions are stored right-handed, front towards -Z; decodeVertex()
/// negates Z, leaving indices clockwise seen from the front, as
/// world::TileMesh.
struct CarBin {
    enum class Kind : std::uint8_t {
        /// Exterior or cockpit of a car (`media/cars/<car>/`).
        Car,
        /// A wheel with its tyre (`media/wheels/<name>/<name>.carbin`).
        Wheel,
        /// A brake rotor or caliper, one per corner.
        Brake,
    };

    struct VertexBuffer {
        std::uint32_t count = 0;
        std::uint32_t stride = 0;
        /// Set for the `stripped_` copies, whose vertex data is left out;
        /// `data` is empty then.
        bool stripped = false;
        Bytes data;

        bool empty() const { return count == 0; }
    };

    struct Mesh {
        /// The material, which names its shader.
        std::string name;
        /// 0 is the most detailed; wheels hold LODs 0 to 5 in one part.
        std::uint32_t lod = 0;
        /// Xenos primitive type: 4 triangle list, 6 triangle strip.
        std::uint32_t primitive = 6;
        /// Texture coordinates are quantised per mesh: a stored u16 pair
        /// (u, v) maps to (x + y · u / 65535, z + w · v / 65535). Verified
        /// on data: every mesh uses the full u16 range.
        glm::vec4 uv0{0.0F, 1.0F, 0.0F, 1.0F};
        glm::vec4 uv1{0.0F, 1.0F, 0.0F, 1.0F};
        /// The other eight floats of the factor block; mostly zero, unknown.
        std::array<float, 8> extraFactors{};
        /// The byte before the factors; unknown.
        bool factorFlag = false;
        /// From the model's draw lists: drawn in the translucent pass.
        bool translucent = false;
        /// Indices as stored, before strips are unrolled.
        std::uint32_t indexCount = 0;
        /// The index data is left out (`stripped_` files).
        bool indicesStripped = false;
        /// Triangle list into the part's vertex buffer for this mesh's LOD,
        /// degenerate triangles dropped, in file winding.
        std::vector<std::uint32_t> triangles;
    };

    struct Part {
        std::string name;
        /// Centre of the part's box, file space (see CarBin).
        glm::vec3 position{0.0F};
        /// Half the size of the box; the stored minimum is its negation.
        glm::vec3 halfExtent{0.0F};
        /// The float after the box; 1.0 in all but six parts (0.1).
        float boxScale = 1.0F;
        /// Mean of the vertex positions relative to `position`.
        glm::vec3 centroid{0.0F};
        /// The float after the centroid; unknown.
        float unknownValue = 0.0F;
        /// The byte after the glass section; mostly set on parts with glass,
        /// unknown.
        bool flag = false;
        /// Entries of the glass section's two arrays (16 bytes and 2 bytes
        /// each; shatter data, inferred, not decoded).
        std::uint32_t glassEntries = 0;
        std::uint32_t glassIndices = 0;
        std::vector<Mesh> meshes;
        /// Vertices of meshes with LOD 1 and up.
        VertexBuffer vertices;
        /// Vertices of LOD 0 meshes.
        VertexBuffer lod0Vertices;
        bool hasMorphs = false;
        /// Per-vertex damage data (4 bytes a vertex in cars, 16 in wheels);
        /// not decoded.
        VertexBuffer morph;
        VertexBuffer morphSecondary;

        const VertexBuffer& verticesFor(const Mesh& mesh) const;
    };

    /// A record of the car preamble's light list (plain exteriors only).
    struct Light {
        /// File space.
        glm::vec3 position{0.0F};
        /// Signed 16-bit normalised direction, file space.
        glm::vec3 direction{0.0F};
        /// The 14 bytes that follow; not decoded.
        std::array<std::uint8_t, 14> data{};
    };

    /// An element of the header's node array.
    struct Node {
        std::string name;
        std::int32_t index = 0;
        glm::mat4 first{1.0F};
        glm::mat4 second{1.0F};
    };

    /// A (part, mesh) pair of a draw list.
    struct DrawItem {
        std::uint16_t part = 0;
        std::uint16_t mesh = 0;
    };

    Kind kind = Kind::Car;
    std::vector<Light> lights;
    /// The wheel preamble's 16 floats (zero for other kinds).
    std::array<float, 16> wheelInfo{};
    std::vector<Node> nodes;
    /// The header's 256-byte block as 64 floats.
    std::array<float, 64> steeringInfo{};
    /// Minimum and range of the per-vertex lighting bytes (see Vertex).
    glm::vec2 lightingRange{0.0F, 1.0F};
    std::uint32_t lodCount = 0;
    /// Per LOD, translucent and opaque draw lists.
    std::vector<std::vector<DrawItem>> translucentDrawLists;
    std::vector<std::vector<DrawItem>> opaqueDrawLists;
    std::vector<Part> parts;

    std::size_t triangleCount() const;
    std::size_t vertexCount() const;
};

/// One vertex decoded from the 28-byte format every carbin uses:
///
///     0   s16[3] direction, s16 length   position = direction / 32767
///                                        · length / 32767 · |halfExtent|
///                                        + part position (verified; the
///                                        shaders' pack_partPosition)
///     8   u16[2] first texture coordinates (Mesh::uv0)
///     12  u16[2] second texture coordinates (Mesh::uv1)
///     16  s16[4] tangent frame as a quaternion (x, y, z, w) / 32767:
///                +X turns into the normal, +Y into the tangent along u
///                (verified)
///     24  s8[4]  lighting transfer: order-2 spherical harmonic
///                coefficients in D3DX order, each decompressed as
///                (b / 127 · 0.5 + 0.5) · range + minimum with the header's
///                lighting minimum and range (shCompressionFactors).
///                Verified.
struct Vertex {
    /// Car space (Z negated from the file).
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    glm::vec3 tangent{1.0F, 0.0F, 0.0F};
    /// Before the mesh's quantisation factors, 0 to 1.
    glm::vec2 rawUv0{0.0F};
    glm::vec2 rawUv1{0.0F};
    std::array<std::uint8_t, 4> lighting{};
};

namespace carbin {

inline constexpr std::uint32_t kVertexStride = 28;

/// Parses a whole file. `kind` must match the file (the preamble differs):
/// exteriors and cockpits are Car, `media/wheels` files are Wheel, calipers,
/// rotors and `media/brakes` files are Brake. Throws DataError, naming
/// `source`, on any structural mismatch or trailing data.
CarBin parse(ByteView data, const std::string& source, CarBin::Kind kind);

/// The kind a file's name implies, as above.
CarBin::Kind kindFromPath(std::string_view path);

/// Decodes vertex `index` of `buffer`, which belongs to `part`. Returns a
/// default vertex if the buffer's stride is not kVertexStride or the index is
/// out of range.
Vertex decodeVertex(const CarBin::Part& part, const CarBin::VertexBuffer& buffer, std::uint32_t index);

/// File space position of the stored vertex at `vertex` (28 bytes) of a
/// part whose position and half diagonal length are `packed` (xyz, w), as
/// the car vertex shaders compute it: direction · length · w + xyz.
glm::vec3 decodeFilePosition(const std::uint8_t* vertex, const glm::vec4& packed);

/// Texture coordinates of a decoded vertex through a mesh's factors.
glm::vec2 meshUv0(const CarBin::Mesh& mesh, const Vertex& vertex);
glm::vec2 meshUv1(const CarBin::Mesh& mesh, const Vertex& vertex);

/// Unrolls an index buffer into a triangle list, dropping degenerate
/// triangles and indices at or past `vertexCount` (the strip restarts).
/// `primitive` 4 is a list; anything else is read as strips, in which every
/// other triangle has its first two indices swapped back.
void appendTriangles(std::span<const std::uint32_t> indices, std::uint32_t primitive, std::uint32_t vertexCount,
    std::vector<std::uint32_t>& out);

} // namespace carbin
} // namespace openhorizon::carmodel
