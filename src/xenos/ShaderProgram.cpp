#include "ShaderProgram.h"

#include "base/Error.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>

namespace openhorizon::xenos {

namespace {

constexpr std::uint32_t kPixelBoolBase = 128;
constexpr std::uint32_t kPixelLoopBase = 16;
constexpr std::size_t kInterpolatorRegisters = 16;

constexpr std::string_view kEmptyFragmentShader = "#version 450 core\n// Depth-only pass: no pixel shader.\n"
                                                  "void main()\n{\n}\n";

void appendConstants(const Shader& shader, std::vector<ConstantBinding>& out)
{
    for (const ConstantInfo& c : shader.constants.constants) {
        ConstantBinding binding;
        binding.name = c.name;
        binding.stage = shader.stage;
        binding.set = c.set;
        binding.index = c.registerIndex;
        binding.count = c.registerCount;
        binding.parameterClass = c.parameterClass;
        binding.rows = c.rows;
        binding.columns = c.columns;
        binding.defaultValue = c.defaultValue;
        if (shader.stage == ShaderStage::Pixel) {
            if (c.set == RegisterSet::Bool) {
                binding.index += kPixelBoolBase;
            } else if (c.set == RegisterSet::Int4) {
                binding.index += kPixelLoopBase;
            }
        }
        if (c.set == RegisterSet::Sampler) {
            binding.index = samplerFetchConstant(shader.stage, c.registerIndex);
        }
        out.push_back(std::move(binding));
    }
}

void appendSamplers(const Shader& shader, const TranslationResult& result, std::vector<SamplerBinding>& out)
{
    for (const SamplerUse& use : result.samplers) {
        SamplerBinding binding;
        binding.stage = shader.stage;
        binding.unit = use.slot;
        binding.dimension = use.dimension;
        binding.glslName = use.glslName;
        for (const ConstantInfo& c : shader.constants.constants) {
            if (c.set == RegisterSet::Sampler && use.slot >= samplerFetchConstant(shader.stage, c.registerIndex)
                && use.slot < samplerFetchConstant(shader.stage, c.registerIndex) + c.registerCount) {
                binding.name = c.name;
            }
        }
        out.push_back(std::move(binding));
    }
}

void appendUnsupported(const TranslationResult& result, ShaderStage stage, std::vector<std::string>& out)
{
    for (const std::string& what : result.unsupported) {
        out.push_back(std::format("{}: {}", stageName(stage), what));
    }
}

void storeWord(std::uint8_t* at, std::uint32_t value)
{
    std::memcpy(at, &value, sizeof value);
}

} // namespace

std::vector<const ConstantBinding*> ShaderProgramSource::findConstant(std::string_view name) const
{
    std::vector<const ConstantBinding*> found;
    for (const ConstantBinding& c : constants) {
        if (c.name == name) {
            found.push_back(&c);
        }
    }
    return found;
}

const SamplerBinding* ShaderProgramSource::findSampler(std::string_view name) const
{
    for (const SamplerBinding& s : samplers) {
        if (s.name == name) {
            return &s;
        }
    }
    return nullptr;
}

ShaderProgramSource buildProgram(const Effect& effect, const Pass& pass, const VertexDeclaration* override)
{
    if (pass.vertexShader < 0 || static_cast<std::size_t>(pass.vertexShader) >= effect.shaders.size()) {
        throw DataError("pass {} has no vertex shader", pass.name);
    }
    const Shader& vertex = effect.shaders[static_cast<std::size_t>(pass.vertexShader)];
    const Shader* pixel = pass.pixelShader >= 0 && static_cast<std::size_t>(pass.pixelShader) < effect.shaders.size()
        ? &effect.shaders[static_cast<std::size_t>(pass.pixelShader)]
        : nullptr;

    ShaderProgramSource source;
    const std::optional<VertexDeclaration> effectDeclaration = effect.inputDeclaration(vertex);
    const VertexDeclaration* declaration
        = override != nullptr ? override : (effectDeclaration ? &*effectDeclaration : nullptr);
    TranslationOptions vertexOptions = defaultOptions(vertex, declaration);
    vertexOptions.exportLocations.fill(-1);

    if (pixel != nullptr) {
        TranslationOptions pixelOptions = defaultOptions(*pixel, nullptr);
        std::array<bool, kInterpolatorRegisters> fed{};
        for (const Interpolator& in : pixel->interpolators) {
            for (const Interpolator& out : vertex.interpolators) {
                if (out.usage == in.usage && out.usageIndex == in.usageIndex && out.reg < kInterpolatorRegisters
                    && in.reg < kInterpolatorRegisters) {
                    vertexOptions.exportLocations[out.reg] = static_cast<int>(in.reg);
                    fed[in.reg] = true;
                }
            }
        }
        // Pixel inputs the vertex shader does not produce still need a
        // writer for the stages to link; give them an export register the
        // vertex shader never writes, which stays zero.
        for (const Interpolator& in : pixel->interpolators) {
            if (in.reg >= kInterpolatorRegisters || fed[in.reg]) {
                continue;
            }
            for (std::size_t k = 0; k < kInterpolatorRegisters; ++k) {
                const bool exported = std::any_of(vertex.interpolators.begin(), vertex.interpolators.end(),
                    [&](const Interpolator& out) { return out.reg == k; });
                if (!exported && vertexOptions.exportLocations[k] < 0) {
                    vertexOptions.exportLocations[k] = static_cast<int>(in.reg);
                    fed[in.reg] = true;
                    break;
                }
            }
        }
        const TranslationResult pixelResult = translate(decodeProgram(ShaderStage::Pixel, pixel->microcode), pixelOptions);
        source.fragmentGlsl = pixelResult.glsl;
        source.colorOutputs = pixelResult.colorOutputs;
        source.writesDepth = pixelResult.writesDepth;
        appendConstants(*pixel, source.constants);
        appendSamplers(*pixel, pixelResult, source.samplers);
        appendUnsupported(pixelResult, ShaderStage::Pixel, source.unsupported);
        source.pixelLiterals = pixel->literals;
    } else {
        source.fragmentGlsl = std::string(kEmptyFragmentShader);
    }

    const TranslationResult vertexResult = translate(decodeProgram(ShaderStage::Vertex, vertex.microcode), vertexOptions);
    source.vertexGlsl = vertexResult.glsl;
    source.streams = vertexResult.streams;
    appendConstants(vertex, source.constants);
    appendSamplers(vertex, vertexResult, source.samplers);
    appendUnsupported(vertexResult, ShaderStage::Vertex, source.unsupported);
    source.vertexLiterals = vertex.literals;

    if (declaration != nullptr) {
        for (const VertexDeclElement& e : declaration->elements) {
            VertexInputElement element;
            element.name = e.name;
            element.usage = e.usage;
            element.usageIndex = e.usageIndex;
            element.stream = e.stream;
            element.byteOffset = e.offset;
            element.type = e.type;
            element.format = e.format();
            source.vertexInput.elements.push_back(std::move(element));
            if (source.vertexInput.strides.size() <= e.stream) {
                source.vertexInput.strides.resize(std::size_t{e.stream} + 1, 0);
            }
            source.vertexInput.strides[e.stream] = declaration->stride(e.stream);
        }
    }
    return source;
}

ShaderProgramSource buildProgram(const Effect& effect, std::string_view technique, const VertexDeclaration* override)
{
    const Technique* found = effect.findTechnique(technique);
    if (found == nullptr || found->passes.empty()) {
        throw DataError("no technique {} with a pass", technique);
    }
    return buildProgram(effect, found->passes.front(), override);
}

std::array<std::uint8_t, SystemConstants::kBytes> SystemConstants::pack() const
{
    std::array<std::uint8_t, kBytes> bytes{};
    std::uint8_t* at = bytes.data();
    for (const auto* vector : {&clipScale, &clipOffset, &screen, &alphaTest}) {
        for (const float f : *vector) {
            storeWord(at, std::bit_cast<std::uint32_t>(f));
            at += 4;
        }
    }
    for (const std::uint32_t word : flags) {
        storeWord(at, word);
        at += 4;
    }
    for (const std::uint32_t word : streamBase) {
        storeWord(at, word);
        at += 4;
    }
    return bytes;
}

void ControlConstants::setBool(std::uint32_t index, bool value)
{
    const std::uint32_t word = (index / 32) % bools.size();
    const std::uint32_t mask = 1U << (index % 32);
    bools[word] = value ? (bools[word] | mask) : (bools[word] & ~mask);
}

void ControlConstants::setLoop(std::uint32_t index, std::uint32_t count, std::uint32_t start, std::int32_t step)
{
    loops[index % loops.size()]
        = (count & 0xFF) | ((start & 0xFF) << 8) | ((static_cast<std::uint32_t>(step) & 0xFF) << 16);
}

std::array<std::uint8_t, ControlConstants::kBytes> ControlConstants::pack() const
{
    std::array<std::uint8_t, kBytes> bytes{};
    std::uint8_t* at = bytes.data();
    for (const std::uint32_t word : bools) {
        storeWord(at, word);
        at += 4;
    }
    for (const std::uint32_t word : loops) {
        storeWord(at, word);
        at += 4;
    }
    return bytes;
}

} // namespace openhorizon::xenos
