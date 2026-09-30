#include "ShadowPass.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace fh::render {

namespace {

/// Shadows are drawn while 1 - ShadowIntensity is below this (0x82000CB0,
/// compared at 0x82482F28).
constexpr float kActiveBelow = 0.99F;
/// The maps' depth bias (+0xF84 = 5e-5 for the levels, +0x209C = 2e-4 for
/// the cascades; set as D3DRS_DEPTHBIAS at 0x82433D58 and 0x824344B0) in
/// Direct3D's depth units, and the slope-scaled bias (+0xF88, +0x20A0: 2).
/// The maps are 24-bit, as the game's D24S8 ones (0x82EA15D8), so a unit of
/// OpenGL's polygon offset is 2^-24 of the depth range.
constexpr float kLevelDepthBias = 5e-5F;
constexpr float kCascadeDepthBias = 2e-4F;
constexpr float kSlopeBias = 2.0F;
constexpr float kDepthUnitsPerRange = 16777216.0F;
/// The blur's quad lies at this view distance (+0x20F0, 0x82EA6A88).
constexpr float kBlurDistance = 250.0F;
/// RotVariation (+0x240, the constructor's 0x82256100).
constexpr glm::vec2 kRotVariation(8.4811F, 5.49169F);
/// The inner box of levels 1 and 2 is faded into by the next level over
/// this share of its tile (+0x254 = ratio × 0.25, 0x82483C64, with the
/// 0.25 at 0x82000D04).
constexpr float kTileHalf = 0.25F;

/// The quads of the light box (the index list at 0x833901BC), over the
/// corners of shadowmath::boxCorners().
constexpr std::array<std::array<int, 4>, 6> kBoxFaces{{
    {0, 1, 2, 3},
    {4, 7, 6, 5},
    {0, 4, 5, 1},
    {1, 5, 6, 2},
    {3, 2, 6, 7},
    {4, 0, 3, 7},
}};

const char* const kBoxVertex = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
uniform mat4 uViewProjection;
void main()
{
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
}
)";

const char* const kApplyFragment = R"(
layout(binding = 0) uniform sampler2D uDepth;
layout(binding = 1) uniform sampler2D uMap;
uniform float uNear;
uniform vec2 uTan;
uniform vec2 uSize;
uniform vec3 uRight;
uniform vec3 uUp;
uniform vec3 uForward;
uniform mat4 uTexture;
uniform vec4 uTaps[2];
uniform float uLitOffset;
uniform vec2 uDepthFade;
uniform vec4 uSurfaceFade;
uniform vec2 uRotation;
layout(location = 0) out vec4 fragColour;

float viewDepth(ivec2 pixel)
{
    ivec2 at = clamp(pixel, ivec2(0), ivec2(uSize) - 1);
    return uNear / texelFetch(uDepth, at, 0).r;
}

// The kernel of FilterTaps c32, c33 scaled by k and turned by (sin, cos),
// in the order the microcode fetches it; lit where the map is at least as
// far as the receiver (sge).
float taps(vec2 uv, float receiver, vec2 turn, float k)
{
    vec4 a = uTaps[0] * k;
    vec4 b = uTaps[1] * k;
    vec4 s = vec4(texture(uMap, uv + vec2(turn.x * a.x, turn.y * a.y)).r,
        texture(uMap, uv + vec2(turn.y * a.z, turn.x * a.w)).r,
        texture(uMap, uv + vec2(turn.x * b.x, turn.y * b.y)).r,
        texture(uMap, uv + vec2(turn.y * b.z, turn.x * b.w)).r);
    return dot(step(vec4(receiver), s), vec4(0.25));
}

void main()
{
    // VPOS: whole pixels from the top left; the mask's rows run from the top.
    vec2 pixel = floor(gl_FragCoord.xy);
    ivec2 at = ivec2(pixel);
    float z = viewDepth(at);
    vec2 ndc = vec2((pixel.x + 0.5) / uSize.x * 2.0 - 1.0, 1.0 - (pixel.y + 0.5) / uSize.y * 2.0);
    vec3 relative = uForward * z + uRight * (ndc.x * uTan.x * z) + uUp * (ndc.y * uTan.y * z);
    vec4 mapped = uTexture * vec4(relative, 1.0);
    vec2 uv = clamp(mapped.xy, 0.0, 1.0);
    float receiver = clamp(mapped.z, 0.0, 1.0);
    float angle = 6.2831855 * fract(dot(pixel, uRotation) * 0.15915494 + 0.5) - 3.1415927;
    vec2 turn = vec2(sin(angle), cos(angle));
#if SPREAD
    float lit;
    if (z < 25.0) {
        lit = taps(uv, receiver, turn, 1.0);
    } else {
        // How steeply the surface runs away from the view, from the depth
        // of the pixel below (the fetch offset 0.5, 1.5).
        float spread = clamp((viewDepth(at + ivec2(0, 1)) - z - 0.5) * 0.25, 0.0, 1.0);
        lit = taps(uv, receiver, turn, 1.0 + 8.0 * spread);
        if (spread > 0.5) {
            lit = 0.5 * (lit + taps(uv, receiver, turn, 1.0 + 16.0 * spread));
        }
    }
#else
    float lit = taps(uv, receiver, turn, 1.0);
#endif
    float fade = 0.0;
#if FADING_DEPTH
    fade = clamp(uDepthFade.y + uDepthFade.x * z, 0.0, 1.0);
#elif FADING_UV
    vec2 edge = 0.23 - abs(0.75 - mapped.xy);
    fade = clamp(1.0 - 40.0 * min(edge.x, edge.y), 0.0, 1.0);
#elif FADE_OUTER
    vec2 edge = 0.22 - abs(0.25 - (mapped.xy + uSurfaceFade.zw));
    fade = clamp(1.0 - 40.0 * min(edge.x, edge.y), 0.0, 1.0);
#elif FADE_INNER
    vec2 edge = uSurfaceFade.y - abs(0.25 - (mapped.xy + uSurfaceFade.zw));
    fade = clamp(100.0 * min(edge.x, edge.y) - 2.0, 0.0, 1.0);
#endif
    fragColour = vec4(uLitOffset + fade + lit, 1.0, 0.0, 0.0);
}
)";

const char* const kBlurVertex = R"(#version 450 core
uniform float uDepthValue;
void main()
{
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, uDepthValue, 1.0);
}
)";

// BlurShadowMask (post effect PS at 0x821D97E0): four fetches half a texel
// to either side, averaged.
const char* const kBlurFragment = R"(#version 450 core
layout(binding = 0) uniform sampler2D uMask;
uniform vec2 uTexel;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec2 uv = gl_FragCoord.xy * uTexel;
    float sum = texture(uMask, uv + vec2(-0.5, -0.5) * uTexel).r + texture(uMask, uv + vec2(0.5, -0.5) * uTexel).r
        + texture(uMask, uv + vec2(-0.5, 0.5) * uTexel).r + texture(uMask, uv + vec2(0.5, 0.5) * uTexel).r;
    fragColour = vec4(0.25 * sum, 1.0, 0.0, 0.0);
}
)";

std::string applySource(const char* defines)
{
    return std::string("#version 450 core\n") + defines + kApplyFragment;
}

GLuint depthMap(int size)
{
    GLuint id = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &id);
    glTextureStorage2D(id, 1, GL_DEPTH_COMPONENT24, size, size);
    // The shadow samplers are point sampled, clamped to a white border
    // (0x82482CD0..0x82482E00: filters 0, clamp 6, border colour 1).
    glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    const float border[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    glTextureParameterfv(id, GL_TEXTURE_BORDER_COLOR, border);
    glTextureParameteri(id, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    return id;
}

GLuint depthFramebuffer(GLuint texture)
{
    GLuint id = 0;
    glCreateFramebuffers(1, &id);
    glNamedFramebufferTexture(id, GL_DEPTH_ATTACHMENT, texture, 0);
    glNamedFramebufferDrawBuffer(id, GL_NONE);
    glNamedFramebufferReadBuffer(id, GL_NONE);
    if (glCheckNamedFramebufferStatus(id, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("a shadow map framebuffer is incomplete");
    }
    return id;
}

/// Shadow map coordinates of a camera-relative position for a map covering
/// `scale` of the texture from `offset`.
glm::mat4 textureMatrix(const ShadowFrustum& frustum, const glm::vec3& camera, const glm::vec2& offset, float scale)
{
    glm::mat4 bias(1.0F);
    bias[0][0] = 0.5F * scale;
    bias[1][1] = 0.5F * scale;
    bias[3][0] = 0.5F * scale + offset.x;
    bias[3][1] = 0.5F * scale + offset.y;
    return bias * frustum.viewProjection * glm::translate(glm::mat4(1.0F), camera);
}

/// The level's box as the orchestrator draws it: the level's basis and
/// projection around the camera (0x82483720..0x82483768).
ShadowFrustum levelBox(const ShadowCascades::Level& level, const glm::vec3& camera)
{
    ShadowFrustum box = level.frustum;
    box.world[3] = glm::vec4(camera, 1.0F);
    box.view = glm::inverse(box.world);
    box.viewProjection = box.projection * box.view;
    return box;
}

glm::vec2 tileOffset(std::size_t level)
{
    return {0.5F * static_cast<float>(level & 1), 0.5F * static_cast<float>(level >> 1)};
}

} // namespace

ShadowPass::ShadowPass()
    : m_apply{Shader("shadow apply lq", kBoxVertex, applySource("")),
          Shader("shadow apply lq fading depth", kBoxVertex, applySource("#define FADING_DEPTH 1\n")),
          Shader("shadow apply lq fading uv", kBoxVertex, applySource("#define FADING_UV 1\n")),
          Shader("shadow apply lq spread", kBoxVertex, applySource("#define SPREAD 1\n")),
          Shader(
              "shadow apply lq fade outer spread", kBoxVertex, applySource("#define SPREAD 1\n#define FADE_OUTER 1\n")),
          Shader(
              "shadow apply lq fade inner spread", kBoxVertex, applySource("#define SPREAD 1\n#define FADE_INNER 1\n")),
          Shader("shadow apply lq fade inner", kBoxVertex, applySource("#define FADE_INNER 1\n"))}
    , m_blur("shadow blur", kBlurVertex, kBlurFragment)
{
    m_levelMap = depthMap(ShadowCascades::kAtlasResolution);
    m_levelFramebuffer = depthFramebuffer(m_levelMap);
    ShadowCascades layout;
    for (std::size_t i = 0; i < ShadowCascades::kCascades; ++i) {
        m_cascadeMaps[i] = depthMap(layout.cascades()[i].resolution);
        m_cascadeFramebuffers[i] = depthFramebuffer(m_cascadeMaps[i]);
    }
    glCreateFramebuffers(1, &m_screenFramebuffer);
    glCreateFramebuffers(1, &m_depthCopyFramebuffer);
    glCreateFramebuffers(1, &m_maskCopyFramebuffer);
    glCreateFramebuffers(1, &m_depthOnlyFramebuffer);
    glCreateVertexArrays(1, &m_emptyVao);
    glCreateBuffers(1, &m_boxBuffer);
    glNamedBufferStorage(m_boxBuffer, static_cast<GLsizeiptr>(36 * sizeof(glm::vec3)), nullptr, GL_DYNAMIC_STORAGE_BIT);
    glCreateVertexArrays(1, &m_boxVao);
    glVertexArrayVertexBuffer(m_boxVao, 0, m_boxBuffer, 0, static_cast<GLsizei>(sizeof(glm::vec3)));
    glEnableVertexArrayAttrib(m_boxVao, 0);
    glVertexArrayAttribFormat(m_boxVao, 0, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_boxVao, 0, 0);
    for (auto& frame : m_queries) {
        glGenQueries(static_cast<GLsizei>(frame.size()), frame.data());
    }
}

ShadowPass::~ShadowPass()
{
    for (auto& frame : m_queries) {
        glDeleteQueries(static_cast<GLsizei>(frame.size()), frame.data());
    }
    glDeleteVertexArrays(1, &m_boxVao);
    glDeleteBuffers(1, &m_boxBuffer);
    glDeleteVertexArrays(1, &m_emptyVao);
    glDeleteFramebuffers(1, &m_depthOnlyFramebuffer);
    glDeleteFramebuffers(1, &m_maskCopyFramebuffer);
    glDeleteFramebuffers(1, &m_depthCopyFramebuffer);
    glDeleteFramebuffers(1, &m_screenFramebuffer);
    glDeleteFramebuffers(static_cast<GLsizei>(m_cascadeFramebuffers.size()), m_cascadeFramebuffers.data());
    glDeleteTextures(static_cast<GLsizei>(m_cascadeMaps.size()), m_cascadeMaps.data());
    glDeleteFramebuffers(1, &m_levelFramebuffer);
    glDeleteTextures(1, &m_levelMap);
    const std::array<GLuint, 4> screen{m_depth, m_depthCopy, m_mask, m_maskCopy};
    glDeleteTextures(static_cast<GLsizei>(screen.size()), screen.data());
}

bool ShadowPass::active(float shadowIntensity)
{
    return 1.0F - shadowIntensity < kActiveBelow;
}

glm::mat4 ShadowPass::flippedViewProjection(const glm::mat4& viewProjection)
{
    return glm::scale(glm::mat4(1.0F), glm::vec3(1.0F, -1.0F, 1.0F)) * viewProjection;
}

void ShadowPass::resizeScreen(int width, int height)
{
    width = std::max(width, 1);
    height = std::max(height, 1);
    if (width == m_width && height == m_height) {
        return;
    }
    m_width = width;
    m_height = height;
    const std::array<GLuint, 4> old{m_depth, m_depthCopy, m_mask, m_maskCopy};
    glDeleteTextures(static_cast<GLsizei>(old.size()), old.data());
    const auto make = [&](GLenum format, GLenum filter) {
        GLuint id = 0;
        glCreateTextures(GL_TEXTURE_2D, 1, &id);
        glTextureStorage2D(id, 1, format, width, height);
        glTextureParameteri(id, GL_TEXTURE_MIN_FILTER, filter);
        glTextureParameteri(id, GL_TEXTURE_MAG_FILTER, filter);
        glTextureParameteri(id, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(id, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        return id;
    };
    m_depth = make(GL_DEPTH32F_STENCIL8, GL_NEAREST);
    m_depthCopy = make(GL_DEPTH32F_STENCIL8, GL_NEAREST);
    // The mask's sampler for the blur is bilinear and clamped to its edge
    // (0x824840EC..0x82484134); the scene's effects read it the same way.
    m_mask = make(GL_RGBA8, GL_LINEAR);
    m_maskCopy = make(GL_RGBA8, GL_LINEAR);
    glNamedFramebufferTexture(m_depthOnlyFramebuffer, GL_DEPTH_STENCIL_ATTACHMENT, m_depth, 0);
    glNamedFramebufferDrawBuffer(m_depthOnlyFramebuffer, GL_NONE);
    glNamedFramebufferTexture(m_screenFramebuffer, GL_COLOR_ATTACHMENT0, m_mask, 0);
    glNamedFramebufferTexture(m_screenFramebuffer, GL_DEPTH_STENCIL_ATTACHMENT, m_depth, 0);
    glNamedFramebufferTexture(m_depthCopyFramebuffer, GL_DEPTH_STENCIL_ATTACHMENT, m_depthCopy, 0);
    glNamedFramebufferDrawBuffer(m_depthCopyFramebuffer, GL_NONE);
    glNamedFramebufferTexture(m_maskCopyFramebuffer, GL_COLOR_ATTACHMENT0, m_maskCopy, 0);
    for (const GLuint framebuffer :
        {m_depthOnlyFramebuffer, m_screenFramebuffer, m_depthCopyFramebuffer, m_maskCopyFramebuffer}) {
        if (glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("a shadow mask framebuffer is incomplete");
        }
    }
}

void ShadowPass::beginTimer(std::size_t which)
{
    if (m_timing != kTimers) {
        return;
    }
    glBeginQuery(GL_TIME_ELAPSED, m_queries[m_frame][which]);
    m_timing = which;
}

void ShadowPass::endTimer()
{
    if (m_timing == kTimers) {
        return;
    }
    glEndQuery(GL_TIME_ELAPSED);
    m_pending[m_frame][m_timing] = true;
    m_timing = kTimers;
}

void ShadowPass::collectTimers()
{
    m_frame = (m_frame + 1) % kTimerFrames;
    // The slot about to be reused holds the oldest frame's queries.
    std::array<double, kTimers> seconds{};
    bool any = false;
    for (std::size_t t = 0; t < kTimers; ++t) {
        if (!m_pending[m_frame][t]) {
            continue;
        }
        GLint available = 0;
        glGetQueryObjectiv(m_queries[m_frame][t], GL_QUERY_RESULT_AVAILABLE, &available);
        if (available == 0) {
            continue;
        }
        GLuint64 nanoseconds = 0;
        glGetQueryObjectui64v(m_queries[m_frame][t], GL_QUERY_RESULT, &nanoseconds);
        seconds[t] = static_cast<double>(nanoseconds) * 1e-9;
        m_pending[m_frame][t] = false;
        any = true;
    }
    if (any) {
        m_timings = Timings{seconds[0], seconds[1], seconds[2]};
    }
}

void ShadowPass::beginLevel(std::size_t index)
{
    beginTimer(0);
    const int size = ShadowCascades::kLevelResolution;
    const int x = static_cast<int>(index & 1) * size;
    const int y = static_cast<int>(index >> 1) * size;
    glBindFramebuffer(GL_FRAMEBUFFER, m_levelFramebuffer);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glViewport(x, y, size, size);
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, size, size);
    glDepthMask(GL_TRUE);
    // The clear depth is the level's +0x144 (1.0, 0x82E9F5F8).
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    // Light space keeps the world's handedness, so faces turned to the
    // light wind clockwise in its clip space.
    glFrontFace(GL_CW);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(kSlopeBias, kLevelDepthBias * kDepthUnitsPerRange);
}

void ShadowPass::beginCascade(std::size_t index)
{
    beginTimer(0);
    ShadowCascades layout;
    const int size = layout.cascades()[index].resolution;
    glBindFramebuffer(GL_FRAMEBUFFER, m_cascadeFramebuffers[index]);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glDisable(GL_SCISSOR_TEST);
    glViewport(0, 0, size, size);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glFrontFace(GL_CW);
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(kSlopeBias, kCascadeDepthBias * kDepthUnitsPerRange);
}

void ShadowPass::endShadowMaps()
{
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(0.0F, 0.0F);
    glFrontFace(GL_CCW);
    glDepthFunc(GL_GREATER);
    glClearDepth(0.0);
    endTimer();
}

void ShadowPass::beginDepth(int width, int height)
{
    resizeScreen(width, height);
    beginTimer(1);
    glBindFramebuffer(GL_FRAMEBUFFER, m_depthOnlyFramebuffer);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_TRUE);
    glStencilMask(0xFF);
    glClearNamedFramebufferfi(m_depthOnlyFramebuffer, GL_DEPTH_STENCIL, 0, 0.0F, 0);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    // The flip in y turns the scene's counter-clockwise front faces
    // clockwise.
    glFrontFace(GL_CW);
}

void ShadowPass::endDepth()
{
    glFrontFace(GL_CCW);
    glBlitNamedFramebuffer(m_depthOnlyFramebuffer, m_depthCopyFramebuffer, 0, 0, m_width, m_height, 0, 0, m_width,
        m_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    endTimer();
}

void ShadowPass::clearMask(int width, int height)
{
    resizeScreen(width, height);
    const float white[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    glClearNamedFramebufferfv(m_screenFramebuffer, GL_COLOR, 0, white);
    collectTimers();
}

void ShadowPass::drawApply(const Apply& apply, const ShadowFrustum& box, float shrink, bool incrementStencil)
{
    const std::array<glm::vec3, 8> corners = shadowmath::boxCorners(box, shrink);
    glm::vec3 centre(0.0F);
    for (const glm::vec3& c : corners) {
        centre += c * 0.125F;
    }
    // The back of the box: the game draws its far side and keeps what lies
    // in front of it (depth "further or equal" with its reversed depth).
    std::array<glm::vec3, 36> vertices{};
    GLsizei count = 0;
    for (const auto& face : kBoxFaces) {
        glm::vec3 faceCentre(0.0F);
        for (const int k : face) {
            faceCentre += corners[static_cast<std::size_t>(k)] * 0.25F;
        }
        if (glm::dot(faceCentre - centre, faceCentre - m_camera.position) <= 0.0F) {
            continue;
        }
        for (const int k : {0, 1, 2, 0, 2, 3}) {
            vertices[static_cast<std::size_t>(count++)]
                = corners[static_cast<std::size_t>(face[static_cast<std::size_t>(k)])];
        }
    }
    if (count == 0) {
        return;
    }
    glNamedBufferSubData(
        m_boxBuffer, 0, static_cast<GLsizeiptr>(static_cast<std::size_t>(count) * sizeof(glm::vec3)), vertices.data());

    Shader& shader = m_apply[static_cast<std::size_t>(apply.variant)];
    shader.use();
    shader.set("uViewProjection", m_flipped);
    shader.set("uNear", m_camera.nearPlane);
    shader.set("uTan", glm::vec2(m_camera.tanHalfWidth, m_camera.tanHalfHeight));
    shader.set("uSize", glm::vec2(static_cast<float>(m_width), static_cast<float>(m_height)));
    shader.set("uRight", m_camera.right);
    shader.set("uUp", m_camera.up);
    shader.set("uForward", m_camera.forward);
    shader.set("uTexture", apply.texture);
    shader.set("uTaps[0]", apply.filter[0]);
    shader.set("uTaps[1]", apply.filter[1]);
    shader.set("uLitOffset", 1.0F - m_intensity);
    shader.set("uDepthFade", apply.depthFade);
    shader.set("uSurfaceFade", apply.surfaceFade);
    shader.set("uRotation", kRotVariation);
    glBindTextureUnit(0, m_depthCopy);
    glBindTextureUnit(1, apply.map);
    glStencilOp(GL_KEEP, GL_KEEP, incrementStencil ? GL_INCR_WRAP : GL_KEEP);
    glBindVertexArray(m_boxVao);
    glDrawArrays(GL_TRIANGLES, 0, count);
}

void ShadowPass::blur()
{
    glBlitNamedFramebuffer(m_screenFramebuffer, m_maskCopyFramebuffer, 0, 0, m_width, m_height, 0, 0, m_width, m_height,
        GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glDisable(GL_BLEND);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    m_blur.use();
    // The reversed depth of the quad's distance.
    m_blur.set("uDepthValue", m_camera.nearPlane / kBlurDistance);
    m_blur.set("uTexel", glm::vec2(1.0F / static_cast<float>(m_width), 1.0F / static_cast<float>(m_height)));
    glBindTextureUnit(0, m_maskCopy);
    glBindVertexArray(m_emptyVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void ShadowPass::buildMask(
    const ShadowCascades& cascades, const ShadowCamera& camera, const glm::mat4& viewProjection, float shadowIntensity)
{
    beginTimer(2);
    m_camera = camera;
    m_flipped = flippedViewProjection(viewProjection);
    m_intensity = shadowIntensity;
    glBindFramebuffer(GL_FRAMEBUFFER, m_screenFramebuffer);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_SCISSOR_TEST);
    const float white[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    glClearNamedFramebufferfv(m_screenFramebuffer, GL_COLOR, 0, white);
    glStencilMask(0xFF);
    glClear(GL_STENCIL_BUFFER_BIT);
    glDisable(GL_CULL_FACE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glEnable(GL_STENCIL_TEST);
    glStencilFunc(GL_EQUAL, 0, 0xFF);
    glDisable(GL_BLEND);

    const auto& c = cascades.cascades();
    for (std::size_t i = 0; i < c.size(); ++i) {
        Apply apply;
        const bool last = i + 1 == c.size();
        apply.variant = last ? Variant::LqFadingDepth : Variant::Lq;
        apply.map = m_cascadeMaps[i];
        apply.texture = textureMatrix(c[i].frustum, camera.position, glm::vec2(0.0F), 1.0F);
        apply.filter = shadowmath::filterTaps(c[i].filter / static_cast<float>(c[i].resolution));
        apply.depthFade = shadowmath::depthFade(c.back().reach);
        drawApply(apply, c[i].frustum, c[i].shrink, true);
    }

    glClear(GL_STENCIL_BUFFER_BIT);
    glEnable(GL_BLEND);
    glBlendEquation(GL_MIN);
    glBlendFunc(GL_ONE, GL_ONE);
    const auto& levels = cascades.levels();
    const float atlasScale = 0.5F;
    const auto levelTexture
        = [&](std::size_t i) { return textureMatrix(levels[i].frustum, camera.position, tileOffset(i), atlasScale); };
    const auto surfaceFade
        = [&](std::size_t i, float inner) { return glm::vec4(0.5F, inner, -tileOffset(i).x, -tileOffset(i).y); };
    for (std::size_t i = 0; i < levels.size(); ++i) {
        const ShadowFrustum box = levelBox(levels[i], camera.position);
        Apply apply;
        apply.map = m_levelMap;
        apply.texture = levelTexture(i);
        apply.filter = shadowmath::filterTaps(
            ShadowCascades::levelFilter(i) / static_cast<float>(ShadowCascades::kAtlasResolution));
        if (i == 0) {
            apply.variant = Variant::Lq;
            drawApply(apply, box, levels[i].shrink, true);
        } else if (i + 1 == levels.size()) {
            apply.variant = Variant::LqFadingUv;
            drawApply(apply, box, levels[i].shrink, true);
        } else {
            apply.variant = Variant::LqSpread;
            drawApply(apply, box, levels[i].innerShrink, true);
            apply.variant = Variant::LqFadeOuterSpread;
            apply.surfaceFade = surfaceFade(i, 0.0F);
            drawApply(apply, box, levels[i].shrink, false);
            const std::size_t next = i + 1;
            const float ratio
                = levels[i].range * (1.0F + levels[i].margin) / (levels[next].range * (1.0F + levels[next].margin));
            apply.variant = next == 1 || next == 2 ? Variant::LqFadeInnerSpread : Variant::LqFadeInner;
            apply.texture = levelTexture(next);
            apply.surfaceFade = surfaceFade(next, ratio * kTileHalf);
            drawApply(apply, box, levels[i].shrink, true);
        }
    }
    blur();

    glDisable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glDisable(GL_STENCIL_TEST);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_GREATER);
    glEnable(GL_CULL_FACE);
    glBindVertexArray(0);
    glUseProgram(0);
    endTimer();
    collectTimers();
}

} // namespace fh::render
