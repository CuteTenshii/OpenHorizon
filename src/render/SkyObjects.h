#pragma once

#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include <cstddef>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::render {

struct FrameParams;

/// The sun, the moon and the stars, drawn over the sky dome as the game's
/// realtime sky draws them (CRealtimeSky, whose draw at 0x82E37A18 draws
/// the stars, then the sun or moon, then the clouds). The shaders follow
/// the executable's Sun_VS, Sun_PS, Star_VS and Star_PS (at 0x82195848,
/// 0x8216A0F8, 0x82195B68 and 0x8216A288) step by step; the assets come
/// from media/realtimesky.
///
/// The sun or moon (draw at 0x82476DE0, object made at 0x82E34C60):
/// a quad list of one quad with corners (-1, -1), (-1, 1), (1, 1), (1, -1)
/// is placed 300 m from the camera, centred on `-SunDirection * 300`
/// (SunSize.x, 0x8224D2B8) and spanned by two axes the vertex shader
/// builds from the direction, scaled by the SunObjectSize curve. Sun.xds
/// is drawn when SunObjectMoon is at most one half, added to the sky
/// (source alpha, one) with colour `clamp(SunObjectColour, 0, 1) *
/// SunObjectAlpha` and alpha 1; otherwise Moon0.xds, the only moon texture
/// the object loads (0x82E342F0), blended over the sky (source alpha,
/// inverse source alpha) with colour `clamp(SunObjectColour, 0, 1)` and
/// alpha SunObjectAlpha. The pixel shader multiplies the texture by that
/// colour. Textures clamp at their edges and filter linearly. The values
/// reach the object through the sky's parameter block, copied whole into
/// it (0x82E35624: 48 bytes from offset 512, where the time-of-day sampler
/// at 0x8265A7CC puts SunObjectAlpha, SunObjectSize, CubeSunObjectSize,
/// SunObjectMoon and SunObjectColour in that order).
///
/// The stars (draw at 0x8242A618, made at 0x82E375F0): stars.bin holds one
/// big-endian float4 per star, a unit direction and a brightness, drawn as
/// a point list. The vertex shader rotates the direction by a matrix the
/// object keeps as the identity (set at 0x82E34200 and never written
/// again). The pixel shader writes white with alpha
/// `StarFade * saturate((brightness - 0.25) / (0.97 - 0.25)) ^ 7`, blended
/// over the sky; 0.25, 0.97 and 7 are the parameter block's defaults
/// (0x82656C08), which no curve overrides.
///
/// Both are drawn only where the depth buffer still holds the far plane,
/// as the dome is: the game's vertex shaders put them there too (z = w -
/// 1e-6). Inferred, not read: that the game's MatWVP is the camera's view
/// without its translation, which the stars' unit distance implies.
///
/// Known difference: the game blends in the square-root space its scene
/// target stores (every one of these pixel shaders but the stars' ends in
/// `sqrt`, and the post-processing squares the target back), while this
/// renderer blends linear colour.
class SkyObjects {
public:
    /// Loads the textures and stars; throws DataError if they are missing
    /// or malformed.
    explicit SkyObjects(const Vfs& vfs);
    ~SkyObjects();
    SkyObjects(const SkyObjects&) = delete;
    SkyObjects& operator=(const SkyObjects&) = delete;

    /// Draws the stars, then the sun or the moon, where the depth buffer
    /// holds the far plane; leaves blending off and depth writes on.
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

} // namespace fh::render
