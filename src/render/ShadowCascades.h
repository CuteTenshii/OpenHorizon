#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace openhorizon::render {

/// The camera as the shadow manager's SetCamera keeps it (0x824A8698).
struct ShadowCamera {
    glm::vec3 position{0.0F};
    glm::vec3 right{1.0F, 0.0F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    float nearPlane = 0.1F;
    float tanHalfWidth = 1.0F;
    float tanHalfHeight = 1.0F;
};

/// Column-vector matrices (the executable keeps the transposes).
/// `projection` maps to Direct3D clip space, depth 0 towards the light.
struct ShadowFrustum {
    /// Light space to world.
    glm::mat4 world{1.0F};
    glm::mat4 view{1.0F};
    glm::mat4 projection{1.0F};
    glm::mat4 viewProjection{1.0F};
};

struct ShadowSphere {
    glm::vec3 centre{0.0F};
    float radius = 0.0F;
};

/// Each helper ports one function of the executable.
namespace shadowmath {

/// The light's world matrix (0x8241B8D0, basis from 0x8241ADB8). The
/// direction is not normalised: ShadowLightDirection is used as written.
glm::mat4 lightWorld(const glm::vec3& direction, const glm::vec3& origin);

/// Direct3D's left-handed off-centre orthographic projection (0x8249BD70).
glm::mat4 orthoOffCenter(float left, float right, float bottom, float top, float nearZ, float farZ);

/// The fitted projection (0x82497DB0); `snap` moves the origin so the world
/// origin projects onto a texel corner.
ShadowFrustum fit(const glm::vec3& direction, const glm::vec3& centre, float halfWidth, float halfHeight,
    float halfDepth, bool snap, int resolution);

/// `frustum`'s world corners, x and y scaled by 1 - `shrink` (0x8241C4D0),
/// in the order of the table at 0x833A4720.
std::array<glm::vec3, 8> boxCorners(const ShadowFrustum& frustum, float shrink);

/// The camera frustum's world corners between `nearZ` and `farZ` (0x8241C4D0).
std::array<glm::vec3, 8> sliceCorners(const ShadowCamera& camera, float nearZ, float farZ);

/// Centred on the bounding box (0x8241A348).
ShadowSphere boundingSphere(std::span<const glm::vec3> points);

/// FilterTaps c32 and c33 of the ApplyShadows "LQ" family (technique 4 of
/// 0x82476CD0) for `scale` texture-space units.
std::array<glm::vec4, 2> filterTaps(float scale);

/// The linear depth fade of 0x8243D378, from 0.9 to 0.9999 of `distance`.
glm::vec2 depthFade(float distance);

} // namespace shadowmath

/// The sun shadow maps (manager made at 0x82EA67C0, updated at 0x82EA0CD8):
/// four cached "CSM" levels in a 1024² atlas (0x82E9F7E8; presets 0x82EA0120,
/// depth half range capped at 2000 by 0x82255EA4; placement
/// 0x824181B8, turn refresh 0x824A9840) and three per-frame "PSSM" cascades
/// (0x82EA0290, direction threshold 0x82EA1018). Verified in those functions.
/// Inferred: the update runs every frame in play (gated at 0x8247ECAC).
class ShadowCascades {
public:
    static constexpr std::size_t kLevels = 4;
    static constexpr std::size_t kCascades = 3;
    /// Each level's tile in the 1024² map.
    static constexpr int kLevelResolution = 512;
    static constexpr int kAtlasResolution = 1024;

    struct Level {
        float range = 0.0F;
        float margin = 0.0F;
        float depthRange = 0.0F;
        float angleTolerance = 0.0F;
        double directionTolerance = 0.0;
        glm::vec3 centre{0.0F};
        glm::vec3 direction{0.0F, 0.0F, 1.0F};
        /// 20 m × tan of the light's angle from vertical (0x82EA0EA0-0x82EA0EE0).
        float shadowLength = 0.0F;
        ShadowFrustum frustum;
        /// ApplyShadows box shrink (level +0x324): (filter + 0.5) / 512 +
        /// margin / (1 + margin).
        float shrink = 0.0F;
        /// The inner box of levels 1 and 2 (+0x32C): max(0.4 - shrink, 0);
        /// the same as `shrink` for levels 0 and 3.
        float innerShrink = 0.0F;
        /// Placed and waiting to be drawn (+0x338 clear).
        bool dirty = true;
        bool placed = false;
    };

    struct Cascade {
        float farDistance = 0.0F;
        float nearDistance = 0.0F;
        int resolution = 0;
        ShadowSphere sphere;
        ShadowFrustum frustum;
        /// To the sphere's far side along the view (cascade +0x2F0).
        float reach = 0.0F;
        /// ApplyShadows box shrink (+0x2D4): (filter + 1.5) / 512.
        float shrink = 0.0F;
        /// Kernel scale in texels (0x2094 × 0x2098^i: 2, 0.7, 0.245).
        float filter = 0.0F;
    };

    ShadowCascades();

    /// `lightDirection` points where the light travels (down by day).
    void update(const ShadowCamera& camera, const glm::vec3& lightDirection);

    const std::array<Level, kLevels>& levels() const { return m_levels; }
    const std::array<Cascade, kCascades>& cascades() const { return m_cascades; }
    void markDrawn(std::size_t level) { m_levels[level].dirty = false; }
    /// In texels: 1.3 × 0.75^i (0x82EA0D70, 0x82483630).
    static float levelFilter(std::size_t level);

private:
    void place(Level& level, std::size_t index, const glm::vec3& cameraPosition);

    std::array<Level, kLevels> m_levels{};
    std::array<Cascade, kCascades> m_cascades{};
    /// The level refreshed next when none waits (+0xF9C).
    std::size_t m_turn = 0;
    /// The light direction the cascades use (+0xFB0).
    glm::vec3 m_cascadeDirection{0.0F, 0.0F, 1.0F};
    bool m_first = true;
};

} // namespace openhorizon::render
