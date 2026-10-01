#pragma once

#include "render/Gl.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::particles {

class ParticleManager;
class ParticleSystem;

/// Per-frame values the particle shaders take from the game's lighting and camera.
struct ParticleFrame {
    glm::mat4 viewProjection{1.0F};
    glm::vec3 cameraPosition{0.0F};
    /// Unit vector the camera looks along; particle depth is measured on it.
    glm::vec3 viewForward{0.0F, 0.0F, 1.0F};
    /// For soft particles: depth = near / view depth (reversed, infinite far plane).
    float nearPlane = 0.1F;
    /// LightDir is the negated manager +8160; that it points away from the sun is inferred.
    glm::vec3 towardsSun{0.0F, 1.0F, 0.0F};
    glm::vec3 sunColour{1.0F};
    glm::vec3 ambientColour{0.0F};
    /// The car whose headlights light particles (CarPosition, CarUpVector, CarRightVector). The far default turns
    /// them off: the shader fades them over 35 m.
    glm::vec3 headLightCarPosition{1.0e9F};
    glm::vec3 headLightCarUp{0.0F, 1.0F, 0.0F};
    glm::vec3 headLightCarRight{1.0F, 0.0F, 0.0F};
};

/// Draws a ParticleManager's systems with ports of the game's particles_* shaders. Blending as 0x82DBF5F0 sets it:
/// ONE, INV_SRC_ALPHA for "alpha", ONE, ONE for "additive". Unlike the game: no shadows, gamma textures as sRGB,
/// identity RelativeTransform, float storage, no Modeled particles. Needs a current GL 4.5 context.
class ParticleRenderer {
public:
    explicit ParticleRenderer(const Vfs& vfs);
    ~ParticleRenderer();
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    /// Draws into the bound framebuffer, whose depth is copied first for soft particles (GL_DEPTH_COMPONENT32F).
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

} // namespace openhorizon::particles
