#pragma once

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <span>

namespace fh::render {

/// The camera the shadows are fitted around: world position and axes, near
/// plane and the tangents of the half field of view, as the shadow manager
/// keeps them from its SetCamera (0x824A8698: the camera's world matrix at
/// +0x00, its position in that matrix's last row at +0x30, the near plane
/// at +0x100).
struct ShadowCamera {
    glm::vec3 position{0.0F};
    glm::vec3 right{1.0F, 0.0F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    float nearPlane = 0.1F;
    float tanHalfWidth = 1.0F;
    float tanHalfHeight = 1.0F;
};

/// One orthographic light projection. Matrices are in the column-vector
/// form glm uses; the executable keeps the transposes (Direct3D row
/// vectors). `projection` maps light space to Direct3D clip space: x and y
/// from -1 to 1, depth from 0 (towards the light) to 1.
struct ShadowFrustum {
    /// Light space to world: the light basis with its origin.
    glm::mat4 world{1.0F};
    glm::mat4 view{1.0F};
    glm::mat4 projection{1.0F};
    glm::mat4 viewProjection{1.0F};
};

struct ShadowSphere {
    glm::vec3 centre{0.0F};
    float radius = 0.0F;
};

/// The helpers the shadow manager builds its projections from, each
/// following one function of the executable.
namespace shadowmath {

/// The light's world matrix (0x8241B8D0): rows u, v, n and the origin,
/// where n is the light direction as given (not normalised: the track's
/// ShadowLightDirection is used as written) and u, v come from 0x8241ADB8:
/// if |n.x| > |n.y|, u = (n.z, 0, -n.x) / sqrt(n.x² + n.z²), otherwise
/// u = (0, -n.z, n.y) / sqrt(n.y² + n.z²); v = n × u.
glm::mat4 lightWorld(const glm::vec3& direction, const glm::vec3& origin);

/// Direct3D's left-handed off-centre orthographic projection with depth
/// from 0 at `nearZ` to 1 at `farZ` (0x8249BD70).
glm::mat4 orthoOffCenter(float left, float right, float bottom, float top, float nearZ, float farZ);

/// The fitted projection (0x82497DB0): the light basis at `centre`, an
/// orthographic box of half extents `halfWidth`, `halfHeight` and
/// `halfDepth`, and with `snap` the origin moved so that the world's
/// origin projects onto a texel corner of a `resolution` wide map: the
/// centre is projected by the view-projection without its translation,
/// each of x and y becomes `t - fmod(x, t) + x` for a texel of t = 2 /
/// resolution, and the point that projects there becomes the new origin.
ShadowFrustum fit(const glm::vec3& direction, const glm::vec3& centre, float halfWidth, float halfHeight,
    float halfDepth, bool snap, int resolution);

/// The eight corners of `frustum`'s box in world space with its x and y
/// scaled by 1 - `shrink` (0x8241C4D0): the Direct3D clip corners (±1, ±1)
/// at depth 0 and 1, in the order of the executable's table at 0x833A4720:
/// (1, 1), (-1, 1), (-1, -1), (1, -1) at depth 0, then the same at depth 1.
std::array<glm::vec3, 8> boxCorners(const ShadowFrustum& frustum, float shrink);

/// The world corners of the camera frustum between view distances `nearZ`
/// and `farZ` (what 0x8241C4D0 computes from the camera's view and a copy
/// of its projection whose depth terms are replaced for that slice).
std::array<glm::vec3, 8> sliceCorners(const ShadowCamera& camera, float nearZ, float farZ);

/// The centre of the points' bounding box and the largest distance from
/// it to a point (0x8241A348).
ShadowSphere boundingSphere(std::span<const glm::vec3> points);

/// The filter taps of the ApplyShadows "LQ" family (technique 4 of
/// 0x82476CD0) for a kernel of `scale` texture-space units: FilterTaps c32
/// = (s, s, s/2, -s/2), c33 = (-s, -s, -s/2, s/2).
std::array<glm::vec4, 2> filterTaps(float scale);

/// The linear depth fade of 0x8243D378: over the last tenth of `distance`
/// (from 0.9 to 0.9999 of it) a pixel's shadow is faded out; returns
/// (1 / (0.9999 d - 0.9 d), -0.9 d / (0.9999 d - 0.9 d)).
glm::vec2 depthFade(float distance);

} // namespace shadowmath

/// Where the game's sun shadow maps are, frame to frame. The shadow manager
/// (constructed at 0x82EA67C0, updated at 0x82EA0CD8) keeps two sets:
///
/// - Four cached levels ("CSM shadow map", created at 0x82E9F7E8): 512² tiles
///   of a 1024² map, level i at tile (i & 1, i >> 1). Presets from
///   0x82EA0120, called from the constructor: range 25, 50, 200, 600 m;
///   margin 0.05, 0.05, 0.02, 0.01; depth half range 1000, 1000, 1000, 1300
///   (capped at 2000, 0x82255EA4); light angle tolerance 0.07, 0.08, 0.21,
///   0.43; direction tolerance 1e-7, 1.1e-7, 2.1e-7, 2.3e-7. A level is placed
///   (0x824181B8) on the camera, fitted with half extent range × (1 +
///   margin) and snapped to its texels, and rendered once; it is placed
///   again when the camera has moved more than margin × range from its
///   centre (the distance scaled by 0.75 for each level already waiting to
///   be rendered, so that fewer move at once), when the sun's angle or
///   direction changes past the tolerances, or, when no level waits, one
///   level per frame in turn (0x824A9840).
/// - Three cascades ("PSSM shadow map", 0x82EA0290), fitted every frame to
///   slices of the camera frustum ending 4, 15 and 40 m out (the table the
///   constructor builds; the first starts at the camera's near plane), each
///   in a sphere around its slice's bounding box whose radius is rounded up
///   to a whole metre, snapped to its texels, with a depth half range of 100,
///   200, 300 m. The first is 1024² and the others 512². The light direction
///   they use is only taken over when it moves past a cosine of
///   1 - 50 × 1e-7 (0x82EA1018).
///
/// Verified: everything above, in the functions named. Inferred: that the
/// per-frame update runs every frame in play (it is gated on a flag of the
/// camera, vtable 0xA0 at 0x8247ECAC, whose meaning was not traced).
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
        /// 20 m × tan of the light's angle from the vertical, the shadow
        /// length the angle test compares (0x82EA0EA0..0x82EA0EE0).
        float shadowLength = 0.0F;
        ShadowFrustum frustum;
        /// ApplyShadows box shrink (level +0x324): (filter + 0.5) / 512 +
        /// margin / (1 + margin).
        float shrink = 0.0F;
        /// The inner box of levels 1 and 2 (+0x32C): max(0.4 - shrink, 0);
        /// the same as `shrink` for levels 0 and 3.
        float innerShrink = 0.0F;
        /// The level was placed and waits to be drawn (+0x338 clear).
        bool dirty = true;
        bool placed = false;
    };

    struct Cascade {
        float farDistance = 0.0F;
        float nearDistance = 0.0F;
        int resolution = 0;
        ShadowSphere sphere;
        ShadowFrustum frustum;
        /// Distance from the camera to the far side of the sphere along the
        /// view (cascade +0x2F0).
        float reach = 0.0F;
        /// ApplyShadows box shrink (+0x2D4): (filter + 1.5) / 512.
        float shrink = 0.0F;
        /// Kernel scale in texels (0x2094 × 0x2098^i: 2, 0.7, 0.245).
        float filter = 0.0F;
    };

    ShadowCascades();

    /// Moves the shadows on for the camera and the light direction
    /// (where the light travels, pointing down by day).
    void update(const ShadowCamera& camera, const glm::vec3& lightDirection);

    const std::array<Level, kLevels>& levels() const { return m_levels; }
    const std::array<Cascade, kCascades>& cascades() const { return m_cascades; }
    /// Marks a level drawn.
    void markDrawn(std::size_t level) { m_levels[level].dirty = false; }
    /// The kernel scale in texels for level `i` (1.3 × 0.75^i; 0x82EA0D70,
    /// the orchestrator's fr31 at 0x82483630).
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

} // namespace fh::render
