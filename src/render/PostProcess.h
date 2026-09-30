#pragma once

#include "Gl.h"
#include "PostMath.h"
#include "Shader.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace fh::render {

/// One half of the screen's vignette as the combine takes it.
struct PostVignette {
    /// See post::vignetteTransform().
    glm::vec4 transform{0.0F};
    /// Added to the pixel's offset from the picture's centre (in the
    /// game's texture space: x right, y down, the picture spanning 1).
    glm::vec2 position{0.0F};
    float power = 1.0F;
    /// rgb, and in a the strength: the colour is added to the picture
    /// minus one, clamped, and blended in by the vignette times a.
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

    /// The four average-luminance keys, already divided by 255, and the
    /// log2 exposure at each.
    glm::vec4 luminanceKeys{0.0F};
    glm::vec4 exposures{0.0F};
    /// Added to the interpolated exposure (filmicParams2.w: the filmic
    /// exposure plus the exposure bias).
    float exposureBias = 0.0F;
    post::Filmic filmic;
    /// How far the adapted luminance moves towards the frame's each frame
    /// (tonemapDelay), and whether it jumps there (tonemapReset).
    float adaptation = 0.0F;
    bool resetAdaptation = false;

    glm::mat4 colourTransform{1.0F};
    /// The 16x16x16 grading table (RGBA8, red along x, blue along the
    /// slices); empty for none.
    std::span<const std::uint8_t> gradingLut;

    PostVignette vignetteTop;
    PostVignette vignetteBottom;
};

/// The game's post-processing from the scene target to the picture, pass
/// for pass as the executable runs it (the luminance and bloom chain at
/// 0x82494D58, the final combine set up at 0x82432628), with the shaders
/// following the executable's embedded post effect (block marker at
/// 0x821B9918).
///
/// The scene target holds the square root of linear colour, as the game's
/// does: the game resolves it into a 10-bit fixed-point texture with an
/// exponent bias of 2 (render target table 0x8330C830, entry 3
/// "WorldRender": format 0x1A2001B6, k_2_10_10_10_AS_16_16_16_16, bias 2),
/// so reading it gives the square root in [0, 4]. Every down-sampled and
/// bloom texture has the same format, so every pass here clamps what it
/// writes to [0, 4]; the 10-bit quantisation is not reproduced.
///
/// Passes (sizes for a W x H picture):
///  1. DownSample16XGammaCorrect (0x821BEBE0): the scene to D1, align32(W/4)
///     x align32(H/4) (entry 12), each texel sqrt of the mean of the squares
///     of 16 point samples at +-0.5 and +-1.5 source texels.
///  2. The same from D1 to D2, 64 x 32 (entry 22).
///  3. DownSampleBiased (0x821BF1BC): D2 to D3, 32 x 32 (entry 23), each
///     texel the mean of 16 bilinear samples along its row at u = (i +
///     0.5) / 16, without squaring.
///  4. MultiSampleAndAccumulate (0x821BF658): 16 bilinear samples of D3
///     down its centre column at v = (i + 0.5) / 16, weighted by
///     smoothstep(range.x, range.y, 0.5 - |v - 0.5|) (the range is (0, 0)
///     outside the cockpit view, 0x825E6E50, so every weight is 1),
///     squared, averaged and turned into luminance with (0.2125, 0.7154,
///     0.0721); the adapted luminance moves towards it by `adaptation`
///     once per frame, or jumps to it on a reset.
///  5. Add_HotExtract (0x821C83FC, set up at 0x82435800): D1 exposed as
///     the combine exposes it, scaled by the knee curve max(l U + (Lo - U)
///     Cut, l Lo) / l of its luminance l, less 1/255, plus the previous
///     frame's bloom squared times persist * 60, all times Scale.
///  6. Four blur levels (GaussianBlurAccumX / Y, 0x821CF760 / 0x821CF984):
///     level i halves the width with a 7-tap horizontal pass and then the
///     height with a 7-tap vertical one, weights (0.240952, 0.212639,
///     0.122003, 0.0448822) (0x82001210), on squared values.
///  7. Add2Textures_UVScale (0x821C8238), upwards: L2 = 0.2 w3 L2^2 + 0.2
///     w4 L3^2, L1 = 0.2 w2 L1^2 + L2^2, L0 = 0.2 w1 L0^2 + L1^2, and the
///     bloom = 0.2 w0 hot^2 + L0^2, kept for the next frame's persistence.
///  8. The final combine (FinalCombine_BloomTonemap_Vignette, 0x821BD8C4
///     and 0x821C8ABC), drawn as two halves with the top and the bottom
///     vignette: linear = scene^2 + bloom^2, exposed by 2^exposure, filmic
///     curve over its value at the white point, the grading table sampled
///     at that times 15/16 plus 1/32, square root, vignette, squared
///     again, the colour matrix, clamped to [0, 1] and written as its
///     square root, which is what the game sends to the screen.
///
/// Inferred, not read: the blur, add and combine passes filter bilinearly
/// (their fetches carry no filter of their own and the sampler states of
/// the post effect were not decoded); pixel centres are at half-texel
/// coordinates; the combine's vignetteScaleOffsetPower.xyz, which come
/// from the viewport (0x8248B278 + 0x94 + 0x16C) and were not traced, are
/// the full picture's (1, 0, 0), so the vignette is centred and the bloom
/// read over the whole picture.
class PostProcess {
public:
    PostProcess();
    ~PostProcess();
    PostProcess(const PostProcess&) = delete;
    PostProcess& operator=(const PostProcess&) = delete;

    /// Runs every pass on `scene` (width x height, square-root colour) and
    /// draws the picture into `output` over the same size. Leaves blending,
    /// depth and scissor off and `output` bound.
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

} // namespace fh::render
