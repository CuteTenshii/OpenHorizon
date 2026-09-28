#pragma once

#include "Gl.h"
#include "Shader.h"
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

/// The tone curve and exposure for turning the HDR frame into display
/// colours. The curve parameters are those of the game's filmicTone
/// element; the defaults are the curve's usual published values.
struct ToneSettings {
    float shoulderStrength = 0.22F;
    float linearStrength = 0.3F;
    float linearAngle = 0.1F;
    float toeStrength = 0.2F;
    float toeNumerator = 0.01F;
    float toeDenominator = 0.3F;
    float white = 11.2F;
    /// Multiplies the automatic exposure.
    float exposureBias = 1.0F;
    float vignettePower = 3.0F;
};

/// Owns the offscreen HDR target and the passes around the scene: clearing
/// to reversed depth, the sky, and tonemapping to the window.
class FrameRenderer {
public:
    FrameRenderer();
    ~FrameRenderer();
    FrameRenderer(const FrameRenderer&) = delete;
    FrameRenderer& operator=(const FrameRenderer&) = delete;

    /// Binds and clears the HDR target at `width` x `height` pixels and sets
    /// the depth state every scene pass expects.
    void begin(int width, int height);
    /// Draws the sky behind everything drawn so far.
    void sky(const FrameParams& frame);
    /// Tonemaps the HDR target onto the window's framebuffer, with exposure
    /// adapted to the frame's average brightness, and leaves the window
    /// bound for overlays.
    void finish(const ToneSettings& tone);

private:
    void resize(int width, int height);

    GLuint m_framebuffer = 0;
    GLuint m_colour = 0;
    GLuint m_depth = 0;
    GLuint m_emptyVao = 0;
    int m_width = 0;
    int m_height = 0;
    int m_levels = 1;
    Shader m_skyShader;
    Shader m_tonemapShader;
};

} // namespace fh::render
