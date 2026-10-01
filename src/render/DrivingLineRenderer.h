#pragma once

#include "game/DrivingLine.h"
#include "render/Shader.h"
#include "render/Texture.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::render {

/// Draws DrivingLine chevrons as the game's draw (0x82430e20) does, with
/// media/ui/textures/raceline.xds and a depth bias of 2e-5 (0x82000c68)
/// towards the camera, before tone mapping (a scene view stage, 0x824744d0;
/// tone map 0x82432628 is a separate step). Verified: shaders ported from
/// the executable's (pixel 0x8215C848). Inferred: the game's depth is
/// reversed as here, so a positive bias pulls towards the eye.
///
/// Left out: the game's stencil-equal-0 test (rear-view mirror mask,
/// 0x824a2fe0); nothing here writes stencil.
class DrivingLineRenderer {
public:
    /// Throws openhorizon::DataError if the texture is missing or cannot be decoded
    /// and std::runtime_error if the shaders do not compile.
    explicit DrivingLineRenderer(const Vfs& vfs);
    ~DrivingLineRenderer();
    DrivingLineRenderer(const DrivingLineRenderer&) = delete;
    DrivingLineRenderer& operator=(const DrivingLineRenderer&) = delete;

    /// `chevrons` holds four vertices each, in quad order. Expects and
    /// restores FrameRenderer::begin's depth state.
    void draw(const std::vector<DrivingLine::Vertex>& chevrons, const glm::mat4& viewProjection);

private:
    Shader m_shader;
    Texture m_texture;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    std::size_t m_capacity = 0;
    std::vector<DrivingLine::Vertex> m_triangles;
};

} // namespace openhorizon::render
