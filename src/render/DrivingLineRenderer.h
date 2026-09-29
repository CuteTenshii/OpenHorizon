#pragma once

#include "game/DrivingLine.h"
#include "render/Shader.h"
#include "render/Texture.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::render {

/// Draws DrivingLine chevrons the way the game's draw (0x82430e20) does:
/// media/ui/textures/raceline.xds (a white chevron in alpha only) clamped,
/// with anisotropic filtering, its alpha times the vertex alpha over the
/// vertex colour, blended by source alpha into the scene with depth testing
/// and no depth writes, no culling, and the depth bias of 2e-5
/// (0x82000c68) towards the camera. The shaders are written from the game's
/// own, embedded in the executable: the vertex shader transforms by View
/// and Projection and passes the colour and texture coordinate through, the
/// pixel shader outputs (colour.rgb, colour.a * texture.a).
///
/// It is drawn into the scene before tone mapping. Verified: the draw is a
/// stage of the scene view's render (CPresentationRaceLine, called from
/// 0x824744d0) that needs the scene's depth and stencil, and the final
/// combine with bloom and tone map (0x82432628) is a separate render-thread
/// step. Inferred: that step runs after the views.
///
/// Inferred: the console's scene target stores colours gamma-2 encoded (as
/// the game's particle shaders show by encoding their output with a square
/// root) and the line's shader writes its colour unencoded, so the colour
/// is squared into this renderer's linear target. The bias direction: the
/// game's depth is taken to run the same reversed way as here, so a
/// positive bias pulls the line towards the eye.
///
/// Left out: the game also tests stencil equal to 0, so that the line is
/// not drawn where the rear-view mirror mask (0x824a2fe0, ref 255) and
/// other passes marked the stencil first; nothing here writes stencil.
class DrivingLineRenderer {
public:
    /// Throws fh::DataError if the texture is missing or cannot be decoded
    /// and std::runtime_error if the shaders do not compile.
    explicit DrivingLineRenderer(const Vfs& vfs);
    ~DrivingLineRenderer();
    DrivingLineRenderer(const DrivingLineRenderer&) = delete;
    DrivingLineRenderer& operator=(const DrivingLineRenderer&) = delete;

    /// Draws `chevrons` (four vertices each, in quad order) into the bound
    /// scene target, whose depth state it expects as FrameRenderer::begin
    /// leaves it and restores afterwards.
    void draw(const std::vector<DrivingLine::Vertex>& chevrons, const glm::mat4& viewProjection);

private:
    Shader m_shader;
    Texture m_texture;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    std::size_t m_capacity = 0;
    std::vector<DrivingLine::Vertex> m_triangles;
};

} // namespace fh::render
