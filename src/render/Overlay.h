#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon::render {

/// Immediate-mode drawing on top of the scene with a system font: 3D lines,
/// 2D text and rectangles for the debug view. What the player sees comes
/// from the game's own scenes instead. Calls queue geometry; flush() draws
/// and clears it.
class Overlay {
public:
    /// Loads a font through fontconfig by pattern (e.g. "sans-serif:bold")
    /// at `pixelSize`. Throws std::runtime_error if no font can be loaded.
    Overlay(const std::string& fontPattern, int pixelSize);
    ~Overlay();
    Overlay(const Overlay&) = delete;
    Overlay& operator=(const Overlay&) = delete;

    void line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& colour);
    /// The twelve edges of a box given by its transform and half extents.
    void box(const glm::mat4& transform, const glm::vec3& halfExtents, const glm::vec4& colour);
    void circle(const glm::vec3& centre, const glm::vec3& axisA, const glm::vec3& axisB, float radius,
        const glm::vec4& colour, int segments = 24);

    /// Screen-space rectangle in pixels from the top-left.
    void rect(float x, float y, float w, float h, const glm::vec4& colour);
    /// Text with its top-left at x, y in pixels; `scale` multiplies the
    /// loaded pixel size. Returns the advance width.
    float text(float x, float y, std::string_view utf8, const glm::vec4& colour, float scale = 1.0F);
    float textWidth(std::string_view utf8, float scale = 1.0F) const;
    float lineHeight(float scale = 1.0F) const { return static_cast<float>(m_lineHeight) * scale; }
    /// Vertices of the lines, rectangles and text queued since the last
    /// flush.
    std::size_t queuedVertices() const { return m_lines.size() + m_quads.size(); }

    /// Draws everything queued: lines with `viewProjection` and depth
    /// testing, then 2D on a `width` x `height` pixel screen.
    void flush(const glm::mat4& viewProjection, int width, int height);

private:
    struct Glyph {
        glm::vec2 uvMin{0.0F};
        glm::vec2 uvMax{0.0F};
        glm::vec2 size{0.0F};
        glm::vec2 bearing{0.0F};
        float advance = 0.0F;
    };
    struct LineVertex {
        glm::vec3 position;
        glm::vec4 colour;
    };
    struct QuadVertex {
        glm::vec2 position;
        glm::vec2 uv;
        glm::vec4 colour;
    };

    void quad(glm::vec2 p0, glm::vec2 p1, glm::vec2 uv0, glm::vec2 uv1, const glm::vec4& colour);

    std::unordered_map<char32_t, Glyph> m_glyphs;
    int m_lineHeight = 0;
    int m_ascender = 0;
    Texture m_atlas;
    Shader m_lineShader;
    Shader m_quadShader;
    GLuint m_lineVao = 0;
    GLuint m_lineVbo = 0;
    GLuint m_quadVao = 0;
    GLuint m_quadVbo = 0;
    std::size_t m_lineCapacity = 0;
    std::size_t m_quadCapacity = 0;
    std::vector<LineVertex> m_lines;
    std::vector<QuadVertex> m_quads;
};

} // namespace openhorizon::render
