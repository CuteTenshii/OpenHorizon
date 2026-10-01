#pragma once

#include "Gl.h"
#include "Shader.h"
#include "ShadowCascades.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>

namespace openhorizon::render {

/// Sun shadow maps, screen depth and the ShadowMaskSamp mask (red: sun
/// visibility). Verified against the orchestrator (0x824829D8, called at
/// 0x8242A48C): mask cleared white, stencil 0 (0x82482F9C); cascades nearest
/// first under stencil (0x824831E8, 0x82483380), each drawing its box's back
/// (corners from 0x8241C100), technique chosen at 0x82CFB8D8 from flags set
/// at 0x824834F4-0x824835E4; stencil cleared again (0x82483674), levels with
/// min blending (0x82483600-0x82483624, 0x824839A8-0x82484040); one blur
/// (+0x20F4, 0x824841AC; sampler 0x824840BC-0x82484134) nearer than 250 m
/// (+0x20F0, 0x82484144); none while 1 - ShadowIntensity >= 0.99 (+0xF78,
/// 0x8247EA74; 0x82482F28). Shaders port the post
/// effect at 0x821B9918; RotVariation from 0x82256100.
///
/// Inferred: the ShadowMapLevelTM matrices (builders not traced). Verified by
/// experiment: the effects read the mask top row first, so it is y-flipped.
/// Missing: the cars' SMask pass, so the screen depth holds the world only.
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

    /// Binds level `index`'s tile, cleared, with the casters' depth bias.
    void beginLevel(std::size_t index);
    void beginCascade(std::size_t index);
    void endShadowMaps();

    /// Draw with flippedViewProjection(): rows are stored top first, as the
    /// effects fetch at Direct3D screen coordinates.
    void beginDepth(int width, int height);
    void endDepth();
    /// The camera's view-projection with y negated, for beginDepth().
    static glm::mat4 flippedViewProjection(const glm::mat4& viewProjection);

    /// `viewProjection` is the camera's, unflipped.
    void buildMask(const ShadowCascades& cascades, const ShadowCamera& camera, const glm::mat4& viewProjection,
        float shadowIntensity);
    /// Leaves the mask white (fully lit).
    void clearMask(int width, int height);

    /// Whether this size replaces the screen textures, mask() included:
    /// users of the mask must let go of it first.
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
    /// The apply passes read the depth copy; the blur reads the mask copy.
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
