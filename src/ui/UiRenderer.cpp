#include "UiRenderer.h"

#include "base/Error.h"
#include "base/Log.h"
#include "render/GlProgram.h"
#include "vfs/Vfs.h"

#include <xds/xds.h>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <utility>

namespace fh::ui {

namespace {

const char* const kModelVertex = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUv;
uniform mat4 uTransform;
out vec2 vUv;
void main()
{
    vUv = aUv;
    gl_Position = uTransform * vec4(aPosition, 1.0);
}
)";

const char* const kModelFragment = R"(#version 450 core
in vec2 vUv;
uniform vec4 uColor;
uniform int uLayerCount;
uniform vec3 uUvRow0[2];
uniform vec3 uUvRow1[2];
layout(binding = 0) uniform sampler2D uLayer0;
layout(binding = 1) uniform sampler2D uLayer1;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec4 colour = uColor;
    vec3 uv = vec3(vUv, 1.0);
    if (uLayerCount > 0) {
        colour *= texture(uLayer0, vec2(dot(uUvRow0[0], uv), dot(uUvRow1[0], uv)));
    }
    if (uLayerCount > 1) {
        colour *= texture(uLayer1, vec2(dot(uUvRow0[1], uv), dot(uUvRow1[1], uv)));
    }
    fragColour = colour;
}
)";

// Glyph vertices are in ems from the pen; the pen position and the size
// place them in the text's space.
const char* const kTextVertex = R"(#version 450 core
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aCurve;
layout(location = 2) in float aOutside;
uniform mat4 uTransform;
uniform vec2 uPen;
uniform float uSize;
out vec2 vCurve;
flat out float vOutside;
void main()
{
    vCurve = aCurve;
    vOutside = aOutside;
    gl_Position = uTransform * vec4(uPen + aPosition * uSize, 0.0, 1.0);
}
)";

// Inside the outline u*u - v < 0 (reversed on outside triangles). Dividing
// the value by its screen-space gradient gives the distance to the edge in
// pixels, so the edge is anti-aliased over one pixel at any size.
const char* const kTextFragment = R"(#version 450 core
in vec2 vCurve;
flat in float vOutside;
uniform vec4 uColour;
layout(location = 0) out vec4 fragColour;
void main()
{
    float f = vCurve.x * vCurve.x - vCurve.y;
    vec2 gradient = vec2(dFdx(f), dFdy(f));
    float distance = f / max(length(gradient), 1e-6);
    float side = vOutside > 0.5 ? -1.0 : 1.0;
    float coverage = clamp(0.5 - side * distance, 0.0, 1.0);
    if (coverage <= 0.0) {
        discard;
    }
    fragColour = vec4(uColour.rgb, uColour.a * coverage);
}
)";

GLint glWrap(TextureWrap wrap)
{
    switch (wrap) {
    case TextureWrap::Repeat:
        return GL_REPEAT;
    case TextureWrap::Mirror:
        return GL_MIRRORED_REPEAT;
    case TextureWrap::Clamp:
        break;
    }
    // Anark's untiled images show nothing outside their square; the
    // transparent border keeps UV animations (the needle trails turn their
    // image) from smearing edge texels.
    return GL_CLAMP_TO_BORDER;
}

GLenum glBlend(BlendFactor factor, bool source)
{
    switch (factor) {
    case BlendFactor::Zero:
        return GL_ZERO;
    case BlendFactor::One:
        return GL_ONE;
    case BlendFactor::SourceAlpha:
        return GL_SRC_ALPHA;
    case BlendFactor::InverseSourceAlpha:
        return GL_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::Other:
        break;
    }
    return source ? GL_SRC_ALPHA : GL_ONE_MINUS_SRC_ALPHA;
}

} // namespace

UiTexture::~UiTexture()
{
    if (m_id) {
        glDeleteTextures(1, &m_id);
    }
}

UiTexture::UiTexture(UiTexture&& other) noexcept
    : m_id(std::exchange(other.m_id, 0))
    , m_width(other.m_width)
    , m_height(other.m_height)
{
}

UiTexture& UiTexture::operator=(UiTexture&& other) noexcept
{
    if (this != &other) {
        if (m_id) {
            glDeleteTextures(1, &m_id);
        }
        m_id = std::exchange(other.m_id, 0);
        m_width = other.m_width;
        m_height = other.m_height;
    }
    return *this;
}

UiTexture UiTexture::fromRgba(int width, int height, const std::uint8_t* rgba, bool mipmaps)
{
    UiTexture texture;
    texture.m_width = width;
    texture.m_height = height;
    const int levels
        = mipmaps ? 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(width, height))))) : 1;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture.m_id);
    glTextureStorage2D(texture.m_id, levels, GL_RGBA8, width, height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(texture.m_id, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    if (levels > 1) {
        glGenerateTextureMipmap(texture.m_id);
    }
    return texture;
}

UiTexture UiTexture::dxt1(int width, int height)
{
    // S3TC enums, which glcorearb.h leaves to the extension headers.
    constexpr GLenum kCompressedRgbaDxt1 = 0x83F1;
    UiTexture texture;
    texture.m_width = width;
    texture.m_height = height;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture.m_id);
    glTextureStorage2D(texture.m_id, 1, kCompressedRgbaDxt1, width, height);
    return texture;
}

void UiTexture::uploadDxt1(int x, int y, int width, int height, const std::uint8_t* blocks, std::size_t bytes)
{
    constexpr GLenum kCompressedRgbaDxt1 = 0x83F1;
    glCompressedTextureSubImage2D(
        m_id, 0, x, y, width, height, kCompressedRgbaDxt1, static_cast<GLsizei>(bytes), blocks);
}

UiProgram::UiProgram(std::string_view name, std::string_view vertex, std::string_view fragment)
    : m_id(render::linkProgram(name, vertex, fragment))
{
}

UiProgram::~UiProgram()
{
    if (m_id) {
        glDeleteProgram(m_id);
    }
}

UiProgram::UiProgram(UiProgram&& other) noexcept
    : m_id(std::exchange(other.m_id, 0))
{
}

UiProgram& UiProgram::operator=(UiProgram&& other) noexcept
{
    if (this != &other) {
        if (m_id) {
            glDeleteProgram(m_id);
        }
        m_id = std::exchange(other.m_id, 0);
    }
    return *this;
}

UiRenderer::UiRenderer(const Vfs& vfs, FontLibrary& fonts)
    : m_vfs(vfs)
    , m_fonts(fonts)
    , m_modelProgram("ui model", kModelVertex, kModelFragment)
    , m_textProgram("ui text", kTextVertex, kTextFragment)
{
    glCreateVertexArrays(1, &m_meshVao);
    glEnableVertexArrayAttrib(m_meshVao, 0);
    glVertexArrayAttribFormat(m_meshVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_meshVao, 0, 0);
    glEnableVertexArrayAttrib(m_meshVao, 1);
    glVertexArrayAttribFormat(m_meshVao, 1, 2, GL_FLOAT, GL_FALSE, 3 * sizeof(float));
    glVertexArrayAttribBinding(m_meshVao, 1, 0);

    glCreateVertexArrays(1, &m_textVao);
    glEnableVertexArrayAttrib(m_textVao, 0);
    glVertexArrayAttribFormat(m_textVao, 0, 2, GL_FLOAT, GL_FALSE, offsetof(GlyphGpuVertex, position));
    glVertexArrayAttribBinding(m_textVao, 0, 0);
    glEnableVertexArrayAttrib(m_textVao, 1);
    glVertexArrayAttribFormat(m_textVao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(GlyphGpuVertex, curve));
    glVertexArrayAttribBinding(m_textVao, 1, 0);
    glEnableVertexArrayAttrib(m_textVao, 2);
    glVertexArrayAttribFormat(m_textVao, 2, 1, GL_FLOAT, GL_FALSE, offsetof(GlyphGpuVertex, outside));
    glVertexArrayAttribBinding(m_textVao, 2, 0);
    m_textTransform = m_textProgram.location("uTransform");
    m_textPen = m_textProgram.location("uPen");
    m_textSize = m_textProgram.location("uSize");
    m_textColour = m_textProgram.location("uColour");

    const std::uint8_t white[4] = {255, 255, 255, 255};
    m_white = UiTexture::fromRgba(1, 1, white);
}

UiRenderer::~UiRenderer()
{
    glDeleteVertexArrays(1, &m_meshVao);
    glDeleteVertexArrays(1, &m_textVao);
    for (const GLuint buffer : {m_meshVbo, m_meshIbo}) {
        if (buffer) {
            glDeleteBuffers(1, &buffer);
        }
    }
    for (const auto& [font, gpu] : m_fontBuffers) {
        glDeleteBuffers(1, &gpu.buffer);
    }
    for (const auto& [key, sampler] : m_samplers) {
        glDeleteSamplers(1, &sampler);
    }
}

void UiRenderer::begin(int width, int height, float designWidth, float designHeight)
{
    m_target = {static_cast<float>(std::max(width, 1)), static_cast<float>(std::max(height, 1))};
    m_design = {designWidth, designHeight};
    m_scale = std::min(m_target.x / designWidth, m_target.y / designHeight);
    m_offset = (m_target - m_design * m_scale) * 0.5F;
    const glm::vec2 centre = m_offset + m_design * 0.5F * m_scale;
    m_projection = glm::mat4(1.0F);
    m_projection[0][0] = 2.0F * m_scale / m_target.x;
    m_projection[1][1] = 2.0F * m_scale / m_target.y;
    m_projection[2][2] = 0.0F;
    m_projection[3][0] = 2.0F * centre.x / m_target.x - 1.0F;
    m_projection[3][1] = 1.0F - 2.0F * centre.y / m_target.y;

    glViewport(0, 0, width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glDepthMask(GL_FALSE);
}

void UiRenderer::end()
{
    glBindVertexArray(0);
    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glEnable(GL_DEPTH_TEST);
}

glm::vec2 UiRenderer::toPixels(const glm::vec2& design) const
{
    return {
        m_offset.x + (design.x + m_design.x * 0.5F) * m_scale, m_offset.y + (m_design.y * 0.5F - design.y) * m_scale};
}

glm::vec2 UiRenderer::toDesign(const glm::vec2& pixels) const
{
    return {
        (pixels.x - m_offset.x) / m_scale - m_design.x * 0.5F, m_design.y * 0.5F - (pixels.y - m_offset.y) / m_scale};
}

void UiRenderer::setTextureOverride(const std::string& key, GLuint texture)
{
    if (texture) {
        m_overrides[key] = texture;
    } else {
        m_overrides.erase(key);
    }
}

const UiTexture* UiRenderer::texture(const std::string& path)
{
    const auto it = m_textures.find(path);
    if (it != m_textures.end()) {
        return it->second.get();
    }
    std::unique_ptr<UiTexture> loaded;
    try {
        if (const std::optional<Bytes> data = m_vfs.read(path)) {
            const xds::Texture xdsTexture = xds::Texture::parse(std::span<const std::uint8_t>(*data));
            const xds::Image image = xdsTexture.decodeLevel(0);
            loaded = std::make_unique<UiTexture>(
                UiTexture::fromRgba(static_cast<int>(image.width), static_cast<int>(image.height), image.rgba.data()));
        } else {
            logWarning("ui: texture {} is missing", path);
        }
    } catch (const std::exception& e) {
        logWarning("ui: texture {}: {}", path, e.what());
    }
    const UiTexture* result = loaded.get();
    m_textures.emplace(path, std::move(loaded));
    return result;
}

GLuint UiRenderer::sampler(TextureWrap u, TextureWrap v)
{
    const int key = static_cast<int>(u) * 4 + static_cast<int>(v);
    const auto it = m_samplers.find(key);
    if (it != m_samplers.end()) {
        return it->second;
    }
    GLuint id = 0;
    glCreateSamplers(1, &id);
    glSamplerParameteri(id, GL_TEXTURE_WRAP_S, glWrap(u));
    glSamplerParameteri(id, GL_TEXTURE_WRAP_T, glWrap(v));
    glSamplerParameteri(id, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    m_samplers.emplace(key, id);
    return id;
}

const UiRenderer::GpuMesh& UiRenderer::mesh(const Mesh& source)
{
    const auto it = m_meshes.find(&source);
    if (it != m_meshes.end()) {
        return it->second;
    }
    GpuMesh gpu;
    gpu.baseVertex = static_cast<GLint>(m_meshVertexData.size() / 5);
    gpu.firstIndex = static_cast<GLsizei>(m_meshIndexData.size());
    gpu.indexCount = static_cast<GLsizei>(source.indices.size());
    for (const MeshVertex& v : source.vertices) {
        m_meshVertexData.insert(m_meshVertexData.end(), {v.position.x, v.position.y, v.position.z, v.uv.x, v.uv.y});
    }
    m_meshIndexData.insert(m_meshIndexData.end(), source.indices.begin(), source.indices.end());

    // Buffers grow by recreating them from the CPU copies, which happens a
    // few times while a scene's meshes are first drawn.
    const auto upload = [](GLuint& buffer, std::size_t& capacity, const void* data, std::size_t bytes) {
        if (bytes > capacity) {
            if (buffer) {
                glDeleteBuffers(1, &buffer);
            }
            capacity = std::max<std::size_t>(bytes * 2, 64 * 1024);
            glCreateBuffers(1, &buffer);
            glNamedBufferData(buffer, static_cast<GLsizeiptr>(capacity), nullptr, GL_STATIC_DRAW);
        }
        glNamedBufferSubData(buffer, 0, static_cast<GLsizeiptr>(bytes), data);
    };
    upload(m_meshVbo, m_meshVertexCapacity, m_meshVertexData.data(), m_meshVertexData.size() * sizeof(float));
    upload(m_meshIbo, m_meshIndexCapacity, m_meshIndexData.data(), m_meshIndexData.size() * sizeof(std::uint16_t));
    glVertexArrayVertexBuffer(m_meshVao, 0, m_meshVbo, 0, static_cast<GLsizei>(5 * sizeof(float)));
    glVertexArrayElementBuffer(m_meshVao, m_meshIbo);
    return m_meshes.emplace(&source, gpu).first->second;
}

void UiRenderer::applyBlend(BlendFactor source, BlendFactor destination)
{
    glBlendFuncSeparate(glBlend(source, true), glBlend(destination, false), GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
}

void UiRenderer::draw(const std::vector<DrawItem>& items)
{
    for (const DrawItem& item : items) {
        if (item.isText) {
            drawText(item.text);
        } else {
            drawModel(item.model);
        }
    }
}

void UiRenderer::drawModel(const DrawModel& model)
{
    if (!model.mesh || model.mesh->indices.empty()) {
        return;
    }
    GLuint textures[2] = {m_white.id(), m_white.id()};
    glm::vec3 row0[2] = {glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(1.0F, 0.0F, 0.0F)};
    glm::vec3 row1[2] = {glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F)};
    GLuint samplers[2] = {0, 0};
    const int layerCount = static_cast<int>(std::min<std::size_t>(model.layers.size(), 2));
    for (int i = 0; i < layerCount; ++i) {
        const DrawModel::Layer& layer = model.layers[static_cast<std::size_t>(i)];
        GLuint id = 0;
        if (const auto byName = m_overrides.find(layer.imageName); byName != m_overrides.end()) {
            id = byName->second;
        } else if (const auto byPath = m_overrides.find(layer.texture); byPath != m_overrides.end()) {
            id = byPath->second;
        } else if (const UiTexture* loaded = texture(layer.texture)) {
            id = loaded->id();
        } else {
            // A missing layer would turn into a flat box; leave the model out.
            return;
        }
        textures[i] = id;
        row0[i] = layer.uvRow0;
        row1[i] = layer.uvRow1;
        samplers[i] = sampler(layer.wrapU, layer.wrapV);
    }
    const GpuMesh& gpu = mesh(*model.mesh);
    const GLuint program = m_modelProgram.id();
    glUseProgram(program);
    const glm::mat4 transform = m_projection * model.world;
    glProgramUniformMatrix4fv(program, m_modelProgram.location("uTransform"), 1, GL_FALSE, glm::value_ptr(transform));
    glProgramUniform4f(
        program, m_modelProgram.location("uColor"), model.color.r, model.color.g, model.color.b, model.color.a);
    glProgramUniform1i(program, m_modelProgram.location("uLayerCount"), layerCount);
    glProgramUniform3fv(program, m_modelProgram.location("uUvRow0"), 2, glm::value_ptr(row0[0]));
    glProgramUniform3fv(program, m_modelProgram.location("uUvRow1"), 2, glm::value_ptr(row1[0]));
    for (int i = 0; i < 2; ++i) {
        glBindTextureUnit(static_cast<GLuint>(i), textures[i]);
        glBindSampler(static_cast<GLuint>(i), samplers[i]);
    }
    applyBlend(model.sourceBlend, model.destinationBlend);
    glBindVertexArray(m_meshVao);
    glDrawElementsBaseVertex(GL_TRIANGLES, gpu.indexCount, GL_UNSIGNED_SHORT,
        reinterpret_cast<const void*>(static_cast<std::uintptr_t>(gpu.firstIndex) * sizeof(std::uint16_t)),
        gpu.baseVertex);
}

float UiRenderer::measure(std::string_view font, std::string_view text, float size, float tracking)
{
    const VectorFont* face = m_fonts.font(font);
    if (!face || size <= 0.0F) {
        return 0.0F;
    }
    return face->measure(text, tracking / size) * size;
}

const UiRenderer::FontGpu& UiRenderer::fontBuffer(const VectorFont& font)
{
    const auto it = m_fontBuffers.find(&font);
    if (it != m_fontBuffers.end()) {
        return it->second;
    }
    FontGpu gpu;
    std::vector<GlyphGpuVertex> vertices;
    for (const auto& [code, glyph] : font.glyphs()) {
        const auto first = static_cast<GLint>(vertices.size());
        for (const GlyphVertex& v : glyph.triangles) {
            vertices.push_back({v.position, v.curve, v.outside ? 1.0F : 0.0F});
        }
        gpu.ranges.emplace(code, std::make_pair(first, static_cast<GLsizei>(glyph.triangles.size())));
    }
    glCreateBuffers(1, &gpu.buffer);
    glNamedBufferStorage(gpu.buffer,
        static_cast<GLsizeiptr>(std::max<std::size_t>(vertices.size(), 1) * sizeof(GlyphGpuVertex)),
        vertices.empty() ? nullptr : vertices.data(), 0);
    return m_fontBuffers.emplace(&font, std::move(gpu)).first->second;
}

std::vector<TextLineLayout> layoutText(const VectorFont& font, const DrawText& text)
{
    std::vector<TextLineLayout> lines;
    if (text.size <= 0.0F) {
        return lines;
    }
    std::string_view rest = text.text;
    while (true) {
        const std::size_t newline = rest.find('\n');
        TextLineLayout line;
        line.text = rest.substr(0, newline);
        lines.push_back(line);
        if (newline == std::string_view::npos) {
            break;
        }
        rest = rest.substr(newline + 1);
    }
    const float size = text.size;
    const float lineStep = font.lineHeight() * size + text.leading;
    const float blockHeight = lineStep * static_cast<float>(lines.size());
    // The element's origin is the top of the block of lines, raised by a
    // fraction of the block's height for the other vertical alignments
    // (0x8243a2a8: 0.5, 1, 0.75 and 0.25 for alignments 1 to 4, none for
    // the rest), and each baseline lies the font's ascent below the top of
    // its line (0x8242f1d0 with the line records of 0x8242fa08).
    float top = 0.0F;
    switch (text.verticalAlign) {
    case 1:
        top = 0.5F * blockHeight;
        break;
    case 2:
        top = blockHeight;
        break;
    case 3:
        top = 0.75F * blockHeight;
        break;
    case 4:
        top = 0.25F * blockHeight;
        break;
    default:
        break;
    }
    float baseline = top - font.ascent() * size;
    for (TextLineLayout& line : lines) {
        line.width = font.measure(line.text, text.tracking / size) * size;
        if (text.horizontalAlign == 1) {
            line.x = -line.width * 0.5F;
        } else if (text.horizontalAlign == 2) {
            line.x = -line.width;
        }
        line.baseline = baseline;
        baseline -= lineStep;
    }
    return lines;
}

void UiRenderer::drawText(const DrawText& text)
{
    const VectorFont* font = m_fonts.font(text.font);
    if (!font || text.size <= 0.0F || text.text.empty()) {
        return;
    }
    const float size = text.size;
    const FontGpu& gpu = fontBuffer(*font);
    const GLuint program = m_textProgram.id();
    glUseProgram(program);
    const glm::mat4 transform = m_projection * text.world;
    glProgramUniformMatrix4fv(program, m_textTransform, 1, GL_FALSE, glm::value_ptr(transform));
    glProgramUniform1f(program, m_textSize, size);
    glProgramUniform4f(program, m_textColour, text.color.r, text.color.g, text.color.b, text.color.a);
    applyBlend(BlendFactor::SourceAlpha, BlendFactor::InverseSourceAlpha);
    glVertexArrayVertexBuffer(m_textVao, 0, gpu.buffer, 0, sizeof(GlyphGpuVertex));
    glBindVertexArray(m_textVao);

    const Glyph* fallback = font->fallback();
    for (const TextLineLayout& line : layoutText(*font, text)) {
        float pen = line.x;
        std::size_t i = 0;
        while (i < line.text.size()) {
            const char32_t c = decodeUtf8(line.text, i);
            if (c == U' ' || c == 0xA0) {
                pen += font->spaceAdvance() * size + text.tracking;
                continue;
            }
            const Glyph* glyph = font->glyph(c);
            if (!glyph) {
                glyph = fallback;
            }
            if (!glyph) {
                continue;
            }
            const auto range = gpu.ranges.find(glyph->codePoint);
            if (range != gpu.ranges.end() && range->second.second > 0) {
                glProgramUniform2f(program, m_textPen, pen, line.baseline);
                glDrawArrays(GL_TRIANGLES, range->second.first, range->second.second);
            }
            pen += glyph->advance * size + text.tracking;
        }
    }
}

} // namespace fh::ui
