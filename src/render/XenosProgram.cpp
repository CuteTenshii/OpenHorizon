#include "XenosProgram.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <new>
#include <string_view>
#include <type_traits>
#include <utility>

namespace openhorizon::render {

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
    // The driver holds the linked program; the GLSL text is not needed again.
    std::string().swap(m_source.vertexGlsl);
    std::string().swap(m_source.fragmentGlsl);
    for (std::size_t i = 0; i < m_source.constants.size(); ++i) {
        m_byName[m_source.constants[i].name].push_back(static_cast<std::uint32_t>(i));
    }
}

const std::vector<std::uint32_t>* XenosProgram::constantsNamed(std::string_view name) const
{
    const auto it = m_byName.find(name);
    return it == m_byName.end() ? nullptr : &it->second;
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
    const std::vector<std::uint32_t>* named = constantsNamed(name);
    if (named == nullptr) {
        return false;
    }
    bool any = false;
    for (const std::uint32_t i : *named) {
        const ConstantBinding& c = m_source.constants[i];
        if (c.set != RegisterSet::Float4 || c.index >= 256) {
            continue;
        }
        stageBlock(constants, c.stage)[c.index] = value;
        any = true;
    }
    return any;
}

bool XenosProgram::setMatrix(XenosConstants& constants, std::string_view name, const glm::mat4& value) const
{
    const std::vector<std::uint32_t>* named = constantsNamed(name);
    if (named == nullptr) {
        return false;
    }
    bool any = false;
    for (const std::uint32_t at : *named) {
        const ConstantBinding& c = m_source.constants[at];
        if (c.set != RegisterSet::Float4) {
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

XenosProgram::FloatSlot XenosProgram::floatSlot(std::string_view name) const
{
    FloatSlot slot;
    if (const std::vector<std::uint32_t>* named = constantsNamed(name)) {
        for (const std::uint32_t at : *named) {
            const ConstantBinding& c = m_source.constants[at];
            if (c.set == RegisterSet::Float4) {
                slot.registers.push_back({c.stage, c.index, c.count});
            }
        }
    }
    return slot;
}

void XenosProgram::set(XenosConstants& constants, const FloatSlot& slot, const glm::vec4& value)
{
    for (const FloatSlot::Register& r : slot.registers) {
        if (r.index < 256) {
            stageBlock(constants, r.stage)[r.index] = value;
        }
    }
}

void XenosProgram::setMatrix(XenosConstants& constants, const FloatSlot& slot, const glm::mat4& value)
{
    for (const FloatSlot::Register& r : slot.registers) {
        std::array<glm::vec4, 256>& block = stageBlock(constants, r.stage);
        for (std::uint32_t i = 0; i < std::min<std::uint32_t>(r.count, 4) && r.index + i < 256; ++i) {
            block[r.index + i] = value[static_cast<glm::length_t>(i)];
        }
    }
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
    m_stagingSize = 0;
    m_draws.clear();
}

std::size_t XenosConstantStream::place(std::size_t bytes)
{
    const std::size_t at = (m_stagingSize + m_alignment - 1) / m_alignment * m_alignment;
    if (at + bytes > m_stagingCapacity) {
        const std::size_t capacity
            = std::max({at + bytes, m_stagingCapacity * 2, kInitialDraws * sizeof(XenosConstants)});
        auto grown = std::make_unique_for_overwrite<std::uint8_t[]>(capacity);
        if (m_stagingSize > 0) {
            std::memcpy(grown.get(), m_staging.get(), m_stagingSize);
        }
        m_staging = std::move(grown);
        m_stagingCapacity = capacity;
    }
    m_stagingSize = at + bytes;
    return at;
}

XenosConstants& XenosConstantStream::stage(const XenosConstants& base)
{
    const std::size_t at = place(sizeof(XenosConstants));
    // The staging bytes provide storage for the copy; the type is
    // trivially destructible, so nothing needs to end its lifetime.
    static_assert(std::is_trivially_destructible_v<XenosConstants>);
    return *::new (static_cast<void*>(m_staging.get() + at)) XenosConstants(base);
}

std::size_t XenosConstantStream::commit(const XenosConstants& constants)
{
    const auto offsetOf = [&](const void* at) {
        return static_cast<std::size_t>(static_cast<const std::uint8_t*>(at) - m_staging.get());
    };
    Offsets offsets;
    offsets.vertex = offsetOf(constants.vertex.data());
    offsets.pixel = offsetOf(constants.pixel.data());
    const auto control = constants.control.pack();
    const auto system = constants.system.pack();
    // The vertex block starts aligned; the pixel block follows it at 4 KB,
    // aligned too unless a driver asks for more than that. From here on
    // `constants` may move, as place() can grow the staging memory.
    if (offsets.pixel % m_alignment != 0) {
        const std::array<glm::vec4, 256> pixel = constants.pixel;
        offsets.pixel = place(kFloatBlockBytes);
        std::memcpy(m_staging.get() + offsets.pixel, pixel.data(), kFloatBlockBytes);
    }
    offsets.control = place(control.size());
    std::memcpy(m_staging.get() + offsets.control, control.data(), control.size());
    offsets.system = place(system.size());
    std::memcpy(m_staging.get() + offsets.system, system.data(), system.size());
    m_draws.push_back(offsets);
    return m_draws.size() - 1;
}

void XenosConstantStream::upload()
{
    if (m_stagingSize == 0) {
        return;
    }
    if (m_stagingSize > m_capacity) {
        if (m_buffer) {
            glDeleteBuffers(1, &m_buffer);
        }
        m_capacity = std::max(m_stagingSize, kInitialDraws * (sizeof(XenosConstants) + 2 * m_alignment));
        glCreateBuffers(1, &m_buffer);
        glNamedBufferData(m_buffer, static_cast<GLsizeiptr>(m_capacity), nullptr, GL_STREAM_DRAW);
    } else {
        // Orphans the storage the previous frame's draws may still read.
        glNamedBufferData(m_buffer, static_cast<GLsizeiptr>(m_capacity), nullptr, GL_STREAM_DRAW);
    }
    glNamedBufferSubData(m_buffer, 0, static_cast<GLsizeiptr>(m_stagingSize), m_staging.get());
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

} // namespace openhorizon::render
