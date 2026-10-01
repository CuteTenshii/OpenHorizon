#pragma once

#include "Gl.h"
#include "Shader.h"

#include "xenos/ShaderProgram.h"

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openhorizon::render {

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
    /// The pass's bindings; its GLSL is released once the program links.
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

    /// The float registers a named constant takes in every stage that
    /// declares it, looked up once so a draw can fill the constant without
    /// a search by name. Empty when the program does not declare it.
    struct FloatSlot {
        struct Register {
            xenos::ShaderStage stage = xenos::ShaderStage::Vertex;
            std::uint32_t index = 0;
            std::uint32_t count = 0;
        };
        std::vector<Register> registers;

        bool empty() const { return registers.empty(); }
    };
    FloatSlot floatSlot(std::string_view name) const;
    /// set() through a slot of this program.
    static void set(XenosConstants& constants, const FloatSlot& slot, const glm::vec4& value);
    /// setMatrix() through a slot of this program.
    static void setMatrix(XenosConstants& constants, const FloatSlot& slot, const glm::mat4& value);

    /// Loads the literal constants and the constant table defaults of both
    /// stages into `constants`.
    void loadDefaults(XenosConstants& constants) const;

private:
    struct NameHash {
        using is_transparent = void;
        std::size_t operator()(std::string_view name) const { return std::hash<std::string_view>{}(name); }
    };

    /// The constants of `name`, as indices into the source's table.
    const std::vector<std::uint32_t>* constantsNamed(std::string_view name) const;

    std::string m_name;
    xenos::ShaderProgramSource m_source;
    Shader m_shader;
    /// A car's draw sets some thirty constants by name for each of its
    /// passes, a hundred or so draws a car; a hash lookup keeps that from
    /// comparing every name of the table each time.
    std::unordered_map<std::string, std::vector<std::uint32_t>, NameHash, std::equal_to<>> m_byName;
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
    /// Appends a copy of `base` edited by `fill`, which is handed the copy
    /// where it sits in the stream's staging memory. Filling it there, rather
    /// than filling a XenosConstants and appending that, saves copying its
    /// 8 KB twice for every draw. Returns the index to bind() after
    /// upload().
    template <typename Fill> std::size_t append(const XenosConstants& base, Fill&& fill)
    {
        XenosConstants& constants = stage(base);
        std::forward<Fill>(fill)(constants);
        return commit(constants);
    }
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
    /// A copy of `base` in the staging memory, valid until the next place().
    XenosConstants& stage(const XenosConstants& base);
    /// Records the draw of the constants stage() returned last.
    std::size_t commit(const XenosConstants& constants);

    GLuint m_buffer = 0;
    std::size_t m_capacity = 0;
    std::size_t m_alignment = 256;
    /// Not a vector: growing one zeroes the bytes that are about to be
    /// overwritten, 8 KB a draw.
    std::unique_ptr<std::uint8_t[]> m_staging;
    std::size_t m_stagingSize = 0;
    std::size_t m_stagingCapacity = 0;
    std::vector<Offsets> m_draws;
};

} // namespace openhorizon::render
