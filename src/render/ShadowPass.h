#pragma once

#include "Gl.h"
#include "Shader.h"
#include "ShadowCascades.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>

namespace openhorizon::render {

/// The game's sun shadows on the GPU: the shadow maps, a depth pass of the
/// screen and the screen-space shadow mask the scene's effects sample as
/// ShadowMaskSamp (the track effects' Default pixel shaders multiply the
/// sun's light and specular by its red channel, fetched at the pixel's own
/// screen position).
///
/// The shadow manager's orchestrator (0x824829D8, called from the frame at
/// 0x8242A48C) builds the mask; verified there:
///  - The mask target is cleared white, its stencil to 0 (0x82482F9C).
///  - The three cascades are applied nearest first, blending off, each
///    drawing the back of its light box (the eight corners of 0x8241C100,
///    shrunk by its +0x2D4) with the depth test "further or equal" against
///    the screen depth, and the stencil test "equal 0" incremented where it
///    passes (0x824831E8, 0x82483380), so each pixel takes the first
///    cascade that holds it. The first two use ApplyShadowsLQ, the last
///    ApplyShadowsLQWithFading_Depth (the technique choice at 0x82CFB8D8,
///    fed the flags the loop sets at 0x824834F4..0x824835E4).
///  - The stencil is cleared again (0x82483674) and the levels are applied
///    with min blending (0x82483600..0x82483624): level 0 with
///    ApplyShadowsLQ; levels 1 and 2 in three passes each, an inner box
///    (ApplyShadowsLQ_SpreadKernel, stencil incremented), the outer ring
///    fading out (ApplyShadowsLQ_FadeOuter_SpreadKernel, stencil kept) and
///    the next level fading in over the same ring
///    (ApplyShadowsLQ_FadeInner_SpreadKernel after level 1,
///    ApplyShadowsLQ_FadeInner after level 2, stencil incremented); level 3
///    with ApplyShadowsLQWithFading_UV (0x824839A8..0x82484040).
///  - The mask is blurred once (this+0x20F4 = 1, 0x824841AC) with
///    BlurShadowMask, four bilinear taps half a texel away
///    (the sampler set up at 0x824840BC..0x82484134), over a quad at the
///    depth of 250 m (+0x20F0, 0x82484144) tested "further or equal", so
///    only pixels nearer than 250 m are blurred.
///  - Shadows are drawn only while 1 - ShadowIntensity is below 0.99
///    (+0xF78, 0x8247EA74; 0x82482F28); otherwise the mask is left white.
///
/// The pixel shaders are ports of the executable's post effect techniques
/// (the effect block at 0x821B9918), each step following the microcode:
/// the fragment's view depth from the screen depth, its shadow map
/// coordinate through ShadowMapLevelTM (saturated), four taps of a kernel
/// FilterTaps rotated per pixel by 2π·frac(dot(pixel, RotVariation)/2π +
/// 1/2) − π with RotVariation (8.4811, 5.49169) (the constructor's
/// 0x82256100), lit where the map's depth is at least the fragment's, and
/// the result 1 − ShadowIntensity + fade + lit fraction, saturated by the
/// 8-bit target. The spread kernels widen the kernel beyond 25 m by 1 + 8s
/// (and average with 1 + 16s where s > 1/2), s = saturate((z below − z −
/// 1/2) / 4) from the depth of the pixel below.
///
/// Inferred rather than read: the shadow map texture matrices, which map
/// this renderer's own views into its maps as it renders them (the
/// executable's ShadowMapLevelTM and ShadowMapLevelTMFade builders were
/// not traced), with the fade matrix equal to the map one. The screen
/// depth pass stands in for the executable's "Screen Depth Texture".
/// Verified by experiment: the translated effects read the mask with its
/// first row at the top of the screen (a mask black over its first rows
/// darkens the top of the picture), so the mask and the screen depth are
/// drawn with y flipped.
/// Missing: the cars' SMask pass (not traced where it is drawn), so the
/// screen depth holds the world only; and the cars' own pixels of the mask
/// keep what lies behind them.
class ShadowPass {
public:
    /// GPU seconds of the last completed frame.
    struct Timings {
        double shadowMaps = 0.0;
        double depth = 0.0;
        double mask = 0.0;
    };

    ShadowPass();
    ~ShadowPass();
    ShadowPass(const ShadowPass&) = delete;
    ShadowPass& operator=(const ShadowPass&) = delete;

    /// Whether the game draws shadows for this ShadowIntensity.
    static bool active(float shadowIntensity);

    /// Binds level `index`'s tile of the level map, cleared, with the depth
    /// state and bias for rendering casters (see casterState()).
    void beginLevel(std::size_t index);
    /// Binds cascade `index`'s map, cleared.
    void beginCascade(std::size_t index);
    /// Restores the state the scene passes expect after the maps.
    void endShadowMaps();

    /// Binds the screen depth target of `width` × `height`, cleared to the
    /// far plane, for drawing the scene's depth with flippedViewProjection():
    /// the target stores rows from the top of the screen, as the mask must
    /// (the game's effects fetch it at Direct3D screen coordinates).
    void beginDepth(int width, int height);
    void endDepth();
    /// The camera's view-projection with y negated, for beginDepth().
    static glm::mat4 flippedViewProjection(const glm::mat4& viewProjection);

    /// Builds the mask from the maps and the depth drawn since the calls
    /// above. `viewProjection` is the camera's (unflipped).
    void buildMask(const ShadowCascades& cascades, const ShadowCamera& camera, const glm::mat4& viewProjection,
        float shadowIntensity);
    /// Leaves the mask white (fully lit) at `width` × `height`.
    void clearMask(int width, int height);

    /// Whether the next frame at this size replaces the screen textures,
    /// mask() included: users of the mask must let go of it first.
    bool resizes(int width, int height) const
    {
        return (width < 1 ? 1 : width) != m_width || (height < 1 ? 1 : height) != m_height;
    }

    /// The mask texture (red: sun visibility), rows from the top.
    GLuint mask() const { return m_mask; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    const Timings& timings() const { return m_timings; }

private:
    enum class Variant : std::size_t {
        Lq,
        LqFadingDepth,
        LqFadingUv,
        LqSpread,
        LqFadeOuterSpread,
        LqFadeInnerSpread,
        LqFadeInner,
        Count,
    };
    /// Where one ApplyShadows pass samples and what it fades with.
    struct Apply {
        Variant variant = Variant::Lq;
        GLuint map = 0;
        /// Camera-relative world position to (u, v, depth).
        glm::mat4 texture{1.0F};
        glm::vec2 taps{0.0F};
        std::array<glm::vec4, 2> filter{};
        /// ViewParallelSurfaceFadingParams (c26).
        glm::vec4 surfaceFade{0.0F};
        /// LinDepthAndFadingParams z, w (c25).
        glm::vec2 depthFade{0.0F};
    };

    void resizeScreen(int width, int height);
    void drawApply(const Apply& apply, const ShadowFrustum& box, float shrink, bool incrementStencil);
    void blur();
    void beginTimer(std::size_t which);
    void endTimer();
    void collectTimers();

    Shader m_apply[static_cast<std::size_t>(Variant::Count)];
    Shader m_blur;
    GLuint m_levelMap = 0;
    GLuint m_levelFramebuffer = 0;
    std::array<GLuint, ShadowCascades::kCascades> m_cascadeMaps{};
    std::array<GLuint, ShadowCascades::kCascades> m_cascadeFramebuffers{};
    /// The screen: depth and stencil drawn by the depth pass, its copy the
    /// apply passes read, the mask and the mask's copy the blur reads.
    GLuint m_depth = 0;
    GLuint m_depthCopy = 0;
    GLuint m_mask = 0;
    GLuint m_maskCopy = 0;
    GLuint m_screenFramebuffer = 0;
    GLuint m_depthCopyFramebuffer = 0;
    GLuint m_maskCopyFramebuffer = 0;
    GLuint m_depthOnlyFramebuffer = 0;
    GLuint m_boxVao = 0;
    GLuint m_boxBuffer = 0;
    GLuint m_emptyVao = 0;
    int m_width = 0;
    int m_height = 0;
    /// Camera state buildMask() hands its passes.
    ShadowCamera m_camera;
    glm::mat4 m_flipped{1.0F};
    float m_intensity = 0.0F;

    static constexpr std::size_t kTimerFrames = 4;
    static constexpr std::size_t kTimers = 3;
    std::array<std::array<GLuint, kTimers>, kTimerFrames> m_queries{};
    std::array<std::array<bool, kTimers>, kTimerFrames> m_pending{};
    std::size_t m_frame = 0;
    std::size_t m_timing = kTimers;
    Timings m_timings;
};

} // namespace openhorizon::render
