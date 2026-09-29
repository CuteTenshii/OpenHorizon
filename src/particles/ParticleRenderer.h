#pragma once

#include "render/Gl.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::particles {

class ParticleManager;
class ParticleSystem;

/// Per-frame values the particle shaders take from the game's lighting and
/// camera.
struct ParticleFrame {
    glm::mat4 viewProjection{1.0F};
    glm::vec3 cameraPosition{0.0F};
    /// Unit vector the camera looks along; particle depth is measured on it.
    glm::vec3 viewForward{0.0F, 0.0F, 1.0F};
    /// Near plane of the renderer's projection (infinite far plane, reversed
    /// depth, depth = near / view depth), for soft particles.
    float nearPlane = 0.1F;
    /// Direction towards the sun (the shaders' LightDir takes the negated
    /// direction the manager stores at +8160; which way that one points is
    /// inferred from the lighting maths, not read).
    glm::vec3 towardsSun{0.0F, 1.0F, 0.0F};
    glm::vec3 sunColour{1.0F};
    glm::vec3 ambientColour{0.0F};
    /// The car whose headlights light particles (CarPosition, CarUpVector,
    /// CarRightVector). The default is far away, which switches that light
    /// off: the shader fades it out over 35 m.
    glm::vec3 headLightCarPosition{1.0e9F};
    glm::vec3 headLightCarUp{0.0F, 1.0F, 0.0F};
    glm::vec3 headLightCarRight{1.0F, 0.0F, 0.0F};
};

/// Draws the particle systems of a ParticleManager with shaders written from
/// the game's particles_* shaders (ParticleShaders.h), their textures from
/// media/effects and the game's blending: premultiplied alpha (ONE,
/// INV_SRC_ALPHA) for "alpha" and ONE, ONE for "additive" effects (the
/// render states 0x82DBF5F0 sets, read as D3D9 blend values).
///
/// Differences from the game, all for lack of engine features or data:
/// the particles are not shadowed (no shadow map; the shader's shadow term
/// is 1); alpha-blended particles skip the final square root the game's
/// pixel shaders apply, which encodes into the console's gamma-2 target,
/// because the target here is linear HDR; for the same reason textures
/// whose fetch constant marks them gamma are sampled as sRGB; the
/// RelativeTransform the renderer can pass is identity (world systems);
/// particles are stored as floats where the game uses halves; Modeled
/// particles (car debris meshes) are not drawn.
///
/// Needs a current GL 4.5 context.
class ParticleRenderer {
public:
    explicit ParticleRenderer(const Vfs& vfs);
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    /// Draws every system into the bound draw framebuffer. Soft particles
    /// read that framebuffer's depth, which is first copied to a texture of
    /// the renderer's own (GL_DEPTH_COMPONENT32F, as the scene target uses).
    /// Leaves blending off, depth writes on and the viewport unchanged.
    void draw(const ParticleManager& manager, const ParticleFrame& frame);

    /// Particles drawn by the last draw().
    std::size_t lastDrawn() const { return m_lastDrawn; }
    /// Texture names of the effects that were not found on the disc.
    const std::vector<std::string>& missingTextures() const { return m_missing; }

private:
    struct Texture {
        GLuint id = 0;
        bool loaded = false;
    };

    GLuint texture(const std::string& name);
    void copyDepth(int width, int height);
    void drawSystem(const ParticleSystem& system, const ParticleFrame& frame);

    const Vfs* m_vfs;
    GLuint m_program = 0;
    GLuint m_vao = 0;
    GLuint m_records = 0;
    GLuint m_indices = 0;
    GLuint m_sampler = 0;
    GLuint m_depthSampler = 0;
    GLuint m_depthCopy = 0;
    GLuint m_depthFramebuffer = 0;
    int m_depthWidth = 0;
    int m_depthHeight = 0;
    std::size_t m_recordCapacity = 0;
    std::size_t m_indexCapacity = 0;
    std::size_t m_lastDrawn = 0;
    std::map<std::string, Texture> m_textures;
    std::vector<std::string> m_missing;
    std::map<std::string, GLint> m_uniforms;

    GLint uniform(const char* name);
};

} // namespace fh::particles
