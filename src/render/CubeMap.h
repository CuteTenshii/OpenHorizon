#pragma once

#include "Gl.h"

#include <cstddef>

namespace fh::world {
struct CubeTexture;
} // namespace fh::world

namespace fh::render {

/// A cube texture on the GPU, every level of every face uploaded as the
/// game stores it (DXT blocks stay compressed).
class CubeMap {
public:
    CubeMap() = default;
    ~CubeMap();
    CubeMap(CubeMap&& other) noexcept;
    CubeMap& operator=(CubeMap&& other) noexcept;
    CubeMap(const CubeMap&) = delete;
    CubeMap& operator=(const CubeMap&) = delete;

    /// Uploads `cube`. `srgb` marks colour data stored gamma-encoded, as for
    /// Texture::fromMipChain. Levels the cube lacks are not made up: a cube
    /// with only its top level is sampled from it alone.
    static CubeMap fromCube(const world::CubeTexture& cube, bool srgb);

    GLuint id() const { return m_id; }
    std::size_t byteSize() const { return m_bytes; }

private:
    GLuint m_id = 0;
    std::size_t m_bytes = 0;
};

} // namespace fh::render
