#include "Clouds.h"

#include "WorldRenderer.h"

#include "base/Error.h"
#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <optional>
#include <string>

namespace openhorizon::render {

namespace {

/// The far object's texture coordinate scales and cover range, set by its
/// constructor (0x82E37EE8) and read by the draw (0x82435FE0).
constexpr float kFarMaskScale1 = 1.01F;
constexpr float kFarMaskScale2 = 3.2F;
constexpr float kFarMapScale = 5.0F;
constexpr float kFarCoverAtNoCoverage = 0.616F;
constexpr float kFarCoverAtFullCoverage = 0.43F;
constexpr float kFarSharpness = 0.984F;

/// Direct3D 9 arithmetic the microcode relies on: zero times infinity is
/// zero, and a zero power of a zero base is 1.
const char* const kXenosMath = R"(
float xmul(float a, float b)
{
    return min(abs(a), abs(b)) < 1.17549435e-38 ? 0.0 : a * b;
}
float xlog(float a)
{
    return a == 1.0 ? 0.0 : log2(a);
}
float xexp(float a)
{
    return a == 0.0 ? 1.0 : exp2(a);
}
float xpow(float base, float power)
{
    return xexp(xmul(power, xlog(abs(base))));
}
)";

// FarCloud_VS; interpolators 0-3 as the microcode exports them.
const char* const kFarVertex = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aTexcoord;
uniform mat4 uViewProjection;
uniform float uMinSkyHeight;
uniform vec3 uFrontMask1;
uniform vec3 uFrontMask2;
uniform vec3 uFrontMap;
uniform vec3 uBackMask1;
uniform vec3 uBackMask2;
uniform vec3 uBackMap;
out vec4 vFrontMask;
out vec3 vFrontMap;
out vec4 vBackMask;
out vec3 vBackMap;
void main()
{
    vec3 position = vec3(aPosition.x, max(aPosition.y, uMinSkyHeight), aPosition.z);
    vec4 clip = uViewProjection * vec4(position, 1.0);
    // At the far plane (depth 0 with the reversed depth range).
    gl_Position = vec4(clip.xy, 0.0, clip.w);
    vec2 uv = aTexcoord.xy;
    vFrontMask = vec4(uv * uFrontMask1.z + uFrontMask1.xy, uv * uFrontMask2.z + uFrontMask2.xy);
    vFrontMap = vec3(uv * uFrontMap.z + uFrontMap.xy, aTexcoord.z);
    vBackMask = vec4(uv * uBackMask1.z + uBackMask1.xy, uv * uBackMask2.z + uBackMask2.xy);
    vBackMap = vec3(uv * uBackMap.z + uBackMap.xy, aTexcoord.z);
}
)";

// FarCloud_PS, following the microcode's order of operations.
const char* const kFarFragmentBody = R"(
layout(binding = 0) uniform sampler2D uMask;
layout(binding = 1) uniform sampler2D uMap;
uniform vec2 uFrontCover;
uniform vec3 uFrontDense;
uniform vec3 uFrontThin;
uniform vec3 uFrontAlpha;
uniform vec2 uBackCover;
uniform vec3 uBackDense;
uniform vec3 uBackThin;
uniform vec3 uBackAlpha;
in vec4 vFrontMask;
in vec3 vFrontMap;
in vec4 vBackMask;
in vec3 vBackMap;
layout(location = 0) out vec4 fragColour;
float density(vec4 maskUv, vec2 mapUv, vec2 cover)
{
    float mask = xmul(texture(uMask, maskUv.xy).x + texture(uMask, maskUv.zw).x, 0.5);
    float covered = clamp(mask - cover.x, 0.0, 1.0);
    float thin = xexp(xmul(xmul(xlog(abs(cover.y)), 255.0), covered));
    return xmul(1.0 - thin, texture(uMap, mapUv).x);
}
float layerAlpha(float d, float weight, vec3 alpha)
{
    return xmul(d, alpha.x + xmul(alpha.y - alpha.x, xpow(weight, alpha.z)));
}
void main()
{
    float front = density(vFrontMask, vFrontMap.xy, uFrontCover);
    float back = density(vBackMask, vBackMap.xy, uBackCover);
    float frontAlpha = layerAlpha(front, vFrontMap.z, uFrontAlpha);
    float backAlpha = layerAlpha(back, vBackMap.z, uBackAlpha);
    vec3 frontColour = uFrontThin + vec3(xmul(front, uFrontDense.x - uFrontThin.x),
        xmul(front, uFrontDense.y - uFrontThin.y), xmul(front, uFrontDense.z - uFrontThin.z));
    vec3 backColour = uBackThin + vec3(xmul(back, uBackDense.x - uBackThin.x),
        xmul(back, uBackDense.y - uBackThin.y), xmul(back, uBackDense.z - uBackThin.z));
    float frontGap = 1.0 - frontAlpha;
    float backShown = xmul(backAlpha, frontGap);
    vec3 behind = vec3(xmul(xmul(backAlpha, backColour.x), frontGap), xmul(xmul(backAlpha, backColour.y), frontGap),
        xmul(xmul(backAlpha, backColour.z), frontGap));
    float alpha = backShown + frontAlpha;
    vec3 colour = vec3(xmul(frontAlpha, frontColour.x), xmul(frontAlpha, frontColour.y),
        xmul(frontAlpha, frontColour.z)) + behind;
    float inverse = alpha > 0.0 ? 1.0 / alpha : 0.0;
    // Both cloud pixel shaders end in `sqrt oC0`: the scene target holds
    // square-root colour (see FrameRenderer).
    fragColour = vec4(sqrt(abs(vec3(xmul(colour.x, inverse), xmul(colour.y, inverse), xmul(colour.z, inverse)))),
        alpha);
}
)";

// CloseCloud_VS: POSITION0.xyzw (w the ring height) and NORMAL0.xyzw (w the
// texture's u).
const char* const kCloseVertex = R"(#version 450 core
layout(location = 0) in vec4 aPosition;
layout(location = 1) in vec4 aNormal;
uniform mat4 uViewProjection;
uniform float uMinSkyHeight;
uniform vec3 uSunDirection;
uniform vec2 uMovement;
uniform vec2 uUvScales;
out vec4 vUv;
out vec2 vLight;
void main()
{
    vec3 position = vec3(aPosition.x, max(aPosition.y, uMinSkyHeight), aPosition.z);
    vec4 clip = uViewProjection * vec4(position, 1.0);
    gl_Position = vec4(clip.xy, 0.0, clip.w);
    float u = 1.0 - aNormal.w;
    float below = 1.0 - aPosition.w;
    float v = below * uUvScales.y - (uUvScales.y - 1.0);
    vUv = vec4(u * uUvScales.x + uMovement.x, v, u * uUvScales.x + uMovement.y, v);
    float sun = min(max(dot(aNormal.xyz, uSunDirection), -1.0), 1.0);
    vLight = vec2(sun * 0.5 + 0.5, below);
}
)";

// CloseCloud_Front_Only_PS, following the microcode's order of operations.
const char* const kCloseFragmentBody = R"(
layout(binding = 0) uniform sampler2D uAlphaDensity;
layout(binding = 1) uniform sampler2D uTopBottomAmbient;
layout(binding = 2) uniform sampler2D uAmbient;
uniform vec3 uGradientFade;
uniform vec4 uLayerFadeMaskPowers;
uniform vec3 uDenseAmbient;
uniform vec3 uThinAmbient;
uniform vec4 uBackLight;
uniform vec4 uTopLight;
uniform vec4 uBottomLight;
in vec4 vUv;
in vec2 vLight;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec2 alphaDensity = texture(uAlphaDensity, vUv.xy).xy;
    vec2 topBottom = texture(uTopBottomAmbient, vUv.xy).xy;
    float ambient = texture(uAmbient, vUv.xy).y;
    float fade = uLayerFadeMaskPowers.x;
    float sunLog = xlog(abs(vLight.x));
    float gradient = uGradientFade.x
        + xmul(uGradientFade.y - uGradientFade.x, xexp(xmul(xlog(abs(vLight.y)), uGradientFade.z)));
    float back = xexp(xmul(sunLog, uBackLight.w));
    float top = xexp(xmul(uTopLight.w, sunLog));
    float bottom = xexp(xmul(sunLog, uBottomLight.w));
    float topMask = xpow(xmul(topBottom.x, fade), uLayerFadeMaskPowers.z);
    float bottomMask = xpow(xmul(topBottom.y, fade), uLayerFadeMaskPowers.w);
    float lit = 1.0 - xmul(alphaDensity.y, fade);
    float mixAmount = xmul(ambient, fade);
    vec3 colour = uDenseAmbient
        + vec3(xmul(mixAmount, uThinAmbient.x - uDenseAmbient.x), xmul(mixAmount, uThinAmbient.y - uDenseAmbient.y),
            xmul(mixAmount, uThinAmbient.z - uDenseAmbient.z));
    colour += vec3(xmul(xmul(back, uBackLight.x), lit), xmul(xmul(back, uBackLight.y), lit),
        xmul(xmul(back, uBackLight.z), lit));
    colour += vec3(xmul(xmul(top, uTopLight.x), topMask), xmul(xmul(top, uTopLight.y), topMask),
        xmul(xmul(top, uTopLight.z), topMask));
    colour += vec3(xmul(xmul(bottom, uBottomLight.x), bottomMask), xmul(xmul(bottom, uBottomLight.y), bottomMask),
        xmul(xmul(bottom, uBottomLight.z), bottomMask));
    fragColour = vec4(sqrt(abs(colour)), xmul(xmul(alphaDensity.x, fade), gradient));
}
)";

std::string fragmentSource(const char* body)
{
    return std::string("#version 450 core\n") + kXenosMath + body;
}

/// Linear: no gamma sign in the sky textures' fetch constants.
Texture loadTexture(const Vfs& vfs, const std::string& path)
{
    const std::optional<Bytes> data = vfs.read(path);
    if (!data) {
        throw DataError("{} is missing", path);
    }
    const world::RgbaImage image = world::decodeXboxTexture(*data);
    return Texture::fromRgba(image.width, image.height, image.rgba, false, true);
}

GLuint makeSampler(GLenum wrapS, GLenum wrapT)
{
    GLuint sampler = 0;
    glCreateSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrapS));
    glSamplerParameteri(sampler, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrapT));
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    return sampler;
}

int commonIndex(const CloudDefinitions::Set& set)
{
    const int index = set.textures[0];
    for (const int other : set.textures) {
        if (other != index) {
            throw DataError("cloud set {} gives its layers different textures, which is not supported", set.name);
        }
    }
    if (index < 0) {
        throw DataError("cloud set {} is not valid", set.name);
    }
    return index;
}

template <typename Vertex>
void uploadDome(const CloudDome<Vertex>& dome, GLuint& vao, GLuint& vertices, GLuint& indices, GLsizei& count)
{
    glCreateBuffers(1, &vertices);
    glNamedBufferStorage(
        vertices, static_cast<GLsizeiptr>(dome.vertices.size() * sizeof(Vertex)), dome.vertices.data(), 0);
    glCreateBuffers(1, &indices);
    glNamedBufferStorage(
        indices, static_cast<GLsizeiptr>(dome.indices.size() * sizeof(std::uint32_t)), dome.indices.data(), 0);
    count = static_cast<GLsizei>(dome.indices.size());
    glCreateVertexArrays(1, &vao);
    glVertexArrayVertexBuffer(vao, 0, vertices, 0, static_cast<GLsizei>(sizeof(Vertex)));
    glVertexArrayElementBuffer(vao, indices);
}

void attribute(GLuint vao, GLuint index, GLint components, GLuint offset)
{
    glEnableVertexArrayAttrib(vao, index);
    glVertexArrayAttribFormat(vao, index, components, GL_FLOAT, GL_FALSE, offset);
    glVertexArrayAttribBinding(vao, index, 0);
}

/// The far object's per-layer coordinate block: offset and scale.
glm::vec3 coordinates(const glm::vec2& offset, float scale)
{
    return glm::vec3(offset, scale);
}

float cover(float coverage)
{
    return kFarCoverAtNoCoverage + (kFarCoverAtFullCoverage - kFarCoverAtNoCoverage) * coverage;
}

} // namespace

Clouds::Clouds(const Vfs& vfs, const CloudDefinitions::Set& set)
    : m_farShader("far clouds", kFarVertex, fragmentSource(kFarFragmentBody))
    , m_closeShader("close clouds", kCloseVertex, fragmentSource(kCloseFragmentBody))
{
    const int index = commonIndex(set);
    m_farMask = loadTexture(vfs, farCloudMaskPath());
    m_farMap = loadTexture(vfs, farCloudMapPath("Front", index));
    m_closeAlphaDensity = loadTexture(vfs, closeCloudAlphaDensityPath("Front", index));
    m_closeTopBottomAmbient = loadTexture(vfs, closeCloudTopBottomAmbientPath("Front", index));
    m_closeAmbient = loadTexture(vfs, closeCloudAmbientPath("Front", index));

    m_farSampler = makeSampler(GL_REPEAT, GL_REPEAT);
    m_closeSampler = makeSampler(GL_REPEAT, GL_CLAMP_TO_EDGE);

    uploadDome(buildFarCloudDome(), m_farVao, m_farVertices, m_farIndices, m_farIndexCount);
    attribute(m_farVao, 0, 3, static_cast<GLuint>(offsetof(FarCloudVertex, position)));
    attribute(m_farVao, 1, 3, static_cast<GLuint>(offsetof(FarCloudVertex, texcoord)));
    uploadDome(buildCloseCloudDome(), m_closeVao, m_closeVertices, m_closeIndices, m_closeIndexCount);
    attribute(m_closeVao, 0, 4, static_cast<GLuint>(offsetof(CloseCloudVertex, position)));
    attribute(m_closeVao, 1, 4, static_cast<GLuint>(offsetof(CloseCloudVertex, normal)));
}

Clouds::~Clouds()
{
    glDeleteSamplers(1, &m_farSampler);
    glDeleteSamplers(1, &m_closeSampler);
    glDeleteVertexArrays(1, &m_farVao);
    glDeleteBuffers(1, &m_farVertices);
    glDeleteBuffers(1, &m_farIndices);
    glDeleteVertexArrays(1, &m_closeVao);
    glDeleteBuffers(1, &m_closeVertices);
    glDeleteBuffers(1, &m_closeIndices);
}

void Clouds::draw(const FrameParams& frame, const CloudParameters& parameters, const CloudMotion& motion)
{
    const SceneLighting& light = frame.lighting;
    // Around the camera: the view's rotation only.
    const glm::mat4 viewProjection = frame.projection * glm::mat4(glm::mat3(frame.view));
    const float minHeight = cloudMinSkyHeight(light.fogSkyHeightFadeStart);

    glDepthFunc(GL_EQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    const FarCloudLayerParameters& front = parameters.farFront;
    const FarCloudLayerParameters& back = parameters.farBack;
    m_farShader.use();
    m_farShader.set("uViewProjection", viewProjection);
    m_farShader.set("uMinSkyHeight", minHeight);
    m_farShader.set("uFrontMask1", coordinates(motion.farFront.mask1, kFarMaskScale1));
    m_farShader.set("uFrontMask2", coordinates(motion.farFront.mask2, kFarMaskScale2));
    m_farShader.set("uFrontMap", coordinates(motion.farFront.map, kFarMapScale));
    m_farShader.set("uBackMask1", coordinates(motion.farBack.mask1, kFarMaskScale1));
    m_farShader.set("uBackMask2", coordinates(motion.farBack.mask2, kFarMaskScale2));
    m_farShader.set("uBackMap", coordinates(motion.farBack.map, kFarMapScale));
    m_farShader.set("uFrontCover", glm::vec2(cover(front.coverage), kFarSharpness));
    m_farShader.set("uFrontDense", front.denseColour * front.denseIntensity);
    m_farShader.set("uFrontThin", front.thinColour * front.thinIntensity);
    m_farShader.set("uFrontAlpha", glm::vec3(front.centreAlpha, front.edgeAlpha, front.alphaPower));
    m_farShader.set("uBackCover", glm::vec2(cover(back.coverage), kFarSharpness));
    m_farShader.set("uBackDense", back.denseColour * back.denseIntensity);
    m_farShader.set("uBackThin", back.thinColour * back.thinIntensity);
    m_farShader.set("uBackAlpha", glm::vec3(back.centreAlpha, back.edgeAlpha, back.alphaPower));
    m_farMask.bind(0);
    m_farMap.bind(1);
    glBindSampler(0, m_farSampler);
    glBindSampler(1, m_farSampler);
    glBindVertexArray(m_farVao);
    glDrawElements(GL_TRIANGLES, m_farIndexCount, GL_UNSIGNED_INT, nullptr);

    const CloseCloudParameters& close = parameters.close;
    m_closeShader.use();
    m_closeShader.set("uViewProjection", viewProjection);
    m_closeShader.set("uMinSkyHeight", minHeight);
    m_closeShader.set("uSunDirection", glm::normalize(light.sunObjectDirection));
    m_closeShader.set("uMovement", glm::vec2(motion.closeFront, motion.closeBack));
    m_closeShader.set("uUvScales", close.uvScales);
    m_closeShader.set("uGradientFade", glm::vec3(close.topAlpha, close.bottomAlpha, close.alphaCurve));
    m_closeShader.set("uLayerFadeMaskPowers",
        glm::vec4(close.frontAlpha, close.backAlpha, close.topMaskPower, close.bottomMaskPower));
    m_closeShader.set("uDenseAmbient", close.denseColour * close.denseColourIntensity);
    m_closeShader.set("uThinAmbient", close.thinColour * close.thinColourIntensity);
    m_closeShader.set("uBackLight", glm::vec4(close.backColour * close.backIntensity, close.backPower));
    m_closeShader.set("uTopLight", glm::vec4(close.topColour * close.topIntensity, close.topPower));
    m_closeShader.set("uBottomLight", glm::vec4(close.bottomColour * close.bottomIntensity, close.bottomPower));
    m_closeAlphaDensity.bind(0);
    m_closeTopBottomAmbient.bind(1);
    m_closeAmbient.bind(2);
    glBindSampler(0, m_closeSampler);
    glBindSampler(1, m_closeSampler);
    glBindSampler(2, m_closeSampler);
    glBindVertexArray(m_closeVao);
    glDrawElements(GL_TRIANGLES, m_closeIndexCount, GL_UNSIGNED_INT, nullptr);

    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindSampler(2, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

} // namespace openhorizon::render
