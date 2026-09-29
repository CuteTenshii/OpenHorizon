#pragma once

#include "Gl.h"
#include "Shader.h"

#include "xenos/ShaderProgram.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh::render {

/// The four constant blocks a translated game shader reads (see
/// xenos/GlslTranslator.h): the vertex and pixel float constants, the
/// boolean and loop constants, and the translator's system block.
struct XenosConstants {
    std::array<glm::vec4, 256> vertex{};
    std::array<glm::vec4, 256> pixel{};
    xenos::ControlConstants control;
    xenos::SystemConstants system;
};

/// One pass of a game effect, translated to GLSL and linked, with its
/// constant table for filling XenosConstants by name.
class XenosProgram {
public:
    /// Links a translated pass; throws std::runtime_error naming `name` when
    /// the driver rejects it. With `squareOutput` the red, green and blue of
    /// colour target 0 are squared on the way out, for pixel shaders that
    /// write the square root of their colour to a target the game squares
    /// again when it reads it.
    XenosProgram(const xenos::ShaderProgramSource& source, std::string name, bool squareOutput = false);

    const std::string& name() const { return m_name; }
    const xenos::ShaderProgramSource& source() const { return m_source; }
    GLuint id() const { return m_shader.id(); }

    /// Constants with this name in `stage`, or nothing when the stage's
    /// table does not have it.
    const xenos::ConstantBinding* constant(std::string_view name, xenos::ShaderStage stage) const;
    /// Texture unit of the sampler of this name (either stage).
    std::optional<std::uint32_t> samplerUnit(std::string_view name) const;

    /// Writes `value` into every stage whose table declares float constant
    /// `name` (first register). Returns whether any did.
    bool set(XenosConstants& constants, std::string_view name, const glm::vec4& value) const;
    /// Writes a matrix into `rows` consecutive registers of every stage
    /// declaring `name`: register i holds column i of `value` in glm terms,
    /// which is row i of the HLSL row-vector matrix the shaders multiply.
    bool setMatrix(XenosConstants& constants, std::string_view name, const glm::mat4& value) const;
    bool setBool(XenosConstants& constants, std::string_view name, bool value) const;

    /// Loads the literal constants and the constant table defaults of both
    /// stages into `constants`.
    void loadDefaults(XenosConstants& constants) const;

private:
    std::string m_name;
    xenos::ShaderProgramSource m_source;
    Shader m_shader;
};

/// A stream buffer holding the constant blocks of a batch of draws: blocks
/// are appended on the CPU, uploaded together, then bound draw by draw.
class XenosConstantStream {
public:
    XenosConstantStream();
    ~XenosConstantStream();
    XenosConstantStream(const XenosConstantStream&) = delete;
    XenosConstantStream& operator=(const XenosConstantStream&) = delete;

    void clear();
    /// Appends `constants`; returns the index to bind() after upload().
    std::size_t append(const XenosConstants& constants);
    void upload();
    /// Binds the blocks of draw `index` to the translator's binding points.
    void bind(std::size_t index) const;

private:
    struct Offsets {
        std::size_t vertex = 0;
        std::size_t pixel = 0;
        std::size_t control = 0;
        std::size_t system = 0;
    };

    std::size_t place(std::size_t bytes);

    GLuint m_buffer = 0;
    std::size_t m_capacity = 0;
    std::size_t m_alignment = 256;
    std::vector<std::uint8_t> m_staging;
    std::vector<Offsets> m_draws;
};

} // namespace fh::render
