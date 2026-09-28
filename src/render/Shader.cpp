#include "Shader.h"

#include <glm/gtc/type_ptr.hpp>

#include <stdexcept>
#include <utility>

namespace fh::render {

namespace {

GLuint compile(std::string_view name, GLenum stage, std::string_view source)
{
    const GLuint shader = glCreateShader(stage);
    const char* text = source.data();
    const auto length = static_cast<GLint>(source.size());
    glShaderSource(shader, 1, &text, &length);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        GLint logLength = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetShaderInfoLog(shader, logLength, nullptr, log.data());
        glDeleteShader(shader);
        throw std::runtime_error(std::string(name) + (stage == GL_VERTEX_SHADER ? " vertex" : " fragment")
            + " shader: " + log);
    }
    return shader;
}

} // namespace

Shader::Shader(std::string_view name, std::string_view vertexSource, std::string_view fragmentSource)
{
    const GLuint vertex = compile(name, GL_VERTEX_SHADER, vertexSource);
    GLuint fragment = 0;
    try {
        fragment = compile(name, GL_FRAGMENT_SHADER, fragmentSource);
    } catch (...) {
        glDeleteShader(vertex);
        throw;
    }
    m_program = glCreateProgram();
    glAttachShader(m_program, vertex);
    glAttachShader(m_program, fragment);
    glLinkProgram(m_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint ok = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &ok);
    if (!ok) {
        GLint logLength = 0;
        glGetProgramiv(m_program, GL_INFO_LOG_LENGTH, &logLength);
        std::string log(static_cast<std::size_t>(std::max(logLength, 1)), '\0');
        glGetProgramInfoLog(m_program, logLength, nullptr, log.data());
        glDeleteProgram(m_program);
        m_program = 0;
        throw std::runtime_error(std::string(name) + " program: " + log);
    }
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

} // namespace fh::render
