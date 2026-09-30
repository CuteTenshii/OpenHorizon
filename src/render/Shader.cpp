#include "Shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <utility>

namespace openhorizon::render {

Shader::Shader(std::string_view name, std::string_view vertexSource, std::string_view fragmentSource)
    : m_program(linkProgram(name, vertexSource, fragmentSource))
{
}

Shader::Shader(ProgramBuild& build)
    : m_program(build.finish())
{
}

Shader::~Shader()
{
    if (m_program) {
        glDeleteProgram(m_program);
    }
}

Shader::Shader(Shader&& other) noexcept
    : m_program(std::exchange(other.m_program, 0))
    , m_locations(std::move(other.m_locations))
{
}

Shader& Shader::operator=(Shader&& other) noexcept
{
    if (this != &other) {
        if (m_program) {
            glDeleteProgram(m_program);
        }
        m_program = std::exchange(other.m_program, 0);
        m_locations = std::move(other.m_locations);
    }
    return *this;
}

GLint Shader::location(const char* name)
{
    const auto it = m_locations.find(name);
    if (it != m_locations.end()) {
        return it->second;
    }
    const GLint loc = glGetUniformLocation(m_program, name);
    m_locations.emplace(name, loc);
    return loc;
}

void Shader::set(const char* name, int value)
{
    glProgramUniform1i(m_program, location(name), value);
}

void Shader::set(const char* name, float value)
{
    glProgramUniform1f(m_program, location(name), value);
}

void Shader::set(const char* name, const glm::vec2& value)
{
    glProgramUniform2f(m_program, location(name), value.x, value.y);
}

void Shader::set(const char* name, const glm::vec3& value)
{
    glProgramUniform3fv(m_program, location(name), 1, glm::value_ptr(value));
}

void Shader::set(const char* name, const glm::vec4& value)
{
    glProgramUniform4fv(m_program, location(name), 1, glm::value_ptr(value));
}

void Shader::set(const char* name, const glm::mat4& value)
{
    glProgramUniformMatrix4fv(m_program, location(name), 1, GL_FALSE, glm::value_ptr(value));
}

void Shader::set(const char* name, const glm::mat3& value)
{
    glProgramUniformMatrix3fv(m_program, location(name), 1, GL_FALSE, glm::value_ptr(value));
}

} // namespace openhorizon::render
