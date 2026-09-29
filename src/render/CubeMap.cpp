#include "CubeMap.h"

#include "world/XboxCube.h"

#include <utility>

namespace fh::render {

namespace {

// S3TC enums, as in Texture.cpp; DXT1 keeps its 1-bit alpha.
constexpr GLenum kCompressedRgbaDxt1 = 0x83F1;
constexpr GLenum kCompressedRgbaDxt5 = 0x83F3;
constexpr GLenum kCompressedSrgbAlphaDxt1 = 0x8C4D;
constexpr GLenum kCompressedSrgbAlphaDxt5 = 0x8C4F;

} // namespace

CubeMap::~CubeMap()
{
    if (m_id != 0) {
        glDeleteTextures(1, &m_id);
    }
}

CubeMap::CubeMap(CubeMap&& other) noexcept
    : m_id(std::exchange(other.m_id, 0))
    , m_bytes(other.m_bytes)
{
}

CubeMap& CubeMap::operator=(CubeMap&& other) noexcept
{
    if (this != &other) {
        if (m_id != 0) {
            glDeleteTextures(1, &m_id);
        }
        m_id = std::exchange(other.m_id, 0);
        m_bytes = other.m_bytes;
    }
    return *this;
}

CubeMap CubeMap::fromCube(const world::CubeTexture& cube, bool srgb)
{
    using Format = world::TextureSurface::Format;
    CubeMap map;
    const int levels = cube.levelCount();
    if (levels == 0 || cube.size <= 0) {
        return map;
    }
    GLenum internal = GL_RGBA8;
    bool compressed = true;
    switch (cube.format) {
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
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &map.m_id);
    glTextureStorage2D(map.m_id, levels, internal, cube.size, cube.size);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (int face = 0; face < world::CubeTexture::kFaces; ++face) {
        for (int level = 0; level < levels; ++level) {
            const world::TextureSurface& surface
                = cube.faces[static_cast<std::size_t>(face)][static_cast<std::size_t>(level)];
            if (compressed) {
                glCompressedTextureSubImage3D(map.m_id, level, 0, 0, face, surface.width, surface.height, 1, internal,
                    static_cast<GLsizei>(surface.data.size()), surface.data.data());
            } else {
                glTextureSubImage3D(map.m_id, level, 0, 0, face, surface.width, surface.height, 1, GL_RGBA,
                    GL_UNSIGNED_BYTE, surface.data.data());
            }
            map.m_bytes += surface.data.size();
        }
    }
    glTextureParameteri(map.m_id, GL_TEXTURE_MIN_FILTER, levels > 1 ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    glTextureParameteri(map.m_id, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(map.m_id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(map.m_id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(map.m_id, GL_TEXTURE_MAX_LEVEL, levels - 1);
    return map;
}

} // namespace fh::render
