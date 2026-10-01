#pragma once

#include "Gl.h"

#include <cstddef>

namespace openhorizon::world {
struct CubeTexture;
} // namespace openhorizon::world

namespace openhorizon::render {

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

    /// `srgb` as for Texture::fromMipChain. Missing levels are not generated.
    static CubeMap fromCube(const world::CubeTexture& cube, bool srgb);

    GLuint id() const { return m_id; }
    std::size_t byteSize() const { return m_bytes; }

private:
    GLuint m_id = 0;
    std::size_t m_bytes = 0;
};

} // namespace openhorizon::render
