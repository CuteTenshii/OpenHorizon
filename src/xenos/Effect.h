#pragma once

#include "Microcode.h"

#include "base/Bytes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The game's compiled effects (`.fxobj`): Xbox 360 D3DX effects holding
/// techniques, passes and Xenos shaders.
///
/// All values are big-endian. Layout as decoded from the files (every one of
/// the 371 effects on the disc and in the track archives parses this way;
/// "inferred" marks fields whose meaning is read from the data rather than
/// from documentation):
///
///     0x00  u32 0x101, u32 hash, u32 effect data size, u32 ?,
///           u32 vertex declaration count, u32 ?,
///           u32 file size - 0x1C, then variable data
///     ...   u32 0xA3D70141 (found by scanning), u32 block size, u32 ?;
///           the block data D starts after these 12 bytes and every offset
///           below is relative to D
///     D+0x200  u32 offset of the technique pointer array
///     D+0x20C  u32 technique count
///
///     technique  u32 name, u32 pass count, u32 ?, u32 ?, then one u32
///                offset per pass
///     pass       u32 name, u32 ?, u32 state record, u32 ?, u32 ?
///     state rec  17 u32 offsets, u32 0xFFFFFFFF, u32 vertex shader object,
///                u32 pixel shader object, u32 next record
///     shader obj u32 0, u32 blob size (0 for "no shader"), blob
///
/// After the block (at D + block size) come the vertex declarations the
/// effect was compiled for, each a 0x34-byte header (u32 0x00100005, u32 1,
/// 3 × u32 0, u32 0xFFFF0000, u32 element count, u32 0, u32 0xFF000000,
/// 4 × u32 0), 12-byte elements (u16 stream, u16 byte offset, u32 type,
/// u8 method, u8 usage, u8 usage index, u8 0) and u32 0; then the element
/// names (the HLSL input members) as NUL-terminated strings in order.
///
/// A shader blob (verified against the constant tables and microcode):
///
///     0x00  u32 0x102A1101 (vertex) or 0x102A1100 (pixel)
///     0x04  u32 virtual size (header, tables, info)
///     0x08  u32 physical size (literal constants and microcode), stored
///           right after the virtual part
///     0x0C  u32 debug info offset or 0: u32 0, u32 length, path text
///     0x10  u32 constant table offset: u32 size, then a Direct3D constant
///           table without its "CTAB" tag
///     0x14  u32 literal constant table offset or 0: u32 0, u32 mask of
///           4-register groups, 2 × u32 0, u32 size, then 8-byte entries
///           (u16 register, 0-255 vertex / 256-511 pixel, u16 float count,
///           u32 offset into the physical part) and 12 zero bytes
///     0x18  u32 shader info offset
///
/// Shader info, u32 words: microcode offset in the physical part, microcode
/// size in bytes, SQ_PROGRAM_CNTL-style program control (register count in
/// bits 0-5 / 8-13, vertex export count - 1 in bits 20-23, parameter
/// generation in bit 18: inferred), SQ_CONTEXT_MISC-style word (pixel
/// parameter register in bits 8-15, inferred), ?, interpolator layout hash.
/// Vertex shaders continue with ?, vertex element count E, export patch
/// count P, u32 0x290, E element words (fetch slot in bits 0-11, usage in
/// 12-15, usage index in 16-19), the interpolator words, and P patch words.
/// Pixel shaders continue with 2 × u32 ? and the interpolator words.
/// Interpolator words hold usage index in bits 0-3, usage in 4-7, the
/// interpolator register in 8-11 and the component mask in 12-15.
namespace fh::xenos {

/// Direct3D declaration usages.
enum class DeclUsage : std::uint8_t {
    Position = 0,
    BlendWeight = 1,
    BlendIndices = 2,
    Normal = 3,
    PointSize = 4,
    Texcoord = 5,
    Tangent = 6,
    Binormal = 7,
    TessFactor = 8,
    PositionT = 9,
    Color = 10,
    Fog = 11,
    Depth = 12,
    Sample = 13,
};

std::string_view usageName(std::uint32_t usage);

/// Register sets of the constant table.
enum class RegisterSet : std::uint8_t {
    Bool = 0,
    Int4 = 1,
    Float4 = 2,
    Sampler = 3,
};

std::string_view registerSetName(RegisterSet set);

/// One constant of a shader's constant table.
struct ConstantInfo {
    std::string name;
    RegisterSet set = RegisterSet::Float4;
    /// First register of the set (c#, i#, b#, s#) as the shader sees it.
    std::uint16_t registerIndex = 0;
    std::uint16_t registerCount = 0;
    /// D3DXPARAMETER_CLASS: 0 scalar, 1 vector, 2 row matrix, 3 column
    /// matrix, 4 object, 5 struct.
    std::uint16_t parameterClass = 0;
    /// D3DXPARAMETER_TYPE: 1 bool, 2 int, 3 float, 10-14 samplers
    /// (sampler, 1D, 2D, 3D, cube).
    std::uint16_t parameterType = 0;
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    std::uint16_t elements = 0;
    std::uint16_t structMembers = 0;
    /// Default value (four floats per float register), empty if none.
    std::vector<float> defaultValue;

    std::string_view typeName() const;
};

/// A shader's Direct3D constant table.
struct ConstantTable {
    std::string creator;
    std::string target;
    std::uint32_t version = 0;
    std::vector<ConstantInfo> constants;

    const ConstantInfo* find(std::string_view name) const;
    /// The constant of `set` covering register `index`, or nullptr.
    const ConstantInfo* atRegister(RegisterSet set, std::uint32_t index) const;
};

/// Parses a constant table whose header starts at `table` (the u32 28 that
/// follows the "CTAB" tag in Direct3D). Throws DataError.
ConstantTable parseConstantTable(ByteView table, const std::string& source);

/// A vertex shader input: the fetch instruction at `fetchSlot` reads the
/// declaration element with this usage.
struct VertexElementUse {
    std::uint32_t raw = 0;
    std::uint32_t fetchSlot = 0;
    std::uint32_t usage = 0;
    std::uint32_t usageIndex = 0;
};

/// One interpolator of a shader's interface.
struct Interpolator {
    std::uint32_t raw = 0;
    std::uint32_t usage = 0;
    std::uint32_t usageIndex = 0;
    /// Interpolator register: the vertex shader's export register and the
    /// pixel shader's input temporary.
    std::uint32_t reg = 0;
    std::uint32_t mask = 0;
};

/// Literal float constants the shader expects in its constant registers.
struct LiteralConstant {
    /// Register as the microcode addresses it (0-255 in both stages).
    std::uint32_t reg = 0;
    std::array<float, 4> value{};
};

/// One shader of an effect.
struct Shader {
    ShaderStage stage = ShaderStage::Vertex;
    /// Offset of the blob in the effect file.
    std::size_t fileOffset = 0;
    std::string debugPath;
    ConstantTable constants;
    std::vector<LiteralConstant> literals;
    std::vector<std::uint32_t> microcode;
    std::uint32_t programControl = 0;
    std::uint32_t contextMisc = 0;
    std::uint32_t interfaceHash = 0;
    /// The info words that are not decoded, for display.
    std::vector<std::uint32_t> unknownInfo;
    std::vector<VertexElementUse> vertexElements;
    std::vector<Interpolator> interpolators;
    /// Vertex shaders: slots the Direct3D runtime patches when linking.
    std::vector<std::uint32_t> exportPatches;

    /// Pixel shaders with parameter generation (screen position and facing,
    /// VPOS/VFACE): the register receiving it; nothing otherwise.
    std::optional<std::uint32_t> parameterRegister() const;
};

/// Parses one shader blob (starting at the 0x102A110x magic). Throws
/// DataError.
Shader parseShader(ByteView blob, const std::string& source);

/// One element of a vertex declaration.
struct VertexDeclElement {
    std::uint16_t stream = 0;
    std::uint16_t offset = 0;
    /// Xbox 360 D3DDECLTYPE: GPU vertex format in bits 0-5, endian swap in
    /// 6-7 (1 = 8-in-16, 2 = 8-in-32), signed in bit 8, integer in bit 9
    /// and the component selectors (3 bits each, x first; 4 = 0, 5 = 1) in
    /// bits 10-21. Decoded from the type values of the declarations
    /// (FLOAT3 0x2A23B9, USHORT2N 0x2C2059, DEC3N 0x1A2187, SHORT4N
    /// 0x1A215A, D3DCOLOR 0x182886), whose selectors give xyz1, xy01,
    /// xyzw and zyxw.
    std::uint32_t type = 0;
    std::uint8_t method = 0;
    std::uint8_t usage = 0;
    std::uint8_t usageIndex = 0;
    std::string name;

    VertexFormat format() const { return static_cast<VertexFormat>(type & 0x3F); }
    std::uint32_t endian() const { return (type >> 6) & 3; }
    bool isSigned() const { return ((type >> 8) & 1) != 0; }
    bool isInteger() const { return ((type >> 9) & 1) != 0; }
    /// Selector for component `i`: 0-3 fetched component, 4 zero, 5 one.
    std::uint32_t selector(std::uint32_t i) const { return (type >> (10 + 3 * i)) & 7; }
    std::uint32_t byteSize() const { return vertexFormatBytes(format()); }
};

struct VertexDeclaration {
    std::vector<VertexDeclElement> elements;

    /// Bytes per vertex of `stream`: the end of its last element, rounded
    /// up to four bytes.
    std::uint32_t stride(std::uint16_t stream) const;
    const VertexDeclElement* find(std::uint32_t usage, std::uint32_t usageIndex) const;
};

struct Pass {
    std::string name;
    /// Index into Effect::shaders, or -1 when the pass has no such shader.
    int vertexShader = -1;
    int pixelShader = -1;
};

struct Technique {
    std::string name;
    std::vector<Pass> passes;
};

struct Effect {
    std::vector<Technique> techniques;
    std::vector<Shader> shaders;
    std::vector<VertexDeclaration> declarations;

    const Technique* findTechnique(std::string_view name) const;
    /// The declaration providing every input of vertex shader `shader`: the
    /// first whose elements cover all its usages; nullptr if none does.
    const VertexDeclaration* declarationFor(const Shader& shader) const;
    /// The declaration to bind vertex shader `shader` with: declarationFor()
    /// if there is one; otherwise one built from the shader's inputs in
    /// fetch order, packed without gaps on stream 0, each with the type the
    /// effect's other declarations give its usage. The effects of the
    /// skinned driver-hand shaders need this: their declarations cover only
    /// the unskinned and shadow inputs, and packing reproduces the full
    /// skinned layout another effect declares (inferred). Nothing if an
    /// input's usage appears in no declaration.
    std::optional<VertexDeclaration> inputDeclaration(const Shader& shader) const;
};

/// Parses a whole `.fxobj`. Throws DataError naming `source`.
Effect parseEffect(ByteView data, const std::string& source);

inline constexpr std::uint32_t kVertexShaderMagic = 0x102A1101;
inline constexpr std::uint32_t kPixelShaderMagic = 0x102A1100;

} // namespace fh::xenos
