#pragma once

#include "Effect.h"
#include "GlslTranslator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// What the renderer needs to draw with one pass of a game effect: GLSL for
/// both stages and where every named constant, texture and vertex input
/// goes. See GlslTranslator.h for the binding points of the generated code.
namespace openhorizon::xenos {

/// Where a named constant of the effect lives.
struct ConstantBinding {
    std::string name;
    ShaderStage stage = ShaderStage::Vertex;
    RegisterSet set = RegisterSet::Float4;
    /// Float4: vec4 index in the stage's constant block (binding 0 for
    /// vertex, 1 for pixel). Bool: bit index in XeControlConstants.bools
    /// (vertex b# at #, pixel b# at 128 + #). Int4: loop constant index
    /// (vertex i# at #, pixel i# at 16 + #; inferred, the game declares no
    /// integer constants). Sampler: texture unit (pixel s# at #, vertex s#
    /// at 16 + #).
    std::uint32_t index = 0;
    std::uint32_t count = 0;
    std::uint16_t parameterClass = 0;
    std::uint16_t rows = 0;
    std::uint16_t columns = 0;
    /// Default from the constant table (floats), empty if none.
    std::vector<float> defaultValue;
};

struct SamplerBinding {
    /// Constant table name, e.g. "Diffuse_TextureSampler"; empty when the
    /// table does not name the fetch constant.
    std::string name;
    ShaderStage stage = ShaderStage::Pixel;
    /// Texture unit (the fetch constant index).
    std::uint32_t unit = 0;
    TextureDimension dimension = TextureDimension::D2;
    std::string glslName;
};

/// One vertex input as the game's vertex buffers store it.
struct VertexInputElement {
    std::string name;
    std::uint32_t usage = 0;
    std::uint32_t usageIndex = 0;
    std::uint32_t stream = 0;
    std::uint32_t byteOffset = 0;
    /// Xbox 360 D3DDECLTYPE (see VertexDeclElement).
    std::uint32_t type = 0;
    VertexFormat format = VertexFormat::Undefined;
};

struct VertexInputLayout {
    std::vector<VertexInputElement> elements;
    /// Byte stride of stream s at index s.
    std::vector<std::uint32_t> strides;
};

struct ShaderProgramSource {
    std::string vertexGlsl;
    std::string fragmentGlsl;
    std::vector<ConstantBinding> constants;
    std::vector<SamplerBinding> samplers;
    VertexInputLayout vertexInput;
    /// Literal constants to upload to the vertex and pixel constant blocks
    /// (the code folds direct uses; relative addressing reads the block).
    std::vector<LiteralConstant> vertexLiterals;
    std::vector<LiteralConstant> pixelLiterals;
    /// Colour targets the pixel shader writes (bit n = target n).
    std::uint32_t colorOutputs = 0;
    bool writesDepth = false;
    /// Vertex streams the vertex shader reads (SSBO bindings).
    std::vector<std::uint32_t> streams;
    /// Instructions not translated faithfully in either stage.
    std::vector<std::string> unsupported;

    /// Every binding of `name` (a name can be used by both stages).
    std::vector<const ConstantBinding*> findConstant(std::string_view name) const;
    const SamplerBinding* findSampler(std::string_view name) const;
};

/// Builds the program of pass `pass` of `effect`. The vertex shader's
/// fetches are bound through `override` when given (the layout of the
/// buffers the renderer will bind), else through the effect's own
/// declaration (Effect::inputDeclaration), and the stages are linked by interpolator semantic: every pixel
/// shader input register r gets varying location r, and the vertex shader
/// export with the same usage and index writes it. A pass without a pixel
/// shader (depth only) gets an empty fragment shader. Throws DataError when
/// the pass has no vertex shader or the microcode does not decode.
ShaderProgramSource buildProgram(
    const Effect& effect, const Pass& pass, const VertexDeclaration* override = nullptr);

/// The first pass of technique `technique`. Throws DataError if there is no
/// such technique.
ShaderProgramSource buildProgram(
    const Effect& effect, std::string_view technique, const VertexDeclaration* override = nullptr);

/// Contents of the XeSystemConstants block (binding 3, std140).
struct SystemConstants {
    /// gl_Position = (pos.xyz * clipScale.xyz + pos.w * clipOffset.xyz,
    /// pos.w). The game's clip space is Direct3D's (z from 0 to w); for the
    /// engine's reversed depth use z scale -1 and z offset 1. The x and y
    /// offsets are the hook for Direct3D 9's half-pixel convention
    /// (-1/width, +1/height moves geometry by half a pixel). Depth exports
    /// go through the same z mapping.
    std::array<float, 4> clipScale{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> clipOffset{0.0F, 0.0F, 0.0F, 0.0F};
    /// x: nonzero to count pixel y from the top (as Direct3D and the
    /// game's VPOS do); y: framebuffer height.
    std::array<float, 4> screen{0.0F, 0.0F, 0.0F, 0.0F};
    /// x: Direct3D comparison (0 or 8 always, 1 never, 2 less, 3 equal,
    /// 4 less-equal, 5 greater, 6 not-equal, 7 greater-equal) of colour 0's
    /// alpha against y.
    std::array<float, 4> alphaTest{0.0F, 0.0F, 0.0F, 0.0F};
    /// x bit 0: encode colour outputs with the Xenos piecewise-linear gamma
    /// curve, as the console's gamma render targets do.
    std::array<std::uint32_t, 4> flags{0, 0, 0, 0};
    /// 32-bit word offset of each vertex stream in its bound buffer.
    std::array<std::uint32_t, 16> streamBase{};

    static constexpr std::size_t kBytes = 5 * 16 + 4 * 16;
    std::array<std::uint8_t, kBytes> pack() const;
};

/// Contents of the XeControlConstants block (binding 2, std140).
struct ControlConstants {
    std::array<std::uint32_t, 8> bools{};
    std::array<std::uint32_t, 32> loops{};

    void setBool(std::uint32_t index, bool value);
    /// Loop constant: `count` iterations, aL from `start` by `step`.
    void setLoop(std::uint32_t index, std::uint32_t count, std::uint32_t start, std::int32_t step);

    static constexpr std::size_t kBytes = 2 * 16 + 8 * 16;
    std::array<std::uint8_t, kBytes> pack() const;
};

} // namespace openhorizon::xenos
