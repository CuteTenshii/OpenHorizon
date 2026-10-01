#pragma once

#include "Presentation.h"
#include "VectorFont.h"

#include "render/Gl.h"

#include <glm/glm.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::ui {

/// In the text's own space (before DrawText::world): the pen starts at (x,
/// baseline) and advances `width`.
struct TextLineLayout {
    std::string_view text;
    float x = 0.0F;
    float baseline = 0.0F;
    float width = 0.0F;
};

/// Size, tracking and leading are taken after the text scale. The views
/// point into `text.text`.
std::vector<TextLineLayout> layoutText(const VectorFont& font, const DrawText& text);

/// An OpenGL texture the UI owns.
class UiTexture {
public:
    UiTexture() = default;
    ~UiTexture();
    UiTexture(UiTexture&& other) noexcept;
    UiTexture& operator=(UiTexture&& other) noexcept;
    UiTexture(const UiTexture&) = delete;
    UiTexture& operator=(const UiTexture&) = delete;

    /// RGBA8, rows from the top. Gamma-encoded values are sampled as they are:
    /// the console blended in display space.
    static UiTexture fromRgba(int width, int height, const std::uint8_t* rgba, bool mipmaps = true);
    /// An empty single-level DXT1 texture to fill with uploadDxt1().
    static UiTexture dxt1(int width, int height);
    /// Block-aligned DXT1 blocks in PC byte order.
    void uploadDxt1(int x, int y, int width, int height, const std::uint8_t* blocks, std::size_t bytes);

    GLuint id() const { return m_id; }
    int width() const { return m_width; }
    int height() const { return m_height; }

private:
    GLuint m_id = 0;
    int m_width = 0;
    int m_height = 0;
};

/// A linked GLSL program.
class UiProgram {
public:
    UiProgram() = default;
    /// Throws std::runtime_error with the driver's log on failure.
    UiProgram(std::string_view name, std::string_view vertex, std::string_view fragment);
    ~UiProgram();
    UiProgram(UiProgram&& other) noexcept;
    UiProgram& operator=(UiProgram&& other) noexcept;
    UiProgram(const UiProgram&) = delete;
    UiProgram& operator=(const UiProgram&) = delete;

    GLuint id() const { return m_id; }
    GLint location(const char* name) const { return glGetUniformLocation(m_id, name); }

private:
    GLuint m_id = 0;
};

/// Coordinates are Anark layer units (1280x720 design, origin at the centre,
/// y up), fitted keeping the aspect ratio. Blends into the bound
/// framebuffer with depth testing and culling off.
class UiRenderer {
public:
    UiRenderer(const Vfs& vfs, FontLibrary& fonts);
    ~UiRenderer();
    UiRenderer(const UiRenderer&) = delete;
    UiRenderer& operator=(const UiRenderer&) = delete;

    /// Starts a frame on a `width` x `height` pixel target for a design of
    /// `designWidth` x `designHeight` units.
    void begin(int width, int height, float designWidth = 1280.0F, float designHeight = 720.0F);
    void draw(const std::vector<DrawItem>& items);
    void drawModel(const DrawModel& model);
    void drawText(const DrawText& text);
    void end();

    /// Makes `key` (a texture path or image name in DrawModel layers) sample
    /// an external texture instead of loading a file.
    void setTextureOverride(const std::string& key, GLuint texture);
    /// Loads a texture from its game path (an .xds under media/ui/textures,
    /// or .jpg/.tga handled by the caller). Returns nullptr if missing.
    const UiTexture* texture(const std::string& path);

    /// Pixels per design unit of the current frame.
    float scale() const { return m_scale; }
    /// Design-space point to pixel coordinates from the top-left.
    glm::vec2 toPixels(const glm::vec2& design) const;
    glm::vec2 toDesign(const glm::vec2& pixels) const;

    /// Width of `text` in design units at `size`.
    float measure(std::string_view font, std::string_view text, float size, float tracking = 0.0F);

private:
    struct GpuMesh {
        GLint baseVertex = 0;
        GLsizei firstIndex = 0;
        GLsizei indexCount = 0;
    };
    struct GlyphGpuVertex {
        glm::vec2 position;
        glm::vec2 curve;
        float outside;
    };
    /// A font's glyph triangles in one buffer, with each glyph's range.
    struct FontGpu {
        GLuint buffer = 0;
        std::unordered_map<char32_t, std::pair<GLint, GLsizei>> ranges;
    };

    const GpuMesh& mesh(const Mesh& mesh);
    const FontGpu& fontBuffer(const VectorFont& font);
    GLuint sampler(TextureWrap u, TextureWrap v);
    void applyBlend(BlendFactor source, BlendFactor destination);

    const Vfs& m_vfs;
    FontLibrary& m_fonts;
    UiProgram m_modelProgram;
    UiProgram m_textProgram;
    GLuint m_meshVao = 0;
    GLuint m_meshVbo = 0;
    GLuint m_meshIbo = 0;
    std::size_t m_meshVertexCapacity = 0;
    std::size_t m_meshIndexCapacity = 0;
    std::vector<float> m_meshVertexData;
    std::vector<std::uint16_t> m_meshIndexData;
    std::unordered_map<const Mesh*, GpuMesh> m_meshes;
    GLuint m_textVao = 0;
    GLint m_textTransform = -1;
    GLint m_textPen = -1;
    GLint m_textSize = -1;
    GLint m_textColour = -1;
    std::unordered_map<const VectorFont*, FontGpu> m_fontBuffers;
    std::unordered_map<std::string, std::unique_ptr<UiTexture>> m_textures;
    std::unordered_map<std::string, GLuint> m_overrides;
    std::unordered_map<int, GLuint> m_samplers;
    UiTexture m_white;
    glm::mat4 m_projection{1.0F};
    float m_scale = 1.0F;
    glm::vec2 m_offset{0.0F};
    glm::vec2 m_target{1.0F};
    glm::vec2 m_design{1280.0F, 720.0F};
};

} // namespace openhorizon::ui
