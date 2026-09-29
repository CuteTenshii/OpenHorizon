#include "XenosProgram.h"

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace fh::render {

namespace {

using xenos::ConstantBinding;
using xenos::RegisterSet;
using xenos::ShaderStage;

constexpr std::size_t kFloatBlockBytes = 256 * sizeof(glm::vec4);
/// Room for this many draws before the buffer grows.
constexpr std::size_t kInitialDraws = 64;

std::array<glm::vec4, 256>& stageBlock(XenosConstants& constants, ShaderStage stage)
{
    return stage == ShaderStage::Vertex ? constants.vertex : constants.pixel;
}

/// Inserts `xe_color0.rgb *= xe_color0.rgb` after the generated code's copy
/// of the colour export into its output (see GlslTranslator.h).
std::string squareColourOutput(std::string glsl)
{
    constexpr std::string_view kWrite = "xe_color0 = xe_oC[0];";
    const std::size_t at = glsl.find(kWrite);
    if (at == std::string::npos) {
        return glsl;
    }
    glsl.insert(at + kWrite.size(), "\n    xe_color0.rgb *= xe_color0.rgb;");
    return glsl;
}

} // namespace

XenosProgram::XenosProgram(const xenos::ShaderProgramSource& source, std::string name, bool squareOutput)
    : m_name(std::move(name))
    , m_source(source)
{
    if (squareOutput) {
        m_source.fragmentGlsl = squareColourOutput(m_source.fragmentGlsl);
    }
    m_shader = Shader(m_name, m_source.vertexGlsl, m_source.fragmentGlsl);
}

const ConstantBinding* XenosProgram::constant(std::string_view name, ShaderStage stage) const
{
    for (const ConstantBinding& c : m_source.constants) {
        if (c.stage == stage && c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

std::optional<std::uint32_t> XenosProgram::samplerUnit(std::string_view name) const
{
    if (const xenos::SamplerBinding* sampler = m_source.findSampler(name)) {
        return sampler->unit;
    }
    return std::nullopt;
}

bool XenosProgram::set(XenosConstants& constants, std::string_view name, const glm::vec4& value) const
{
    bool any = false;
    for (const ConstantBinding& c : m_source.constants) {
        if (c.name != name || c.set != RegisterSet::Float4 || c.index >= 256) {
            continue;
        }
        stageBlock(constants, c.stage)[c.index] = value;
        any = true;
    }
    return any;
}

bool XenosProgram::setMatrix(XenosConstants& constants, std::string_view name, const glm::mat4& value) const
{
    bool any = false;
    for (const ConstantBinding& c : m_source.constants) {
        if (c.name != name || c.set != RegisterSet::Float4) {
            continue;
        }
        std::array<glm::vec4, 256>& block = stageBlock(constants, c.stage);
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(c.count, 4) && c.index + i < 256; ++i) {
            block[c.index + i] = value[static_cast<glm::length_t>(i)];
        }
        any = true;
    }
    return any;
}

bool XenosProgram::setBool(XenosConstants& constants, std::string_view name, bool value) const
{
    bool any = false;
    for (const ConstantBinding& c : m_source.constants) {
        if (c.name == name && c.set == RegisterSet::Bool) {
            constants.control.setBool(c.index, value);
            any = true;
        }
    }
    return any;
}

void XenosProgram::loadDefaults(XenosConstants& constants) const
{
    for (const ConstantBinding& c : m_source.constants) {
        if (c.set != RegisterSet::Float4 || c.defaultValue.empty()) {
            continue;
        }
        std::array<glm::vec4, 256>& block = stageBlock(constants, c.stage);
        for (std::uint32_t i = 0; i < c.count && c.index + i < 256; ++i) {
            const std::size_t at = static_cast<std::size_t>(i) * 4;
            if (at + 3 < c.defaultValue.size()) {
                block[c.index + i] = glm::vec4(
                    c.defaultValue[at], c.defaultValue[at + 1], c.defaultValue[at + 2], c.defaultValue[at + 3]);
            }
        }
    }
    // Literals last: the code folds them where it addresses them directly,
    // and relative addressing expects them in the block.
    for (const xenos::LiteralConstant& literal : m_source.vertexLiterals) {
        if (literal.reg < 256) {
            constants.vertex[literal.reg]
                = glm::vec4(literal.value[0], literal.value[1], literal.value[2], literal.value[3]);
        }
    }
    for (const xenos::LiteralConstant& literal : m_source.pixelLiterals) {
        if (literal.reg < 256) {
            constants.pixel[literal.reg]
                = glm::vec4(literal.value[0], literal.value[1], literal.value[2], literal.value[3]);
        }
    }
}

XenosConstantStream::XenosConstantStream()
{
    GLint alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
    m_alignment = static_cast<std::size_t>(std::max(alignment, 16));
}

XenosConstantStream::~XenosConstantStream()
{
    if (m_buffer) {
        glDeleteBuffers(1, &m_buffer);
    }
}

void XenosConstantStream::clear()
{
    m_staging.clear();
    m_draws.clear();
}

std::size_t XenosConstantStream::place(std::size_t bytes)
{
    const std::size_t at = (m_staging.size() + m_alignment - 1) / m_alignment * m_alignment;
    m_staging.resize(at + bytes);
    return at;
}

std::size_t XenosConstantStream::append(const XenosConstants& constants)
{
    Offsets offsets;
    offsets.vertex = place(kFloatBlockBytes);
    std::memcpy(m_staging.data() + offsets.vertex, constants.vertex.data(), kFloatBlockBytes);
    offsets.pixel = place(kFloatBlockBytes);
    std::memcpy(m_staging.data() + offsets.pixel, constants.pixel.data(), kFloatBlockBytes);
    const auto control = constants.control.pack();
    offsets.control = place(control.size());
    std::memcpy(m_staging.data() + offsets.control, control.data(), control.size());
    const auto system = constants.system.pack();
    offsets.system = place(system.size());
    std::memcpy(m_staging.data() + offsets.system, system.data(), system.size());
    m_draws.push_back(offsets);
    return m_draws.size() - 1;
}

void XenosConstantStream::upload()
{
    if (m_staging.empty()) {
        return;
    }
    if (m_staging.size() > m_capacity) {
        if (m_buffer) {
            glDeleteBuffers(1, &m_buffer);
        }
        m_capacity = std::max(m_staging.size(), kInitialDraws * (2 * kFloatBlockBytes + 2 * m_alignment));
        glCreateBuffers(1, &m_buffer);
        glNamedBufferData(m_buffer, static_cast<GLsizeiptr>(m_capacity), nullptr, GL_STREAM_DRAW);
    } else {
        // Orphans the storage the previous frame's draws may still read.
        glNamedBufferData(m_buffer, static_cast<GLsizeiptr>(m_capacity), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(m_buffer, 0, static_cast<GLsizeiptr>(m_staging.size()), m_staging.data());
}

void XenosConstantStream::bind(std::size_t index) const
{
    const Offsets& o = m_draws.at(index);
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kVertexConstants, m_buffer, static_cast<GLintptr>(o.vertex),
        static_cast<GLsizeiptr>(kFloatBlockBytes));
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kPixelConstants, m_buffer, static_cast<GLintptr>(o.pixel),
        static_cast<GLsizeiptr>(kFloatBlockBytes));
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kControlConstants, m_buffer, static_cast<GLintptr>(o.control),
        static_cast<GLsizeiptr>(xenos::ControlConstants::kBytes));
    glBindBufferRange(GL_UNIFORM_BUFFER, xenos::binding::kSystemConstants, m_buffer, static_cast<GLintptr>(o.system),
        static_cast<GLsizeiptr>(xenos::SystemConstants::kBytes));
}

} // namespace fh::render
