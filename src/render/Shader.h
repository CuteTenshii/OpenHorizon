#pragma once

#include "Gl.h"
#include "GlProgram.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_map>

namespace fh::render {

/// A linked GLSL program. Uniform locations are looked up once and cached.
/// Programs go through the binary cache (GlProgram.h).
class Shader {
public:
    Shader() = default;
    /// Compiles and links, or restores the program from the cache; throws
    /// std::runtime_error with the driver's log, prefixed by `name`, when
    /// either fails.
    Shader(std::string_view name, std::string_view vertexSource, std::string_view fragmentSource);
    /// Takes the program of a build started earlier, waiting for it if
    /// the driver is still compiling; throws as above.
    explicit Shader(ProgramBuild& build);
    ~Shader();
    Shader(Shader&& other) noexcept;
    Shader& operator=(Shader&& other) noexcept;
    Shader(const Shader&) = delete;
    Shader& operator=(const Shader&) = delete;

    GLuint id() const { return m_program; }
    void use() const { glUseProgram(m_program); }

    void set(const char* name, int value);
    void set(const char* name, float value);
    void set(const char* name, const glm::vec2& value);
    void set(const char* name, const glm::vec3& value);
    void set(const char* name, const glm::vec4& value);
    void set(const char* name, const glm::mat4& value);
    void set(const char* name, const glm::mat3& value);

private:
    GLint location(const char* name);

    GLuint m_program = 0;
    std::unordered_map<std::string, GLint> m_locations;
};

} // namespace fh::render
