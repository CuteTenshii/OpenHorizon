#pragma once

#include "Gl.h"
#include "PostMath.h"
#include "Shader.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace openhorizon::render {

/// One half of the screen's vignette as the combine takes it.
struct PostVignette {
    /// See post::vignetteTransform().
    glm::vec4 transform{0.0F};
    /// In texture space: x right, y down, the picture spanning 1.
    glm::vec2 position{0.0F};
    float power = 1.0F;
    /// a is the strength.
    glm::vec4 colour{0.0F};
};

/// Everything the post-processing of one frame takes.
struct PostFrame {
    float bloomScale = 0.0F;
    float bloomCutoff = 0.0F;
    float bloomPersist = 0.0F;
    float bloomLowerGradient = 0.0F;
    float bloomUpperGradient = 0.0F;
    std::array<float, 5> bloomWeights{};

    /// Keys already divided by 255; exposures in log2.
    glm::vec4 luminanceKeys{0.0F};
    glm::vec4 exposures{0.0F};
    /// filmicParams2.w: the filmic exposure plus the exposure bias.
    float exposureBias = 0.0F;
    post::Filmic filmic;
    /// tonemapDelay and tonemapReset.
    float adaptation = 0.0F;
    bool resetAdaptation = false;

    glm::mat4 colourTransform{1.0F};
    /// 16x16x16 RGBA8, red along x, blue along slices; empty for none.
    std::span<const std::uint8_t> gradingLut;

    PostVignette vignetteTop;
    PostVignette vignetteBottom;
};

/// The game's post-processing (luminance and bloom chain 0x82494D58, combine
/// 0x82432628, post effect at 0x821B9918). Targets are the game's 10-bit
/// sqrt colour (0x1A2001B6) with exponent bias 2 (0x8330C830), so passes clamp to [0, 4].
///
/// Passes: DownSample16XGammaCorrect (0x821BEBE0) twice, DownSampleBiased
/// (0x821BF1BC), MultiSampleAndAccumulate (0x821BF658; range (0, 0) outside
/// the cockpit, 0x825E6E50), Add_HotExtract (0x821C83FC, set up at 0x82435800), four
/// GaussianBlurAccumX / Y levels (0x821CF760, 0x821CF984, weights 0x82001210),
/// Add2Textures_UVScale (0x821C8238), FinalCombine_BloomTonemap_Vignette
/// (0x821BD8C4, 0x821C8ABC) as two halves.
///
/// Inferred: bilinear filtering in blur, add and combine; vignette scale and
/// offset (from 0x8248B278, not traced) cover the full picture.
class PostProcess {
public:
    PostProcess();
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    /// Leaves blending, depth and scissor off and `output` bound.
    void run(GLuint scene, int width, int height, GLuint output, const PostFrame& frame);

private:
    struct Target {
        GLuint texture = 0;
        GLuint framebuffer = 0;
        int width = 0;
        int height = 0;
    };

    void resize(int width, int height);
    static void create(Target& target, int width, int height, GLenum format);
    static void destroy(Target& target);
    void bindTarget(const Target& target) const;
    void draw() const;

    Shader m_downsample;
    Shader m_biased;
    Shader m_accumulate;
    Shader m_hotExtract;
    Shader m_blur;
    Shader m_add;
    Shader m_combine;
    GLuint m_emptyVao = 0;
    GLuint m_linear = 0;
    GLuint m_point = 0;
    GLuint m_lut = 0;
    int m_width = 0;
    int m_height = 0;
    Target m_d1;
    Target m_d2;
    Target m_d3;
    std::array<Target, 2> m_luminance;
    int m_luminanceCurrent = 0;
    bool m_luminanceValid = false;
    Target m_hot;
    Target m_bloom;
    bool m_bloomValid = false;
    static constexpr int kLevels = 4;
    std::array<Target, kLevels> m_blurX;
    std::array<Target, kLevels> m_blurY;
    std::array<Target, kLevels> m_composite;
};

} // namespace openhorizon::render
