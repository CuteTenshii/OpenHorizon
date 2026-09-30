#include "FrameRenderer.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <stdexcept>
#include <string>

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

const char* const kSkyFragmentBody = R"(
in vec2 vUv;
uniform mat4 uInverseViewProjection;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec4 far = uInverseViewProjection * vec4(vUv * 2.0 - 1.0, 0.0, 1.0);
    vec4 near = uInverseViewProjection * vec4(vUv * 2.0 - 1.0, 1.0, 1.0);
    vec3 direction = normalize(far.xyz / far.w - near.xyz / near.w);
    // The scene target holds the square root of colour, as the game's sky
    // shaders write it (Sky_PS and SkyFog_PS end in `sqrt oC0`).
    fragColour = vec4(sqrt(max(skyColour(direction), vec3(0.0))), 1.0);
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
    : m_skyShader("sky", kFullscreenVertex, std::string("#version 450 core\n") + kSceneLightingGlsl + kSkyFragmentBody)
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
    // Half floats hold the square-root colour with room above the [0, 4]
    // the game's 7e3 target (inferred from its cube map faces, 0x82426C38)
    // blends in; the post-processing clamps to the resolve's [0, 4].
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
    dome(frame);
    if (m_skyObjects != nullptr) {
        m_skyObjects->draw(frame);
    }
    // CRealtimeSky's draw (0x82E37A18) draws the far, then the close clouds
    // after the sun, moon and stars.
    if (m_clouds != nullptr && m_cloudParameters != nullptr && m_cloudMotion != nullptr) {
        m_clouds->draw(frame, *m_cloudParameters, *m_cloudMotion);
    }
}

void FrameRenderer::dome(const FrameParams& frame)
{
    m_skyShader.use();
    m_skyShader.set("uInverseViewProjection", glm::inverse(frame.projection * frame.view));
    setSceneLighting(m_skyShader, frame.lighting);
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

void FrameRenderer::finish(const PostFrame& post)
{
    m_post.run(m_colour, m_width, m_height, m_output, post);
}

} // namespace fh::render
