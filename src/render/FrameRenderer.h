#pragma once

#include "CloudDefs.h"
#include "Clouds.h"
#include "Gl.h"
#include "PostProcess.h"
#include "Shader.h"
#include "SkyObjects.h"
#include "WorldRenderer.h"

#include <glm/glm.hpp>

namespace fh::render {

/// The camera for one frame.
struct Camera {
    glm::vec3 position{0.0F};
    glm::vec3 target{0.0F, 0.0F, 1.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    /// Vertical field of view, radians.
    float fov = 1.05F;
    float nearPlane = 0.1F;

    /// The view matrix for the game's left-handed world (x east, y up,
    /// z north), mirrored in Z so OpenGL's right-handed clip space shows it
    /// the right way round.
    glm::mat4 view() const;
    /// Infinite far plane with reversed depth (1 at the near plane, 0 at
    /// infinity), which keeps precision across a 25 km world.
    glm::mat4 projection(float aspect) const;
};

/// Owns the offscreen scene target and the passes around the scene:
/// clearing to reversed depth, the sky, and the game's post-processing to
/// the window.
///
/// The scene target holds the square root of linear colour, as the game's
/// does: its shaders end in `sqrt oC0`, blend in that space, and its
/// post-processing squares what it reads (see PostProcess.h). Every pass
/// drawing into the target writes that.
class FrameRenderer {
public:
    FrameRenderer();
    ~FrameRenderer();
    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;

    /// Binds and clears the HDR target at `width` x `height` pixels and sets
    /// the depth state every scene pass expects.
    void begin(int width, int height);
    /// Draws the sky behind everything drawn so far, with the sun, moon and
    /// stars when setSkyObjects() gave them.
    void sky(const FrameParams& frame);
    /// The sky in a face of the environment cube (EnvironmentCube), into the
    /// target bound: the dome and the sun, moon and stars. Inferred that the
    /// faces draw the sky: CRealtimeSky's draw (0x82E37A18) has a branch for
    /// the cube's pass (1), which draws the sun, moon and stars after the
    /// clouds (0x82E37A84-0x82E37AE8); the path from the face draw to it was
    /// not found. The clouds are left out, their cube map pass not being
    /// implemented (see Clouds).
    void cubeSky(const FrameParams& frame);
    /// What sky() draws over the dome; nullptr for none. Not owned.
    void setSkyObjects(SkyObjects* objects) { m_skyObjects = objects; }
    /// The cloud layers sky() draws after the sky objects, with their
    /// parameters and motion for the frame; nullptr for none. Not owned.
    void setClouds(Clouds* clouds, const CloudParameters* parameters, const CloudMotion* motion)
    {
        m_clouds = clouds;
        m_cloudParameters = parameters;
        m_cloudMotion = motion;
    }
    /// Runs the post-processing on the scene target into the output
    /// framebuffer and leaves it bound for overlays.
    void finish(const PostFrame& post);
    /// Where finish() writes the tonemapped picture: the window's
    /// framebuffer (0) unless the game draws into its own 16:9 picture.
    void setOutputFramebuffer(GLuint framebuffer) { m_output = framebuffer; }
    /// The scene colour (the square root of linear colour, before
    /// post-processing), for reading back in tests.
    GLuint sceneColour() const { return m_colour; }
    /// The scene depth (reversed: 0 where nothing was drawn), for reading
    /// back in tests.
    GLuint sceneDepth() const { return m_depth; }

private:
    void resize(int width, int height);
    /// The dome, where nothing was drawn (depth still at the cleared 0).
    void dome(const FrameParams& frame);

    GLuint m_framebuffer = 0;
    GLuint m_colour = 0;
    GLuint m_depth = 0;
    GLuint m_emptyVao = 0;
    GLuint m_output = 0;
    int m_width = 0;
    int m_height = 0;
    Shader m_skyShader;
    PostProcess m_post;
    SkyObjects* m_skyObjects = nullptr;
    Clouds* m_clouds = nullptr;
    const CloudParameters* m_cloudParameters = nullptr;
    const CloudMotion* m_cloudMotion = nullptr;
};

} // namespace fh::render
