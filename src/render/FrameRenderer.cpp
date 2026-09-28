#include "FrameRenderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <stdexcept>

namespace fh::render {

namespace {

const char* const kFullscreenVertex = R"(#version 450 core
out vec2 vUv;
void main()
{
    // One triangle covering the screen, from the vertex index alone.
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUv = corner;
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

const char* const kSkyFragment = R"(#version 450 core
in vec2 vUv;
uniform mat4 uInverseViewProjection;
uniform vec3 uSunDirection;
uniform vec3 uSunColour;
uniform vec3 uSkyColour;
uniform vec3 uFogColour;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec4 far = uInverseViewProjection * vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
    vec4 near = uInverseViewProjection * vec4(vUv * 2.0 - 1.0, 1.0, 1.0);
    vec3 direction = normalize(far.xyz / far.w - near.xyz / near.w);
    float height = clamp(direction.y, -0.2, 1.0);
    // Deep blue overhead fading into the haze at the horizon.
    vec3 zenith = uSkyColour * vec3(0.55, 0.7, 1.0);
    vec3 colour = mix(uFogColour, zenith, pow(max(height, 0.0), 0.45));
    float sun = max(dot(direction, -uSunDirection), 0.0);
    colour += uSunColour * (pow(sun, 1200.0) * 40.0 + pow(sun, 12.0) * 0.25);
    fragColour = vec4(colour, 1.0);
}
)";

const char* const kTonemapFragment = R"(#version 450 core
in vec2 vUv;
layout(binding = 0) uniform sampler2D uScene;
uniform float uExposure;
layout(location = 0) out vec4 fragColour;
// ACES filmic curve (Narkowicz's fit), then sRGB encoding by the
// framebuffer.
vec3 aces(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}
void main()
{
    vec3 hdr = texture(uScene, vUv).rgb * uExposure;
    fragColour = vec4(aces(hdr), 1.0);
}
)";

} // namespace

glm::mat4 Camera::view() const
{
    const glm::mat4 mirror = glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, 1.0F, -1.0F));
    const glm::vec3 eye = glm::vec3(mirror * glm::vec4(position, 1.0F));
    const glm::vec3 at = glm::vec3(mirror * glm::vec4(target, 1.0F));
    const glm::vec3 upMirrored = glm::vec3(mirror * glm::vec4(up, 0.0F));
    return glm::lookAt(eye, at, upMirrored) * mirror;
}

glm::mat4 Camera::projection(float aspect) const
{
    const float f = 1.0F / std::tan(fov * 0.5F);
    glm::mat4 m(0.0F);
    m[0][0] = f / aspect;
    m[1][1] = f;
    m[2][3] = -1.0F;
    m[3][2] = nearPlane;
    return m;
}

FrameRenderer::FrameRenderer()
    : m_skyShader("sky", kFullscreenVertex, kSkyFragment)
    , m_tonemapShader("tonemap", kFullscreenVertex, kTonemapFragment)
{
    glCreateVertexArrays(1, &m_emptyVao);
    glCreateFramebuffers(1, &m_framebuffer);
}

FrameRenderer::~FrameRenderer()
{
    glDeleteFramebuffers(1, &m_framebuffer);
    glDeleteTextures(1, &m_colour);
    glDeleteTextures(1, &m_depth);
    glDeleteVertexArrays(1, &m_emptyVao);
}

void FrameRenderer::resize(int width, int height)
{
    if (width == m_width && height == m_height) {
        return;
    }
    m_width = width;
    m_height = height;
    if (m_colour) {
        glDeleteTextures(1, &m_colour);
        glDeleteTextures(1, &m_depth);
    }
    glCreateTextures(GL_TEXTURE_2D, 1, &m_colour);
    glTextureStorage2D(m_colour, 1, GL_RGBA16F, width, height);
    glTextureParameteri(m_colour, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_colour, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_depth);
    glTextureStorage2D(m_depth, 1, GL_DEPTH_COMPONENT32F, width, height);
    glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_colour, 0);
    glNamedFramebufferTexture(m_framebuffer, GL_DEPTH_ATTACHMENT, m_depth, 0);
    if (glCheckNamedFramebufferStatus(m_framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("the HDR framebuffer is incomplete");
    }
}

void FrameRenderer::begin(int width, int height)
{
    resize(std::max(width, 1), std::max(height, 1));
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, m_width, m_height);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    // The world's triangles wind clockwise seen from their front in the
    // game's left-handed space; the view's Z mirror makes that
    // counter-clockwise on screen.
    glFrontFace(GL_CCW);
    const float clearColour[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    glClearNamedFramebufferfv(m_framebuffer, GL_COLOR, 0, clearColour);
    glClearNamedFramebufferfi(m_framebuffer, GL_DEPTH_STENCIL, 0, 0.0F, 0);
}

void FrameRenderer::sky(const FrameParams& frame)
{
    m_skyShader.use();
    m_skyShader.set("uInverseViewProjection", glm::inverse(frame.projection * frame.view));
    m_skyShader.set("uSunDirection", frame.sunDirection);
    m_skyShader.set("uSunColour", frame.sunColour);
    m_skyShader.set("uSkyColour", frame.skyColour);
    m_skyShader.set("uFogColour", frame.fogColour);
    // Only where nothing was drawn: depth still at the cleared 0.
    glDepthFunc(GL_EQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glBindVertexArray(m_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

void FrameRenderer::finish(float exposure)
{
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_FRAMEBUFFER_SRGB);
    m_tonemapShader.use();
    m_tonemapShader.set("uExposure", exposure);
    glBindTextureUnit(0, m_colour);
    glBindVertexArray(m_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindVertexArray(0);
}

} // namespace fh::render
