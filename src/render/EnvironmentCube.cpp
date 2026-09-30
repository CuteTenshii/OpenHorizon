#include "EnvironmentCube.h"

#include <array>
#include <cstddef>
#include <span>
#include <stdexcept>

namespace fh::render {

namespace {

using envcube::kSphereHeight;
using envcube::kSphereLevels;
using envcube::kSphereWidth;

const char* const kFullscreenVertex = R"(#version 450 core
void main()
{
    // One triangle covering the target, from the vertex index alone.
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

/// BlitSphericalMap: the cube's level 0 along envcube::sphereDirection(),
/// written squared, so the map holds linear colour. The source is sampled
/// bilinearly from its top level only (the blit helper, 0x824728D8, sets
/// the mip filter to the base map).
const char* const kToSphereFragment = R"(#version 450 core
layout(binding = 0) uniform samplerCube uCube;
uniform vec2 uSize;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec2 uv = gl_FragCoord.xy / uSize;
    float p = uv.x * 6.28318531 - 3.14159265;
    float t = uv.y * 3.14159265;
    vec3 d = vec3(sin(t) * cos(p), cos(t), sin(t) * sin(p));
    vec4 c = textureLod(uCube, d, 0.0);
    fragColour = vec4(c.rgb * c.rgb, c.a);
}
)";

/// AveragedDownSample (0x82CFD7C8, blob 0x500C): one bilinear fetch of the
/// level above at the texel's centre (setTexLOD to that level), which for
/// a map halving each level is the average of its 2 x 2 texels.
const char* const kDownsampleFragment = R"(#version 450 core
layout(binding = 0) uniform sampler2D uSphere;
uniform vec2 uSize;
uniform float uLevel;
layout(location = 0) out vec4 fragColour;
void main()
{
    fragColour = textureLod(uSphere, gl_FragCoord.xy / uSize, uLevel);
}
)";

/// BlitCubeFaceFromSphericalMap: the face texel's direction axis + (2u - 1)
/// U + (2v - 1) V (the bases at 0x82CFA9A0, the same as OpenGL's cube map
/// addressing), read from the lat-long map at envcube::sphereCoordinates()
/// trilinearly with the level the texel's footprint selects (the fetch
/// names no level; that the hardware then takes it from the gradients is
/// inferred), then written as the square root again.
const char* const kToFaceFragment = R"(#version 450 core
layout(binding = 0) uniform sampler2D uSphere;
uniform vec3 uAxis;
uniform vec3 uU;
uniform vec3 uV;
uniform float uSize;
layout(location = 0) out vec4 fragColour;
float gameAtan2(float y, float x)
{
    float ax = abs(x);
    float ay = abs(y);
    float r = min(ax, ay) / max(max(ax, ay), 1e-30);
    float r2 = r * r;
    float a = ((((0.0208351 * r2 - 0.085133) * r2 + 0.180141) * r2 - 0.3302995) * r2 + 0.999866) * r;
    a = ay > ax ? 1.570796327 - a : a;
    a = x < 0.0 ? 3.141592654 - a : a;
    return y < 0.0 ? -a : a;
}
float gameAcos(float x)
{
    float ax = abs(x);
    float a = ((-0.0187293 * ax + 0.0742610) * ax - 0.2121144) * ax + 1.5707288;
    a *= sqrt(max(1.0 - ax, 0.0));
    return x < 0.0 ? 3.141592654 - a : a;
}
void main()
{
    vec2 uv = gl_FragCoord.xy / uSize;
    vec3 n = normalize(uAxis + (2.0 * uv.x - 1.0) * uU + (2.0 * uv.y - 1.0) * uV);
    vec2 st = vec2(gameAtan2(n.z, n.x) * 0.159154943 + 0.5, gameAcos(clamp(n.y, -1.0, 1.0)) * 0.318309886);
    vec4 c = texture(uSphere, st);
    fragColour = vec4(sqrt(max(c.rgb, vec3(0.0))), c.a);
}
)";

/// The face bases of BlitCubeFaceFromSphericalMap (0x82CFA9A0): the face's
/// axis, then the directions of its u and v, in the cube's coordinates.
struct FaceBasis {
    glm::vec3 axis;
    glm::vec3 u;
    glm::vec3 v;
};
constexpr std::array<FaceBasis, 6> kFaceBases{{
    {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, -1.0F, 0.0F}},
    {{-1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, -1.0F, 0.0F}},
    {{0.0F, 1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
    {{0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},
    {{0.0F, 0.0F, 1.0F}, {1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}},
    {{0.0F, 0.0F, -1.0F}, {-1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}},
}};

} // namespace

EnvironmentCube::EnvironmentCube()
    : m_toSphere("envcube sphere", kFullscreenVertex, kToSphereFragment)
    , m_downsample("envcube downsample", kFullscreenVertex, kDownsampleFragment)
    , m_toFace("envcube face", kFullscreenVertex, kToFaceFragment)
{
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_cube);
    glTextureStorage2D(m_cube, envcube::kLevels, GL_RGBA16F, envcube::kSize, envcube::kSize);
    glTextureParameteri(m_cube, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_cube, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_cube, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_cube, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // A cube of depth too, so that every face can be bound at once.
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &m_depth);
    glTextureStorage2D(m_depth, 1, GL_DEPTH_COMPONENT32F, envcube::kSize, envcube::kSize);

    glCreateTextures(GL_TEXTURE_2D, 1, &m_sphere);
    glTextureStorage2D(m_sphere, kSphereLevels, GL_RGBA16F, kSphereWidth, kSphereHeight);
    // The blit helper's sampler for the lat-long map: bilinear, wrapping
    // round in u and clamped at the poles in v (0x82472B90-0x82472C90).
    glTextureParameteri(m_sphere, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_sphere, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_sphere, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(m_sphere, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glCreateFramebuffers(1, &m_framebuffer);
    glCreateVertexArrays(1, &m_emptyVao);

    // Until its faces are drawn the cube holds black rather than whatever
    // the new storage holds.
    const std::array<float, 4> black{0.0F, 0.0F, 0.0F, 0.0F};
    for (int level = 0; level < envcube::kLevels; ++level) {
        for (int face = 0; face < 6; ++face) {
            glNamedFramebufferTextureLayer(m_framebuffer, GL_COLOR_ATTACHMENT0, m_cube, level, face);
            glClearNamedFramebufferfv(m_framebuffer, GL_COLOR, 0, black.data());
        }
    }
}

EnvironmentCube::~EnvironmentCube()
{
    glDeleteVertexArrays(1, &m_emptyVao);
    glDeleteFramebuffers(1, &m_framebuffer);
    glDeleteTextures(1, &m_sphere);
    glDeleteTextures(1, &m_depth);
    glDeleteTextures(1, &m_cube);
}

FrameParams EnvironmentCube::faceFrame(int face, const glm::vec3& centre, const FrameParams& frame)
{
    FrameParams params = frame;
    params.view = envcube::faceView(face, centre);
    params.projection = envcube::faceProjection();
    params.cameraPosition = centre;
    return params;
}

glm::mat4 EnvironmentCube::faceTransform(int face)
{
    return envcube::faceProjection() * envcube::faceView(face, glm::vec3(0.0F));
}

void EnvironmentCube::update(const glm::vec3& centre, float envMapFrequencyScale, const glm::vec4& clearColour,
    const FrameParams& frame, const DrawFaces* drawFaces, const DrawFace& drawFace)
{
    const envcube::FaceSchedule::Frame faces = m_schedule.next(envMapFrequencyScale);
    m_facesDrawn = faces.count;
    if (faces.count > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glViewport(0, 0, envcube::kSize, envcube::kSize);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        const auto sceneState = [] {
            glEnable(GL_DEPTH_TEST);
            glDepthFunc(GL_GREATER);
            glDepthMask(GL_TRUE);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
            glFrontFace(GL_CCW);
        };
        const auto bindFace = [this](int face) {
            glNamedFramebufferTextureLayer(m_framebuffer, GL_COLOR_ATTACHMENT0, m_cube, 0, face);
            glNamedFramebufferTextureLayer(m_framebuffer, GL_DEPTH_ATTACHMENT, m_depth, 0, face);
        };
        // The game clears the target before the first face and after each
        // face's resolve (0x82431998, 0x8243A120), so every face starts from
        // the same colour and a depth of 0, the far end of its reversed
        // depth.
        const auto clearFace = [&] {
            sceneState();
            glClearNamedFramebufferfv(m_framebuffer, GL_COLOR, 0, &clearColour.x);
            glClearNamedFramebufferfi(m_framebuffer, GL_DEPTH_STENCIL, 0, 0.0F, 0);
        };
        const auto scheduled = std::span(faces.faces.data(), static_cast<std::size_t>(faces.count));
        if (drawFaces != nullptr) {
            std::array<glm::mat4, 6> transforms{};
            for (std::size_t i = 0; i < scheduled.size(); ++i) {
                bindFace(scheduled[i]);
                clearFace();
                transforms[i] = faceTransform(scheduled[i]);
            }
            glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_cube, 0);
            glNamedFramebufferTexture(m_framebuffer, GL_DEPTH_ATTACHMENT, m_depth, 0);
            sceneState();
            FrameParams centred = frame;
            centred.cameraPosition = centre;
            (*drawFaces)(centred, std::span(transforms.data(), scheduled.size()), scheduled);
        }
        for (const int face : scheduled) {
            bindFace(face);
            if (drawFaces == nullptr) {
                clearFace();
            }
            sceneState();
            drawFace(faceFrame(face, centre, frame));
        }
        glNamedFramebufferTexture(m_framebuffer, GL_DEPTH_ATTACHMENT, 0, 0);
    }
    if (m_schedule.mipsDue()) {
        buildMips();
        m_schedule.markMipsBuilt();
        ++m_mipBuilds;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void EnvironmentCube::buildMips()
{
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glBindVertexArray(m_emptyVao);

    // Level 0 of the cube into the lat-long map.
    glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_sphere, 0);
    glViewport(0, 0, kSphereWidth, kSphereHeight);
    m_toSphere.use();
    m_toSphere.set("uSize", glm::vec2(kSphereWidth, kSphereHeight));
    glBindTextureUnit(0, m_cube);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    // Its mips, each from the one above. Only the level read is in the
    // texture's range, so the level drawn into is never also sampled.
    m_downsample.use();
    glBindTextureUnit(0, m_sphere);
    for (int level = 1; level < kSphereLevels; ++level) {
        const int width = kSphereWidth >> level;
        const int height = kSphereHeight >> level;
        glTextureParameteri(m_sphere, GL_TEXTURE_BASE_LEVEL, level - 1);
        glTextureParameteri(m_sphere, GL_TEXTURE_MAX_LEVEL, level - 1);
        glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_sphere, level);
        glViewport(0, 0, width, height);
        m_downsample.set("uSize", glm::vec2(width, height));
        m_downsample.set("uLevel", static_cast<float>(level - 1));
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glTextureParameteri(m_sphere, GL_TEXTURE_BASE_LEVEL, 0);
    glTextureParameteri(m_sphere, GL_TEXTURE_MAX_LEVEL, kSphereLevels - 1);

    // Every level of every face below the top from the map; the top is the
    // rendered face and stays (0x82E8B750 skips level 0 when the map was
    // made from the cube it fills).
    m_toFace.use();
    for (int level = 1; level < envcube::kLevels; ++level) {
        const int size = envcube::kSize >> level;
        glViewport(0, 0, size, size);
        m_toFace.set("uSize", static_cast<float>(size));
        for (int face = 0; face < 6; ++face) {
            const FaceBasis& basis = kFaceBases[static_cast<std::size_t>(face)];
            glNamedFramebufferTextureLayer(m_framebuffer, GL_COLOR_ATTACHMENT0, m_cube, level, face);
            m_toFace.set("uAxis", basis.axis);
            m_toFace.set("uU", basis.u);
            m_toFace.set("uV", basis.v);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    glBindVertexArray(0);
    glUseProgram(0);
    glEnable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

} // namespace fh::render
