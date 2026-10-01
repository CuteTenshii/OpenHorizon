#include "PostProcess.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace openhorizon::render {

namespace {

const char* const kFullscreenVertex = R"(#version 450 core
void main()
{
    // One triangle covering the target, from the vertex index alone.
    vec2 corner = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)";

/// The chain stores sqrt colour in 10 bits with exponent bias 2: [0, 4].
const char* const kCommon = R"(#version 450 core
const float kRange = 4.0;
vec3 stored(vec3 c)
{
    return clamp(c, 0.0, kRange);
}
)";

/// Shared by the hot extract and combine (0x821BD58C, 0x821BD8C4).
const char* const kExposure = R"(
layout(binding = 7) uniform sampler2D uLuminance;
uniform vec4 uKeys;
uniform vec4 uExposures;
uniform float uBias;
float exposureLog2()
{
    float l = texelFetch(uLuminance, ivec2(0), 0).r;
    int i = 1;
    if (!(l >= uKeys.y)) {
        i = 0;
    } else if (l >= uKeys.z) {
        i = 2;
    }
    float t = clamp((l - uKeys[i]) / (uKeys[i + 1] - uKeys[i]), 0.0, 1.0);
    return uBias + uExposures[i] + t * (uExposures[i + 1] - uExposures[i]);
}
)";

const char* const kDownsample = R"(
layout(binding = 0) uniform sampler2D uSource;
uniform vec2 uOutputSize;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec2 uv = gl_FragCoord.xy / uOutputSize;
    vec2 texel = 1.0 / vec2(textureSize(uSource, 0));
    vec3 sum = vec3(0.0);
    float alpha = 0.0;
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            vec4 t = textureLod(uSource, uv + (vec2(x, y) - 1.5) * texel, 0.0);
            vec3 c = stored(t.rgb);
            sum += c * c;
            alpha += t.a;
        }
    }
    fragColour = vec4(stored(sqrt(sum / 16.0)), alpha / 16.0);
}
)";

const char* const kBiased = R"(
layout(binding = 0) uniform sampler2D uSource;
uniform vec2 uOutputSize;
layout(location = 0) out vec4 fragColour;
void main()
{
    float v = gl_FragCoord.y / uOutputSize.y;
    vec4 sum = vec4(0.0);
    for (int i = 0; i < 16; ++i) {
        sum += textureLod(uSource, vec2((float(i) + 0.5) / 16.0, v), 0.0);
    }
    fragColour = vec4(stored(sum.rgb / 16.0), sum.a / 16.0);
}
)";

const char* const kAccumulate = R"(
layout(binding = 0) uniform sampler2D uRows;
layout(binding = 1) uniform sampler2D uPrevious;
uniform vec2 uWeightRange;
uniform float uAmount;
uniform bool uReset;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec3 sum = vec3(0.0);
    float weights = 0.0;
    for (int i = 0; i < 16; ++i) {
        float v = (float(i) + 0.5) / 16.0;
        vec3 c = textureLod(uRows, vec2(0.5, v), 0.0).rgb;
        float d = 0.5 - abs(v - 0.5);
        // With equal edges the microcode's reciprocal is infinite, so the
        // step is 1 above the edge.
        float t = uWeightRange.y != uWeightRange.x
            ? clamp((d - uWeightRange.x) / (uWeightRange.y - uWeightRange.x), 0.0, 1.0)
            : (d > uWeightRange.x ? 1.0 : 0.0);
        float w = t * t * (3.0 - 2.0 * t);
        sum += w * c * c;
        weights += w;
    }
    float luminance = dot(sum / weights, vec3(0.2125, 0.7154, 0.0721));
    float previous = texelFetch(uPrevious, ivec2(0), 0).r;
    fragColour = vec4(uReset ? luminance : previous + (luminance - previous) * uAmount);
}
)";

const char* const kHotExtract = R"(
layout(binding = 0) uniform sampler2D uPrevious;
layout(binding = 1) uniform sampler2D uScene;
// Cutoff, scale, lower gradient, upper gradient.
uniform vec4 uKnee;
uniform float uPersist;
layout(location = 0) out vec4 fragColour;
void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
    vec3 s = texelFetch(uScene, p, 0).rgb;
    vec3 x = s * s * exp2(exposureLog2());
    float l = dot(x, vec3(0.2125, 0.7154, 0.0721));
    float cutoff = uKnee.x;
    float lower = uKnee.z;
    float upper = uKnee.w;
    // Zero light stays zero: Direct3D's multiply gives 0 for 0 times the
    // infinite reciprocal.
    float gain = l > 0.0 ? max(l * upper + (lower - upper) * cutoff, l * lower) / l : 0.0;
    vec3 hot = x * gain - vec3(1.0 / 255.0);
    vec3 previous = texelFetch(uPrevious, p, 0).rgb;
    vec3 v = max(previous * previous * uPersist + hot, 0.0) * uKnee.y;
    fragColour = vec4(stored(sqrt(v)), 1.0);
}
)";

const char* const kBlur = R"(
layout(binding = 0) uniform sampler2D uSource;
uniform vec2 uOutputSize;
// One source texel along the blur, in texture coordinates.
uniform vec2 uStep;
const vec4 kWeights = vec4(0.240952, 0.212639, 0.122003, 0.0448822);
layout(location = 0) out vec4 fragColour;
vec3 squared(vec2 uv)
{
    vec3 c = textureLod(uSource, uv, 0.0).rgb;
    return c * c;
}
void main()
{
    vec2 uv = gl_FragCoord.xy / uOutputSize;
    vec3 sum = kWeights.x * squared(uv);
    for (int i = 1; i <= 3; ++i) {
        sum += kWeights[i] * (squared(uv + uStep * float(i)) + squared(uv - uStep * float(i)));
    }
    fragColour = vec4(stored(sqrt(sum)), 1.0);
}
)";

const char* const kAdd = R"(
layout(binding = 0) uniform sampler2D uA;
layout(binding = 1) uniform sampler2D uB;
uniform vec2 uOutputSize;
uniform float uScaleA;
uniform float uScaleB;
layout(location = 0) out vec4 fragColour;
void main()
{
    vec2 uv = gl_FragCoord.xy / uOutputSize;
    vec3 a = textureLod(uA, uv, 0.0).rgb;
    vec3 b = textureLod(uB, uv, 0.0).rgb;
    fragColour = vec4(stored(sqrt(a * a * uScaleA + b * b * uScaleB)), 1.0);
}
)";

const char* const kCombine = R"(
layout(binding = 0) uniform sampler2D uScene;
layout(binding = 2) uniform sampler2D uBloom;
layout(binding = 3) uniform sampler3D uLut;
uniform vec2 uOutputSize;
// (W, A, B, C) and (D, E, F).
uniform vec4 uFilmic1;
uniform vec3 uFilmic2;
uniform bool uGrading;
uniform mat4 uColourTransform;
uniform vec4 uVignetteTransform;
uniform vec2 uVignettePosition;
uniform float uVignettePower;
uniform vec4 uVignetteColour;
layout(location = 0) out vec4 fragColour;
vec3 filmic(vec3 x)
{
    float a = uFilmic1.y;
    float b = uFilmic1.z;
    float c = uFilmic1.w;
    float d = uFilmic2.x;
    float e = uFilmic2.y;
    float f = uFilmic2.z;
    return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
}
void main()
{
    vec2 uv = gl_FragCoord.xy / uOutputSize;
    vec3 s = stored(texelFetch(uScene, ivec2(gl_FragCoord.xy), 0).rgb);
    vec3 b = textureLod(uBloom, uv, 0.0).rgb;
    vec3 x = (s * s + b * b) * exp2(exposureLog2());
    vec3 mapped = filmic(x) / filmic(vec3(uFilmic1.x));
    // The grading table is sampled over its texel centres: scale 15/16 and
    // offset 1/32 for 16 texels (0x824329E0..0x82432A3C).
    vec3 c = uGrading ? textureLod(uLut, mapped * (15.0 / 16.0) + 1.0 / 32.0, 0.0).rgb : mapped;
    c = sqrt(abs(c));
    // The vignette works in the game's texture space, y down.
    vec2 v = vec2(uv.x, 1.0 - uv.y) - 0.5 + uVignettePosition;
    vec2 d = vec2(dot(v, uVignetteTransform.xy), dot(v, uVignetteTransform.zw));
    float fade = clamp(exp2(0.5 * uVignettePower * log2(dot(d, d))), 0.0, 1.0);
    c = mix(c, clamp(c + uVignetteColour.rgb - 1.0, 0.0, 1.0), fade * uVignetteColour.a);
    vec3 graded = clamp((uColourTransform * vec4(c * c, 1.0)).rgb, 0.0, 1.0);
    fragColour = vec4(sqrt(graded), 1.0);
}
)";

std::string fragment(const char* body, bool exposure = false)
{
    return std::string(kCommon) + (exposure ? kExposure : "") + body;
}

int align32(int value)
{
    return (value + 31) & ~31;
}

void setExposure(Shader& shader, const PostFrame& frame)
{
    shader.set("uKeys", frame.luminanceKeys);
    shader.set("uExposures", frame.exposures);
    shader.set("uBias", frame.exposureBias);
}

} // namespace

PostProcess::PostProcess()
    : m_downsample("post downsample", kFullscreenVertex, fragment(kDownsample))
    , m_biased("post downsample biased", kFullscreenVertex, fragment(kBiased))
    , m_accumulate("post luminance", kFullscreenVertex, fragment(kAccumulate))
    , m_hotExtract("post hot extract", kFullscreenVertex, fragment(kHotExtract, true))
    , m_blur("post blur", kFullscreenVertex, fragment(kBlur))
    , m_add("post add", kFullscreenVertex, fragment(kAdd))
    , m_combine("post combine", kFullscreenVertex, fragment(kCombine, true))
{
    glCreateVertexArrays(1, &m_emptyVao);
    glCreateSamplers(1, &m_linear);
    glSamplerParameteri(m_linear, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(m_linear, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(m_linear, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_linear, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_linear, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glCreateSamplers(1, &m_point);
    glSamplerParameteri(m_point, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(m_point, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glSamplerParameteri(m_point, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glSamplerParameteri(m_point, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glCreateTextures(GL_TEXTURE_3D, 1, &m_lut);
    glTextureStorage3D(m_lut, 1, GL_RGBA8, 16, 16, 16);
    // Render target entry 0, k_32_FLOAT (32 x 32 of one value in the game).
    for (Target& t : m_luminance) {
        create(t, 1, 1, GL_R32F);
    }
    create(m_d2, 64, 32, GL_RGBA16F);
    create(m_d3, 32, 32, GL_RGBA16F);
}

PostProcess::~PostProcess()
{
    for (Target* t : {&m_d1, &m_d2, &m_d3, &m_hot, &m_bloom, &m_luminance[0], &m_luminance[1]}) {
        destroy(*t);
    }
    for (int i = 0; i < kLevels; ++i) {
        destroy(m_blurX[i]);
        destroy(m_blurY[i]);
        destroy(m_composite[i]);
    }
    glDeleteTextures(1, &m_lut);
    glDeleteSamplers(1, &m_linear);
    glDeleteSamplers(1, &m_point);
    glDeleteVertexArrays(1, &m_emptyVao);
}

void PostProcess::create(Target& target, int width, int height, GLenum format)
{
    destroy(target);
    target.width = std::max(width, 1);
    target.height = std::max(height, 1);
    glCreateTextures(GL_TEXTURE_2D, 1, &target.texture);
    glTextureStorage2D(target.texture, 1, format, target.width, target.height);
    glCreateFramebuffers(1, &target.framebuffer);
    glNamedFramebufferTexture(target.framebuffer, GL_COLOR_ATTACHMENT0, target.texture, 0);
    if (glCheckNamedFramebufferStatus(target.framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("a post-processing target is incomplete");
    }
}

void PostProcess::destroy(Target& target)
{
    if (target.texture != 0) {
        glDeleteFramebuffers(1, &target.framebuffer);
        glDeleteTextures(1, &target.texture);
    }
    target = Target{};
}

void PostProcess::resize(int width, int height)
{
    if (width == m_width && height == m_height) {
        return;
    }
    m_width = width;
    m_height = height;
    // Relative sizes round up to multiples of 32 (0x825F21D4); passes draw
    // over the whole target.
    const int w = align32((width + 3) / 4);
    const int h = align32((height + 3) / 4);
    create(m_d1, w, h, GL_RGBA16F);
    create(m_hot, w, h, GL_RGBA16F);
    create(m_bloom, w, h, GL_RGBA16F);
    for (int i = 0; i < kLevels; ++i) {
        create(m_blurX[i], w >> (i + 1), h >> i, GL_RGBA16F);
        create(m_blurY[i], w >> (i + 1), h >> (i + 1), GL_RGBA16F);
        create(m_composite[i], w >> (i + 1), h >> (i + 1), GL_RGBA16F);
    }
    m_bloomValid = false;
}

void PostProcess::bindTarget(const Target& target) const
{
    glBindFramebuffer(GL_FRAMEBUFFER, target.framebuffer);
    glViewport(0, 0, target.width, target.height);
}

void PostProcess::draw() const
{
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

void PostProcess::run(GLuint scene, int width, int height, GLuint output, const PostFrame& frame)
{
    resize(width, height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glBindVertexArray(m_emptyVao);

    const auto size
        = [](const Target& t) { return glm::vec2(static_cast<float>(t.width), static_cast<float>(t.height)); };

    m_downsample.use();
    glBindSampler(0, m_point);
    bindTarget(m_d1);
    m_downsample.set("uOutputSize", size(m_d1));
    glBindTextureUnit(0, scene);
    draw();
    bindTarget(m_d2);
    m_downsample.set("uOutputSize", size(m_d2));
    glBindTextureUnit(0, m_d1.texture);
    draw();

    glBindSampler(0, m_linear);
    glBindSampler(1, m_linear);
    glBindSampler(2, m_linear);
    glBindSampler(3, m_linear);
    glBindSampler(7, m_point);
    m_biased.use();
    bindTarget(m_d3);
    m_biased.set("uOutputSize", size(m_d3));
    glBindTextureUnit(0, m_d2.texture);
    draw();

    const Target& previousLuminance = m_luminance[static_cast<std::size_t>(m_luminanceCurrent)];
    m_luminanceCurrent ^= 1;
    const Target& luminance = m_luminance[static_cast<std::size_t>(m_luminanceCurrent)];
    m_accumulate.use();
    bindTarget(luminance);
    m_accumulate.set("uWeightRange", glm::vec2(0.0F));
    m_accumulate.set("uAmount", frame.adaptation);
    m_accumulate.set("uReset", (frame.resetAdaptation || !m_luminanceValid) ? 1 : 0);
    glBindTextureUnit(0, m_d3.texture);
    glBindTextureUnit(1, previousLuminance.texture);
    draw();
    m_luminanceValid = true;
    glBindTextureUnit(7, luminance.texture);

    if (!m_bloomValid) {
        const float black[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        glClearNamedFramebufferfv(m_bloom.framebuffer, GL_COLOR, 0, black);
        m_bloomValid = true;
    }
    m_hotExtract.use();
    setExposure(m_hotExtract, frame);
    m_hotExtract.set(
        "uKnee", glm::vec4(frame.bloomCutoff, frame.bloomScale, frame.bloomLowerGradient, frame.bloomUpperGradient));
    // The persistence reaches the shader times 60 (0x82435800).
    m_hotExtract.set("uPersist", frame.bloomPersist * 60.0F);
    bindTarget(m_hot);
    glBindTextureUnit(0, m_bloom.texture);
    glBindTextureUnit(1, m_d1.texture);
    draw();

    m_blur.use();
    for (int i = 0; i < kLevels; ++i) {
        const Target& source = i == 0 ? m_hot : m_blurY[static_cast<std::size_t>(i - 1)];
        const Target& x = m_blurX[static_cast<std::size_t>(i)];
        const Target& y = m_blurY[static_cast<std::size_t>(i)];
        bindTarget(x);
        m_blur.set("uOutputSize", size(x));
        m_blur.set("uStep", glm::vec2(1.0F / static_cast<float>(source.width), 0.0F));
        glBindTextureUnit(0, source.texture);
        draw();
        bindTarget(y);
        m_blur.set("uOutputSize", size(y));
        m_blur.set("uStep", glm::vec2(0.0F, 1.0F / static_cast<float>(x.height)));
        glBindTextureUnit(0, x.texture);
        draw();
    }

    m_add.use();
    const auto add = [&](const Target& into, GLuint a, float scaleA, GLuint b, float scaleB) {
        bindTarget(into);
        m_add.set("uOutputSize", size(into));
        m_add.set("uScaleA", scaleA);
        m_add.set("uScaleB", scaleB);
        glBindTextureUnit(0, a);
        glBindTextureUnit(1, b);
        draw();
    };
    const std::array<float, 5>& w = frame.bloomWeights;
    add(m_composite[2], m_blurY[2].texture, 0.2F * w[3], m_blurY[3].texture, 0.2F * w[4]);
    add(m_composite[1], m_blurY[1].texture, 0.2F * w[2], m_composite[2].texture, 1.0F);
    add(m_composite[0], m_blurY[0].texture, 0.2F * w[1], m_composite[1].texture, 1.0F);
    add(m_bloom, m_hot.texture, 0.2F * w[0], m_composite[0].texture, 1.0F);

    if (!frame.gradingLut.empty()) {
        glTextureSubImage3D(m_lut, 0, 0, 0, 0, 16, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, frame.gradingLut.data());
    }
    glBindFramebuffer(GL_FRAMEBUFFER, output);
    glViewport(0, 0, width, height);
    m_combine.use();
    setExposure(m_combine, frame);
    m_combine.set("uOutputSize", glm::vec2(static_cast<float>(width), static_cast<float>(height)));
    m_combine.set("uFilmic1", glm::vec4(frame.filmic.white, frame.filmic.a, frame.filmic.b, frame.filmic.c));
    m_combine.set("uFilmic2", glm::vec3(frame.filmic.d, frame.filmic.e, frame.filmic.f));
    m_combine.set("uGrading", frame.gradingLut.empty() ? 0 : 1);
    m_combine.set("uColourTransform", frame.colourTransform);
    glBindSampler(0, m_point);
    glBindTextureUnit(0, scene);
    glBindTextureUnit(2, m_bloom.texture);
    glBindTextureUnit(3, m_lut);
    glEnable(GL_SCISSOR_TEST);
    const int half = height / 2;
    // The top of the picture is the upper rows in OpenGL's window space.
    const std::array<std::pair<const PostVignette*, std::array<int, 2>>, 2> halves{{
        {&frame.vignetteTop, {height - half, half}},
        {&frame.vignetteBottom, {0, height - half}},
    }};
    for (const auto& [vignette, rows] : halves) {
        m_combine.set("uVignetteTransform", vignette->transform);
        m_combine.set("uVignettePosition", vignette->position);
        m_combine.set("uVignettePower", vignette->power);
        m_combine.set("uVignetteColour", vignette->colour);
        glScissor(0, rows[0], width, rows[1]);
        draw();
    }
    glDisable(GL_SCISSOR_TEST);
    for (GLuint unit : {0U, 1U, 2U, 3U, 7U}) {
        glBindSampler(unit, 0);
    }
    glBindVertexArray(0);
}

} // namespace openhorizon::render
