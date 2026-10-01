#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include <cstddef>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::render {

struct FrameParams;

/// The sun, moon and stars of CRealtimeSky (draw at 0x82E37A18), ported from
/// Sun_VS/PS and Star_VS/PS (0x82195848, 0x8216A0F8, 0x82195B68, 0x8216A288).
///
/// Sun or moon (0x82476DE0, made at 0x82E34C60): a quad 300 m away
/// (0x8224D2B8). Sun.xds added when SunObjectMoon <= 0.5, else
/// Moon0.xds (the only moon loaded, 0x82E342F0) alpha-blended. Parameters
/// come from offset 512 of the sky block (0x82E35624, 0x8265A7CC).
///
/// Stars (0x8242A618, made at 0x82E375F0): stars.bin holds a big-endian
/// float4 per star (unit direction, brightness); the rotation stays identity
/// (0x82E34200). Alpha StarFade * saturate((brightness - 0.25) / (0.97 -
/// 0.25)) ^ 7, the block's defaults (0x82656C08).
///
/// Both draw at the far plane. Inferred: MatWVP is the view without translation.
class SkyObjects {
public:
    /// Throws DataError if the textures or stars are missing or malformed.
    explicit SkyObjects(const Vfs& vfs);
    ~SkyObjects();
    SkyObjects(const SkyObjects&) = delete;
    SkyObjects& operator=(const SkyObjects&) = delete;

    /// Leaves blending off and depth writes on.
    void draw(const FrameParams& frame);

private:
    Shader m_sunShader;
    Shader m_starShader;
    Texture m_sun;
    Texture m_moon;
    GLuint m_sampler = 0;
    GLuint m_emptyVao = 0;
    GLuint m_starVao = 0;
    GLuint m_starBuffer = 0;
    std::size_t m_starCount = 0;
};

} // namespace openhorizon::render
