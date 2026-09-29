#include "SkyObjects.h"

#include "WorldRenderer.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"
#include "world/XboxTexture.h"

#include <glm/glm.hpp>

#include <optional>
#include <vector>

namespace fh::render {

namespace {

/// SunSize.x: how far from the camera the sun or moon quad sits
/// (0x8224D2B8).
constexpr float kSunDistance = 300.0F;
/// Star_PS's StarPowers.xyz, the sky parameter block's defaults.
constexpr float kStarLow = 0.25F;
constexpr float kStarHigh = 0.97F;
constexpr float kStarPower = 7.0F;
constexpr std::size_t kStarBytes = 16;

// Sun_VS: the corner arrives as POSITION0.xy (a float3 stream); gl_VertexID
// picks it from the object's four vertices in the order the vertex buffer
// holds them, drawn as a fan.
const char* const kSunVertex = R"(#version 450 core
uniform mat4 uViewProjection;
uniform vec3 uSunDirection;
uniform float uSunDistance;
uniform float uSunSize;
out vec2 vUv;
const vec2 kCorners[4] = vec2[4](vec2(-1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, 1.0), vec2(1.0, -1.0));
void main()
{
    vec2 corner = kCorners[gl_VertexID];
    vec3 d = uSunDirection;
    float horizontal = d.x * d.x + d.z * d.z;
    // The microcode takes rsq of the horizontal length; straight up or down
    // that is infinite and, by the Xenos rule that zero times anything is
    // zero, both axes vanish.
    vec3 across = vec3(0.0);
    vec3 up = vec3(0.0);
    if (horizontal > 0.0) {
        float l = inversesqrt(horizontal);
        float n = inversesqrt(dot(d, d));
        across = vec3(-d.z * l, 0.0, d.x * l);
        up = vec3(d.x * d.y * l * n, -horizontal * l * n, d.z * d.y * l * n);
    }
    vec3 position = -d * uSunDistance + uSunSize * (corner.x * across + corner.y * up);
    vec4 clip = uViewProjection * vec4(position, 1.0);
    // At the far plane (depth 0 with the reversed depth range).
    gl_Position = vec4(clip.xy, 0.0, clip.w);
    vUv = corner * 0.5 + 0.5;
}
)";

const char* const kSunFragment = R"(#version 450 core
layout(binding = 0) uniform sampler2D uSunMap;
uniform vec4 uSunColour;
in vec2 vUv;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec4 texel = texture(uSunMap, vUv);
    fragColour = vec4(texel.rgb * uSunColour.rgb, texel.a * uSunColour.a);
}
)";

const char* const kStarVertex = R"(#version 450 core
layout(location = 0) in vec4 aStar;
uniform mat4 uViewProjection;
out float vBrightness;
void main()
{
    vec4 clip = uViewProjection * vec4(aStar.xyz, 1.0);
    gl_Position = vec4(clip.xy, 0.0, clip.w);
    vBrightness = aStar.w;
}
)";

const char* const kStarFragment = R"(#version 450 core
uniform vec4 uStarPowers;
in float vBrightness;
layout(location = 0) out vec4 fragColour;
void main()
{
    float t = clamp((vBrightness - uStarPowers.x) / (uStarPowers.y - uStarPowers.x), 0.0, 1.0);
    // exp(power * log(t)), which is 0 at t = 0.
    float glow = t > 0.0 ? exp2(uStarPowers.z * log2(t)) : 0.0;
    fragColour = vec4(1.0, 1.0, 1.0, uStarPowers.w * glow);
}
)";

Bytes readFile(const Vfs& vfs, const char* path)
{
    std::optional<Bytes> data = vfs.read(path);
    if (!data) {
        throw DataError("{} is missing", path);
    }
    return std::move(*data);
}

/// Sun.xds and Moon0.xds carry no gamma flag in their fetch constants, so
/// their texels are linear.
Texture loadTexture(const Vfs& vfs, const char* path)
{
    const world::RgbaImage image = world::decodeXboxTexture(readFile(vfs, path));
    return Texture::fromRgba(image.width, image.height, image.rgba, false, true);
}

std::vector<float> readStars(const Bytes& data)
{
    if (data.empty() || data.size() % kStarBytes != 0) {
        throw DataError("stars.bin is {} bytes, not a whole number of stars", data.size());
    }
    std::vector<float> values(data.size() / sizeof(float));
    for (std::size_t i = 0; i < values.size(); ++i) {
        values[i] = loadBeF32(data.data() + i * sizeof(float));
    }
    return values;
}

} // namespace

SkyObjects::SkyObjects(const Vfs& vfs)
    : m_sunShader("sun", kSunVertex, kSunFragment)
    , m_starShader("stars", kStarVertex, kStarFragment)
    , m_sun(loadTexture(vfs, "media/realtimesky/sun.xds"))
    , m_moon(loadTexture(vfs, "media/realtimesky/moon0.xds"))
{
    const std::vector<float> stars = readStars(readFile(vfs, "media/realtimesky/stars.bin"));
    m_starCount = stars.size() / 4;

    // Sampler states 0x82477220-0x824772A8: clamped U and V, linear
    // minification, magnification and mip filters.
    glCreateSamplers(1, &m_sampler);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glSamplerParameteri(m_sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glCreateVertexArrays(1, &m_emptyVao);
    glCreateBuffers(1, &m_starBuffer);
    glNamedBufferStorage(m_starBuffer, static_cast<GLsizeiptr>(stars.size() * sizeof(float)), stars.data(), 0);
    glCreateVertexArrays(1, &m_starVao);
    glVertexArrayVertexBuffer(m_starVao, 0, m_starBuffer, 0, static_cast<GLsizei>(kStarBytes));
    glEnableVertexArrayAttrib(m_starVao, 0);
    glVertexArrayAttribFormat(m_starVao, 0, 4, GL_FLOAT, GL_FALSE, 0);
    glVertexArrayAttribBinding(m_starVao, 0, 0);
}

SkyObjects::~SkyObjects()
{
    glDeleteSamplers(1, &m_sampler);
    glDeleteVertexArrays(1, &m_emptyVao);
    glDeleteVertexArrays(1, &m_starVao);
    glDeleteBuffers(1, &m_starBuffer);
}

void SkyObjects::draw(const FrameParams& frame)
{
    const SceneLighting& light = frame.lighting;
    // Around the camera: the view's rotation only.
    const glm::mat4 viewProjection = frame.projection * glm::mat4(glm::mat3(frame.view));

    glDepthFunc(GL_EQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);

    m_starShader.use();
    m_starShader.set("uViewProjection", viewProjection);
    m_starShader.set("uStarPowers", glm::vec4(kStarLow, kStarHigh, kStarPower, light.starFade));
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glBindVertexArray(m_starVao);
    glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(m_starCount));

    const bool moon = light.sunObjectMoon > 0.5F;
    const glm::vec3 colour = glm::clamp(light.sunObjectColour, glm::vec3(0.0F), glm::vec3(1.0F));
    m_sunShader.use();
    m_sunShader.set("uViewProjection", viewProjection);
    m_sunShader.set("uSunDirection", light.sunObjectDirection);
    m_sunShader.set("uSunDistance", kSunDistance);
    m_sunShader.set("uSunSize", light.sunObjectSize);
    if (moon) {
        m_sunShader.set("uSunColour", glm::vec4(colour, light.sunObjectAlpha));
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        m_sunShader.set("uSunColour", glm::vec4(colour * light.sunObjectAlpha, 1.0F));
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
    }
    (moon ? m_moon : m_sun).bind(0);
    glBindSampler(0, m_sampler);
    glBindVertexArray(m_emptyVao);
    glDrawArrays(GL_TRIANGLE_FAN, 0, 4);

    glBindSampler(0, 0);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glEnable(GL_CULL_FACE);
}

} // namespace fh::render
