#pragma once

#include "Effect.h"
#include "Microcode.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

/// Translation of Xenos microcode to GLSL 4.50.
///
/// The generated code follows the hardware model instead of reconstructing
/// HLSL: temporaries are a `vec4 r[]` array, the previous scalar result, the
/// predicate, a0 and aL are variables, and every ALU slot evaluates its
/// sources before writing its vector and scalar results. Multiplications use
/// the Direct3D 9 rule that zero times anything (including infinity) is
/// zero, which the game's normalisations rely on.
///
/// Interface of the generated shaders (the same in every program, so the
/// renderer can bind once per frame):
///
///  - `uniform XeVertexConstants { vec4 c[256]; }` at UBO binding 0 and
///    `uniform XePixelConstants { vec4 c[256]; }` at binding 1: the float
///    constants c0-c255 of each stage (std140). Literal constants the
///    shader defines are folded into the code wherever they are addressed
///    directly; relative addressing reads the block, so the renderer should
///    also upload the literals (see ShaderProgramSource).
///  - `uniform XeControlConstants { uvec4 bools[2]; uvec4 loops[8]; }` at
///    binding 2: the 256 boolean constants as bits (vertex b# at bit #,
///    pixel b# at bit 128 + #, verified against the game's constant tables
///    and control flow) and the 32 loop constants (count in bits 0-7, start
///    in 8-15, signed step in 16-23).
///  - `uniform XeSystemConstants` at binding 3 (see kSystemBlock in the
///    source; documented on SystemConstants in ShaderProgram.h): the clip
///    space remap, vertex stream base offsets, screen size for pixel
///    positions, alpha test and output gamma.
///  - Vertex data: `readonly buffer XeVertexStream<s> { uint xe_vb<s>[]; }`
///    at SSBO binding s for stream s. The game's vertex buffers are bound
///    unchanged, byte for byte, and each fetch decodes its format, endian
///    swap, signedness and normalisation in GLSL. This is chosen over GL
///    vertex attributes because the Xenos formats include ones GL cannot
///    express as attributes (big-endian 16-bit and 10-bit data, 10_11_11),
///    because fetches index with any register (not only the vertex id) and
///    because the declaration-to-fetch binding the Direct3D runtime performs
///    is then resolved once, at translation, from the effect's own
///    declaration.
///  - Textures: `sampler2D xe_tex<n>` (1D and 2D fetches), `sampler3D
///    xe_tex<n>_3d`, `samplerCube xe_tex<n>_cube` at texture unit n, the
///    fetch constant index. Pixel shader sampler s# is fetch constant #,
///    vertex shader sampler s# fetch constant 16 + # (both verified against
///    the constant tables; the game has one vertex texture, s1 at tf17).
///    Filter overrides inside fetch instructions are
///    not applied (texture and sampler objects decide), and 1D textures are
///    read as Nx1 2D ones.
///  - Varyings: `layout(location = L) out/in vec4 xe_var<L>`; which
///    interpolator register goes to which location is an option, so a
///    program can link a vertex and pixel shader by semantic.
///  - Pixel outputs: `layout(location = n) out vec4 xe_color<n>` for each
///    colour export, and gl_FragDepth when the shader exports depth.
namespace fh::xenos {

/// Where one vertex fetch reads its data, as the Direct3D runtime would bind
/// it from a vertex declaration.
struct VertexFetchLayout {
    /// SSBO binding (the vertex stream).
    std::uint32_t stream = 0;
    std::uint32_t byteOffset = 0;
    std::uint32_t byteStride = 0;
    VertexFormat format = VertexFormat::Undefined;
    /// 0 none, 1 8-in-16, 2 8-in-32, 3 16-in-32.
    std::uint32_t endian = 2;
    bool isSigned = false;
    /// Integer (unnormalized) data.
    bool integer = false;
    /// Signed normalized data uses (2x+1)/(2^n-1) instead of clamping.
    bool signedNoZero = false;
    std::int32_t expAdjust = 0;
    /// Declaration selectors: 0-3 fetched component, 4 zero, 5 one.
    std::array<std::uint8_t, 4> selectors{0, 1, 2, 3};
};

/// The layout Direct3D would give a fetch of `element` with `stride`.
VertexFetchLayout layoutFromDeclaration(const VertexDeclElement& element, std::uint32_t stride);

struct TranslationOptions {
    /// Vertex shaders: layout per fetch slot. Slots not listed use the
    /// format, offset and stride encoded in the instruction, on stream
    /// 95 - fetch constant index (Direct3D puts stream s at constant 95 - s).
    std::map<std::uint32_t, VertexFetchLayout> vertexFetches;
    /// Vertex shaders: varying location of each interpolator export
    /// register, -1 to drop it. Identity by default.
    std::array<int, 16> exportLocations{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    /// Pixel shaders: varying location feeding each interpolator register,
    /// -1 for registers that start at zero.
    std::array<int, 16> inputLocations{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    /// Pixel shaders: register receiving the pixel parameters.
    std::optional<std::uint32_t> parameterRegister;
    /// Literal float constants to fold, by register.
    std::map<std::uint32_t, std::array<float, 4>> literals;
    /// Names for comments, e.g. the constant table's sampler names by slot.
    std::map<std::uint32_t, std::string> samplerNames;
};

/// Texture fetch constant (and texture unit) of sampler register
/// `samplerRegister` of a shader of `stage`.
std::uint32_t samplerFetchConstant(ShaderStage stage, std::uint32_t samplerRegister);

/// Options derived from a parsed shader: its literals, its interpolators
/// (at locations equal to their registers), its pixel parameter register,
/// its sampler names and, for vertex shaders, fetch layouts from
/// `declaration` when given.
TranslationOptions defaultOptions(const Shader& shader, const VertexDeclaration* declaration);

struct SamplerUse {
    std::uint32_t slot = 0;
    TextureDimension dimension = TextureDimension::D2;
    std::string glslName;
};

struct TranslationResult {
    std::string glsl;
    std::vector<SamplerUse> samplers;
    /// Vertex streams (SSBO bindings) read.
    std::vector<std::uint32_t> streams;
    /// Pixel shaders: colour outputs written (bit n = target n).
    std::uint32_t colorOutputs = 0;
    bool writesDepth = false;
    bool usesKill = false;
    /// Varying locations written (vertex) or read (pixel).
    std::vector<int> varyings;
    /// Instructions that could not be translated faithfully, e.g.
    /// "vector opcode 30" or "memory export"; the code is still valid GLSL.
    std::vector<std::string> unsupported;
};

/// Translates a decoded program. Throws DataError only for microcode that
/// cannot be expressed at all (e.g. an exec past the end).
TranslationResult translate(const Program& program, const TranslationOptions& options);

/// Decodes and translates `shader` with defaultOptions().
TranslationResult translate(const Shader& shader, const VertexDeclaration* declaration = nullptr);

/// UBO and SSBO binding points of the generated code.
namespace binding {
inline constexpr std::uint32_t kVertexConstants = 0;
inline constexpr std::uint32_t kPixelConstants = 1;
inline constexpr std::uint32_t kControlConstants = 2;
inline constexpr std::uint32_t kSystemConstants = 3;
} // namespace binding

} // namespace fh::xenos
