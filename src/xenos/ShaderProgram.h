#pragma once

#include "Effect.h"
#include "GlslTranslator.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

/// GLSL for both stages of one effect pass and where every named constant, texture and vertex input
/// goes. Binding points are in GlslTranslator.h.
namespace openhorizon::xenos {

/// Where a named constant of the effect lives.
struct ConstantBinding {
    std::string name;
    ShaderStage stage = ShaderStage::Vertex;
    RegisterSet set = RegisterSet::Float4;
    /// Float4: vec4 index in the stage's block. Bool: bit in XeControlConstants.bools (pixel at 128 +
    /// #). Int4: loop constant (pixel at 16 + #; inferred, the game declares none). Sampler: texture
    /// unit (vertex at 16 + #).
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

/// Builds `pass` of `effect`, binding vertex fetches through `override` or else the effect's own
/// declaration, and linking stages by interpolator semantic. A pass without a pixel shader gets an
/// empty fragment shader. Throws DataError without a vertex shader or on undecodable microcode.
ShaderProgramSource buildProgram(
    const Effect& effect, const Pass& pass, const VertexDeclaration* override = nullptr);

/// The first pass of technique `technique`. Throws DataError if there is no
/// such technique.
ShaderProgramSource buildProgram(
    const Effect& effect, std::string_view technique, const VertexDeclaration* override = nullptr);

/// Contents of the XeSystemConstants block (binding 3, std140).
struct SystemConstants {
    /// gl_Position = (pos.xyz * clipScale.xyz + pos.w * clipOffset.xyz, pos.w), also applied to depth
    /// exports. Game clip z runs 0 to w; reversed depth uses z scale -1, offset 1. x/y offsets take
    /// Direct3D 9's half pixel (-1/width, +1/height).
    std::array<float, 4> clipScale{1.0F, 1.0F, 1.0F, 1.0F};
    std::array<float, 4> clipOffset{0.0F, 0.0F, 0.0F, 0.0F};
    /// x: nonzero to count pixel y from the top (as the game's VPOS does); y: framebuffer height.
    std::array<float, 4> screen{0.0F, 0.0F, 0.0F, 0.0F};
    /// x: Direct3D comparison (0 or 8 always, 1 never, 2 less, 3 equal,
    /// 4 less-equal, 5 greater, 6 not-equal, 7 greater-equal) of colour 0's
    /// alpha against y.
    std::array<float, 4> alphaTest{0.0F, 0.0F, 0.0F, 0.0F};
    /// x bit 0: encode colour outputs with the Xenos piecewise-linear gamma, as its gamma targets do.
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
