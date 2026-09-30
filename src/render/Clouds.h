#pragma once

#include "CloudDefs.h"
#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include <cstddef>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::render {

struct FrameParams;

/// The realtime sky's far and close cloud layers, drawn after the sun and
/// before the sky fog as CRealtimeSky's draw (0x82E37A18) orders them: far
/// clouds (0x82435FE0), then close clouds (0x82E33660).
///
/// Far clouds (verified against the draw, FarCloud_VS at 0x82195190 and
/// FarCloud_PS at 0x82169918): the dome of buildFarCloudDome() with, per
/// layer, two mask coordinates (the far object's fields 356-376: offsets
/// that CloudMotion scrolls and scales 1.01 and 3.2 set by the constructor
/// at 0x82E37EE8) and one map coordinate (fields 348/352 and 380/384, scale
/// 5.0 in field 412). FarCloudMask.xds is sampled at both mask coordinates
/// of each layer and FarCloudMapFront0.xds at each layer's map coordinate:
/// the pixel shader fetches both maps through texture fetch constant 1
/// (its constant table gives FrontAlphaMap and BackAlphaMap the same
/// register s1), so the back map, which the draw binds to s2, is never read
/// and not loaded here. Per layer the shader takes a density
/// `(1 - sharpness ^ (255 saturate((mask1 + mask2) / 2 - cover))) * map`,
/// with the
/// cover `mix(0.616, 0.43, Coverage)` and sharpness 0.984 (fields 420-428),
/// an alpha `density * mix(CentreAlpha, EdgeAlpha, weight ^ AlphaPower)`
/// and a colour `mix(ThinColour * ThinIntensity, DenseColour *
/// DenseIntensity, density)`, and composites the front layer over the back
/// one, dividing the colour by the combined alpha.
///
/// Close clouds (verified against the draw, CloseCloud_VS at 0x821949D8
/// and CloseCloud_Front_Only_PS at 0x82169250, the pixel shader the main
/// view uses; CloseCloud_PS is never created): the dome of
/// buildCloseCloudDome() with u = (1 - normal.w) + the front turn and
/// v = 1 - w, and the sun term `dot(normal, SunDirection) / 2 + 1/2`.
/// CloseCloudAlphaDensityFront0 (s0), CloseCloudTopBottomAmbientFront0
/// (s1) and CloseCloudAmbientFront0 (s2) give the colour `mix(Dense, Thin,
/// ambient.y * FrontAlpha) + Back * sun ^ BackPower * (1 - density.y *
/// FrontAlpha) + Top * sun ^ TopPower * (topBottom.x * FrontAlpha) ^
/// TopMaskPower + Bottom * sun ^ BottomPower * (topBottom.y * FrontAlpha) ^
/// BottomMaskPower` and the alpha `density.x * FrontAlpha * mix(TopAlpha,
/// BottomAlpha, (1 - w) ^ AlphaCurve)`, each light being its colour curve
/// times its intensity curve. The "Small" layer and the back textures are
/// used only by the cube map pass, which this renderer does not have.
///
/// Both use SunDirection = the normalised sun object direction (register
/// c4, 0x82474070, from the parameter block's 592, where the sampler puts
/// SunObjectTargetPos - SunObjectPos) and lift vertices below
/// cloudMinSkyHeight(). Both blend over the sky (source alpha, inverse
/// source alpha; render states 0x824361B8 and 0x82E33868) with no culling,
/// at the far plane (their vertex shaders write z = w - 1e-6), drawn where
/// the depth buffer still holds the far plane as the dome is. Samplers:
/// far clouds wrap in u and v, close clouds wrap in u and clamp in v, all
/// filtered linearly with linear mips (0x82436680, 0x82E33F24).
///
/// The textures (DXN, no gamma sign in their fetch constants) are linear.
/// Inferred, as for SkyObjects: that MatWVP is the camera's view without
/// its translation. Both pixel shaders end in `sqrt`, writing the
/// square-root colour the scene target holds. Known difference: the
/// cloud-set transitions (layer
/// fades in the far object's fields 264 and 332 and the close layer's
/// field 68) stay at 1, see CloudDefinitions.
class Clouds {
public:
    /// Loads the textures of `set`, whose four indices must be equal (which
    /// index feeds which layer object is not traced); throws DataError if
    /// they differ or a texture is missing or malformed.
    Clouds(const Vfs& vfs, const CloudDefinitions::Set& set);
    ~Clouds();
    Clouds(const Clouds&) = delete;
    Clouds& operator=(const Clouds&) = delete;

    /// Draws the far then the close layer where the depth buffer holds the
    /// far plane; leaves blending off and depth writes on.
    void draw(const FrameParams& frame, const CloudParameters& parameters, const CloudMotion& motion);

private:
    Shader m_farShader;
    Shader m_closeShader;
    Texture m_farMask;
    Texture m_farMap;
    Texture m_closeAlphaDensity;
    Texture m_closeTopBottomAmbient;
    Texture m_closeAmbient;
    GLuint m_farSampler = 0;
    GLuint m_closeSampler = 0;
    GLuint m_farVao = 0;
    GLuint m_farVertices = 0;
    GLuint m_farIndices = 0;
    GLsizei m_farIndexCount = 0;
    GLuint m_closeVao = 0;
    GLuint m_closeVertices = 0;
    GLuint m_closeIndices = 0;
    GLsizei m_closeIndexCount = 0;
};

} // namespace fh::render
