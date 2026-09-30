#pragma once

#include "Gl.h"

#include <cstdint>
#include <span>

namespace openhorizon::world {
struct TextureMipChain;
} // namespace openhorizon::world

namespace openhorizon::render {

/// A 2D texture on the GPU. Compressed game formats (DXT1, DXT5, BC5) are
/// uploaded as they are; the driver samples them directly.
class Texture {
public:
    Texture() = default;
    ~Texture();
    Texture(Texture&& other) noexcept;
    Texture& operator=(Texture&& other) noexcept;
    Texture(const Texture&) = delete;
    Texture& operator=(const Texture&) = delete;

    /// Uploads every level of `chain`. `srgb` marks colour textures, whose
    /// texels are stored gamma-encoded, as opposed to normal maps and masks.
    static Texture fromMipChain(const world::TextureMipChain& chain, bool srgb);
    /// A texture from tightly packed RGBA8 texels, with generated mips.
    static Texture fromRgba(int width, int height, std::span<const std::uint8_t> rgba, bool srgb, bool mipmaps);
    /// A 1x1 texture of one colour.
    static Texture solid(std::uint8_t r, std::uint8_t g, std::uint8_t b, std::uint8_t a);

    GLuint id() const { return m_id; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    std::size_t byteSize() const { return m_bytes; }
    void bind(GLuint unit) const { glBindTextureUnit(unit, m_id); }

private:
    GLuint m_id = 0;
    int m_width = 0;
    int m_height = 0;
    std::size_t m_bytes = 0;
};

} // namespace openhorizon::render
