#include "Texture.h"

#include "world/TrackTextures.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace openhorizon::render {

namespace {

// S3TC enums, which glcorearb.h leaves to the extension headers. DXT1 is
// uploaded as its RGBA variant because foliage and fences use its 1-bit
// alpha for their cut-outs; the RGB variant would drop it.
constexpr GLenum kCompressedRgbaDxt1 = 0x83F1;
constexpr GLenum kCompressedRgbaDxt5 = 0x83F3;
constexpr GLenum kCompressedSrgbAlphaDxt1 = 0x8C4D;
constexpr GLenum kCompressedSrgbAlphaDxt5 = 0x8C4F;
constexpr GLenum kMaxAnisotropy = 0x84FE;

void applySampling(GLuint id, int levels)
{
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTextureParameteri(id, GL_TEXTURE_MAX_LEVEL, levels - 1);
    // Ground textures are seen at grazing angles all the time.
    glTextureParameterf(id, kMaxAnisotropy, 8.0F);
}

} // namespace

Texture::~Texture()
{
    if (m_id) {
        glDeleteTextures(1, &m_id);
    }
}

Texture::Texture(Texture&& other) noexcept
    : m_id(std::exchange(other.m_id, 0))
    , m_width(other.m_width)
    , m_height(other.m_height)
    , m_bytes(other.m_bytes)
{
}

Texture& Texture::operator=(Texture&& other) noexcept
{
    if (this != &other) {
        if (m_id) {
            glDeleteTextures(1, &m_id);
        }
        m_id = std::exchange(other.m_id, 0);
        m_width = other.m_width;
        m_height = other.m_height;
        m_bytes = other.m_bytes;
    }
    return *this;
}

Texture Texture::fromMipChain(const world::TextureMipChain& chain, bool srgb)
{
    using Format = world::TextureSurface::Format;
    Texture texture;
    texture.m_width = chain.width;
    texture.m_height = chain.height;
    const int levels = static_cast<int>(chain.levels.size());
    if (levels == 0 || chain.width <= 0 || chain.height <= 0) {
        return solid(255, 0, 255, 255);
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &texture.m_id);
    GLenum internal = GL_RGBA8;
    bool compressed = true;
    switch (chain.format) {
    case Format::Dxt1:
        internal = srgb ? kCompressedSrgbAlphaDxt1 : kCompressedRgbaDxt1;
        break;
    case Format::Dxt3:
    case Format::Dxt5:
        internal = srgb ? kCompressedSrgbAlphaDxt5 : kCompressedRgbaDxt5;
        break;
    case Format::Bc5:
        internal = GL_COMPRESSED_RG_RGTC2;
        break;
    case Format::Rgba8:
        internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
        compressed = false;
        break;
    }
    glTextureStorage2D(texture.m_id, levels, internal, chain.width, chain.height);
    for (int level = 0; level < levels; ++level) {
        const Bytes& data = chain.levels[static_cast<std::size_t>(level)];
        const int w = chain.levelWidth(level);
        const int h = chain.levelHeight(level);
        if (compressed) {
            glCompressedTextureSubImage2D(texture.m_id, level, 0, 0, w, h, internal, static_cast<GLsizei>(data.size()),
                data.data());
        } else {
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTextureSubImage2D(texture.m_id, level, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, data.data());
        }
        texture.m_bytes += data.size();
    }
    applySampling(texture.m_id, levels);
    return texture;
}

Texture Texture::fromRgba(int width, int height, std::span<const std::uint8_t> rgba, bool srgb, bool mipmaps)
{
    Texture texture;
    texture.m_width = width;
    texture.m_height = height;
    const int levels = mipmaps
        ? 1 + static_cast<int>(std::floor(std::log2(static_cast<float>(std::max(width, height)))))
        : 1;
    glCreateTextures(GL_TEXTURE_2D, 1, &texture.m_id);
    glTextureStorage2D(texture.m_id, levels, srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8, width, height);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(texture.m_id, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    if (levels > 1) {
        glGenerateTextureMipmap(texture.m_id);
    }
    applySampling(texture.m_id, levels);
    texture.m_bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
    return texture;
}

Texture Texture::solid(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a)
{
    const std::uint8_t texel[4] = {r, g, b, a};
    return fromRgba(1, 1, texel, false, false);
}

} // namespace openhorizon::render
