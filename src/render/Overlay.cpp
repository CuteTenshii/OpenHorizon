#include "Overlay.h"

#include "base/Bytes.h"
#include "base/Host.h"

#ifndef _WIN32
#include <fontconfig/fontconfig.h>
#endif
#include <ft2build.h>
#include FT_FREETYPE_H

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <stdexcept>
#include <system_error>
#include <vector>

namespace openhorizon::render {

namespace {

const char* const kLineVertex = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec4 aColour;
uniform mat4 uViewProjection;
out vec4 vColour;
void main()
{
    vColour = aColour;
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)";

const char* const kLineFragment = R"(#version 450 core
in vec4 vColour;
layout(location = 0) out vec4 fragColour;
void main()
{
    fragColour = vColour;
}
)";

const char* const kQuadVertex = R"(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexcoord;
layout(location = 2) in vec4 aColour;
uniform vec2 uScreen;
out vec2 vTexcoord;
out vec4 vColour;
void main()
{
    vTexcoord = aTexcoord;
    vColour = aColour;
    vec2 ndc = aPosition / uScreen * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

const char* const kQuadFragment = R"(#version 450 core
in vec2 vTexcoord;
in vec4 vColour;
layout(binding = 0) uniform sampler2D uAtlas;
layout(location = 0) out vec4 fragColour;
void main()
{
    fragColour = vColour * vec4(1.0, 1.0, 1.0, texture(uAtlas, vTexcoord).a);
}
)";

#ifdef _WIN32

/// Windows has no fontconfig: the pattern picks a font file every
/// installation ships, with Arial as the last resort.
std::string findFont(const std::string& pattern)
{
    const std::size_t colon = pattern.find(':');
    const std::string family = toLower(pattern.substr(0, colon));
    const bool bold = colon != std::string::npos && toLower(pattern.substr(colon)).find("bold") != std::string::npos;
    std::vector<std::string> candidates;
    if (family.empty() || family == "sans-serif" || family == "sans") {
        candidates = {bold ? "segoeuib.ttf" : "segoeui.ttf"};
    } else if (family == "serif") {
        candidates = {bold ? "timesbd.ttf" : "times.ttf"};
    } else if (family == "monospace" || family == "mono") {
        candidates = {bold ? "consolab.ttf" : "consola.ttf"};
    } else {
        std::string file;
        for (const char c : family) {
            if (c != ' ') {
                file.push_back(c);
            }
        }
        candidates = {file + (bold ? "bd.ttf" : ".ttf"), file + ".ttf"};
    }
    candidates.emplace_back(bold ? "arialbd.ttf" : "arial.ttf");
    const std::filesystem::path folder = systemFontFolder();
    for (const std::string& candidate : candidates) {
        const std::filesystem::path path = folder / candidate;
        std::error_code error;
        if (!folder.empty() && std::filesystem::is_regular_file(path, error)) {
            return pathToUtf8(path);
        }
    }
    throw std::runtime_error("no font matches \"" + pattern + "\" in " + pathToUtf8(folder));
}

#else

std::string findFont(const std::string& pattern)
{
    if (!FcInit()) {
        throw std::runtime_error("fontconfig could not initialise");
    }
    FcPattern* query = FcNameParse(reinterpret_cast<const FcChar8*>(pattern.c_str()));
    FcConfigSubstitute(nullptr, query, FcMatchPattern);
    FcDefaultSubstitute(query);
    FcResult result = FcResultNoMatch;
    FcPattern* match = FcFontMatch(nullptr, query, &result);
    FcPatternDestroy(query);
    std::string path;
    if (match) {
        FcChar8* file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            path = reinterpret_cast<const char*>(file);
        }
        FcPatternDestroy(match);
    }
    if (path.empty()) {
        throw std::runtime_error("no font matches \"" + pattern + "\"");
    }
    return path;
}

#endif

/// Decodes one code point and advances `i`; invalid bytes become U+FFFD.
char32_t nextCodePoint(std::string_view text, std::size_t& i)
{
    const auto byte = [&](std::size_t k) { return static_cast<unsigned char>(text[k]); };
    const unsigned char c = byte(i);
    if (c < 0x80) {
        ++i;
        return c;
    }
    int extra = 0;
    char32_t value = 0;
    if ((c & 0xE0) == 0xC0) {
        extra = 1;
        value = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        extra = 2;
        value = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        extra = 3;
        value = c & 0x07;
    } else {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<std::size_t>(extra) >= text.size()) {
        i = text.size();
        return 0xFFFD;
    }
    for (int k = 1; k <= extra; ++k) {
        value = (value << 6) | (byte(i + static_cast<std::size_t>(k)) & 0x3F);
    }
    i += static_cast<std::size_t>(extra) + 1;
    return value;
}

} // namespace

Overlay::Overlay(const std::string& fontPattern, int pixelSize)
    : m_lineShader("overlay lines", kLineVertex, kLineFragment)
    , m_quadShader("overlay quads", kQuadVertex, kQuadFragment)
{
    const std::string fontPath = findFont(fontPattern);
    FT_Library library = nullptr;
    if (FT_Init_FreeType(&library)) {
        throw std::runtime_error("FreeType could not initialise");
    }
    FT_Face face = nullptr;
    if (FT_New_Face(library, fontPath.c_str(), 0, &face)) {
        FT_Done_FreeType(library);
        throw std::runtime_error("cannot load font " + fontPath);
    }
    FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(pixelSize));
    m_lineHeight = static_cast<int>(face->size->metrics.height >> 6);
    m_ascender = static_cast<int>(face->size->metrics.ascender >> 6);

    // Printable ASCII, Latin-1 and a few symbols the HUD uses, packed in
    // rows into one atlas. A solid block at the start serves rectangles.
    std::vector<char32_t> codePoints;
    for (char32_t c = 32; c < 127; ++c) {
        codePoints.push_back(c);
    }
    for (char32_t c = 0xA0; c <= 0xFF; ++c) {
        codePoints.push_back(c);
    }
    for (char32_t c : {U'•', U'–', U'→', U'�'}) {
        codePoints.push_back(c);
    }
    constexpr int kAtlasSize = 1024;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(kAtlasSize) * kAtlasSize * 4, 0);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            std::uint8_t* texel = &rgba[(static_cast<std::size_t>(y) * kAtlasSize + static_cast<std::size_t>(x)) * 4];
            texel[0] = texel[1] = texel[2] = texel[3] = 255;
        }
    }
    int penX = 6;
    int penY = 0;
    int rowHeight = 4;
    for (char32_t c : codePoints) {
        if (FT_Load_Char(face, c, FT_LOAD_RENDER)) {
            continue;
        }
        const FT_Bitmap& bitmap = face->glyph->bitmap;
        const int w = static_cast<int>(bitmap.width);
        const int h = static_cast<int>(bitmap.rows);
        if (penX + w + 2 > kAtlasSize) {
            penX = 0;
            penY += rowHeight + 2;
            rowHeight = 0;
        }
        if (penY + h + 2 > kAtlasSize) {
            break;
        }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const std::uint8_t value = bitmap.buffer[y * bitmap.pitch + x];
                std::uint8_t* texel = &rgba[(static_cast<std::size_t>(penY + y) * kAtlasSize
                                                + static_cast<std::size_t>(penX + x))
                    * 4];
                texel[0] = texel[1] = texel[2] = 255;
                texel[3] = value;
            }
        }
        Glyph glyph;
        glyph.uvMin = glm::vec2(static_cast<float>(penX), static_cast<float>(penY)) / static_cast<float>(kAtlasSize);
        glyph.uvMax
            = glm::vec2(static_cast<float>(penX + w), static_cast<float>(penY + h)) / static_cast<float>(kAtlasSize);
        glyph.size = {static_cast<float>(w), static_cast<float>(h)};
        glyph.bearing = {static_cast<float>(face->glyph->bitmap_left), static_cast<float>(face->glyph->bitmap_top)};
        glyph.advance = static_cast<float>(face->glyph->advance.x >> 6);
        m_glyphs.emplace(c, glyph);
        penX += w + 2;
        rowHeight = std::max(rowHeight, h);
    }
    FT_Done_Face(face);
    FT_Done_FreeType(library);
    m_atlas = Texture::fromRgba(kAtlasSize, kAtlasSize, rgba, false, false);
    glTextureParameteri(m_atlas.id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_atlas.id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateVertexArrays(1, &m_lineVao);
    glEnableVertexArrayAttrib(m_lineVao, 0);
    glVertexArrayAttribFormat(m_lineVao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(LineVertex, position));
    glVertexArrayAttribBinding(m_lineVao, 0, 0);
    glEnableVertexArrayAttrib(m_lineVao, 1);
    glVertexArrayAttribFormat(m_lineVao, 1, 4, GL_FLOAT, GL_FALSE, offsetof(LineVertex, colour));
    glVertexArrayAttribBinding(m_lineVao, 1, 0);

    glCreateVertexArrays(1, &m_quadVao);
    glEnableVertexArrayAttrib(m_quadVao, 0);
    glVertexArrayAttribFormat(m_quadVao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(QuadVertex, position));
    glVertexArrayAttribBinding(m_quadVao, 0, 0);
    glEnableVertexArrayAttrib(m_quadVao, 1);
    glVertexArrayAttribFormat(m_quadVao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(QuadVertex, uv));
    glVertexArrayAttribBinding(m_quadVao, 1, 0);
    glEnableVertexArrayAttrib(m_quadVao, 2);
    glVertexArrayAttribFormat(m_quadVao, 2, 4, GL_FLOAT, GL_FALSE, offsetof(QuadVertex, colour));
    glVertexArrayAttribBinding(m_quadVao, 2, 0);
}

Overlay::~Overlay()
{
    glDeleteVertexArrays(1, &m_lineVao);
    glDeleteVertexArrays(1, &m_quadVao);
    if (m_lineVbo) {
        glDeleteBuffers(1, &m_lineVbo);
    }
    if (m_quadVbo) {
        glDeleteBuffers(1, &m_quadVbo);
    }
}

void Overlay::line(const glm::vec3& a, const glm::vec3& b, const glm::vec4& colour)
{
    m_lines.push_back({a, colour});
    m_lines.push_back({b, colour});
}

void Overlay::box(const glm::mat4& transform, const glm::vec3& h, const glm::vec4& colour)
{
    glm::vec3 corners[8];
    for (int i = 0; i < 8; ++i) {
        const glm::vec3 local((i & 1) ? h.x : -h.x, (i & 2) ? h.y : -h.y, (i & 4) ? h.z : -h.z);
        corners[i] = glm::vec3(transform * glm::vec4(local, 1.0F));
    }
    constexpr int edges[12][2] = {
        {0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (const auto& edge : edges) {
        line(corners[edge[0]], corners[edge[1]], colour);
    }
}

void Overlay::circle(const glm::vec3& centre, const glm::vec3& axisA, const glm::vec3& axisB, float radius,
    const glm::vec4& colour, int segments)
{
    constexpr float kTau = 6.28318531F;
    glm::vec3 previous = centre + axisA * radius;
    for (int i = 1; i <= segments; ++i) {
        const float angle = kTau * static_cast<float>(i) / static_cast<float>(segments);
        const glm::vec3 point = centre + (axisA * std::cos(angle) + axisB * std::sin(angle)) * radius;
        line(previous, point, colour);
        previous = point;
    }
}

void Overlay::quad(glm::vec2 p0, glm::vec2 p1, glm::vec2 uv0, glm::vec2 uv1, const glm::vec4& colour)
{
    const QuadVertex a{p0, uv0, colour};
    const QuadVertex b{{p1.x, p0.y}, {uv1.x, uv0.y}, colour};
    const QuadVertex c{p1, uv1, colour};
    const QuadVertex d{{p0.x, p1.y}, {uv0.x, uv1.y}, colour};
    m_quads.insert(m_quads.end(), {a, b, c, a, c, d});
}

void Overlay::rect(float x, float y, float w, float h, const glm::vec4& colour)
{
    // The atlas's solid corner block gives full coverage.
    const glm::vec2 uv(1.0F / 1024.0F);
    quad({x, y}, {x + w, y + h}, uv, uv * 2.0F, colour);
}

float Overlay::text(float x, float y, std::string_view utf8, const glm::vec4& colour, float scale)
{
    float penX = x;
    const float baseline = y + static_cast<float>(m_ascender) * scale;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const char32_t c = nextCodePoint(utf8, i);
        auto it = m_glyphs.find(c);
        if (it == m_glyphs.end()) {
            it = m_glyphs.find(U'�');
            if (it == m_glyphs.end()) {
                continue;
            }
        }
        const Glyph& g = it->second;
        if (g.size.x > 0.0F) {
            const glm::vec2 p0(std::round(penX + g.bearing.x * scale), std::round(baseline - g.bearing.y * scale));
            quad(p0, p0 + g.size * scale, g.uvMin, g.uvMax, colour);
        }
        penX += g.advance * scale;
    }
    return penX - x;
}

float Overlay::textWidth(std::string_view utf8, float scale) const
{
    float width = 0.0F;
    std::size_t i = 0;
    while (i < utf8.size()) {
        const auto it = m_glyphs.find(nextCodePoint(utf8, i));
        if (it != m_glyphs.end()) {
            width += it->second.advance * scale;
        }
    }
    return width;
}

void Overlay::flush(const glm::mat4& viewProjection, int width, int height)
{
    const auto uploadTo = [](GLuint& vbo, std::size_t& capacity, const void* data, std::size_t bytes) {
        if (bytes > capacity) {
            if (vbo) {
                glDeleteBuffers(1, &vbo);
            }
            capacity = std::max<std::size_t>(bytes * 2, 64 * 1024);
            glCreateBuffers(1, &vbo);
            glNamedBufferData(vbo, static_cast<GLsizeiptr>(capacity), nullptr, GL_STREAM_DRAW);
        }
        glNamedBufferSubData(vbo, 0, static_cast<GLsizeiptr>(bytes), data);
    };
    if (!m_lines.empty()) {
        uploadTo(m_lineVbo, m_lineCapacity, m_lines.data(), m_lines.size() * sizeof(LineVertex));
        glVertexArrayVertexBuffer(m_lineVao, 0, m_lineVbo, 0, sizeof(LineVertex));
        m_lineShader.use();
        m_lineShader.set("uViewProjection", viewProjection);
        glBindVertexArray(m_lineVao);
        glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(m_lines.size()));
        m_lines.clear();
    }
    if (!m_quads.empty()) {
        uploadTo(m_quadVbo, m_quadCapacity, m_quads.data(), m_quads.size() * sizeof(QuadVertex));
        glVertexArrayVertexBuffer(m_quadVao, 0, m_quadVbo, 0, sizeof(QuadVertex));
        m_quadShader.use();
        m_quadShader.set("uScreen", glm::vec2(static_cast<float>(width), static_cast<float>(height)));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        m_atlas.bind(0);
        glBindVertexArray(m_quadVao);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_quads.size()));
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        m_quads.clear();
    }
    glBindVertexArray(0);
}

} // namespace openhorizon::render
