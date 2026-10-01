#pragma once

#include "CloudDefs.h"
#include "Gl.h"
#include "Shader.h"
#include "Texture.h"

#include <cstddef>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::render {

struct FrameParams;

/// The realtime sky's far and close cloud layers, drawn after the sun as
/// CRealtimeSky's draw (0x82E37A18) orders them.
///
/// Verified against the draws and shaders: far clouds (0x82435FE0,
/// FarCloud_VS 0x82195190, FarCloud_PS 0x82169918) and close clouds
/// (0x82E33660, CloseCloud_VS 0x821949D8, CloseCloud_Front_Only_PS
/// 0x82169250). FarCloud_PS reads both maps through s1, so the back map is
/// never loaded. The "Small" layer and back textures belong to the cube map
/// pass, not drawn here. Far mask scales 1.01 and 3.2 (0x82E37EE8).
/// SunDirection is c4 (0x82474070). Both blend by source alpha (0x824361B8,
/// 0x82E33868); far samplers wrap, close wrap u and clamp v, linear with
/// linear mips (0x82436680, 0x82E33F24).
///
/// Inferred, as for SkyObjects: MatWVP is the view without its translation.
/// Cloud-set transition fades stay at 1 (see CloudDefinitions).
class Clouds {
public:
    /// The four indices of `set` must be equal (the index-to-layer mapping is
    /// not traced); throws DataError otherwise or on a bad texture.
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

} // namespace openhorizon::render
