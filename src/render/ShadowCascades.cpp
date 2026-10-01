#include "ShadowCascades.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace openhorizon::render {

namespace {

/// Presets of the four levels, from the constructor's calls to 0x82EA0120
/// (0x82EA6AC4..0x82EA6B94).
struct LevelPreset {
    float range;
    float margin;
    float angleTolerance;
    double directionTolerance;
};
constexpr std::array<LevelPreset, ShadowCascades::kLevels> kLevelPresets{
    LevelPreset{25.0F, 0.05F, 0.07F, 1e-7},
    LevelPreset{50.0F, 0.05F, 0.08F, 1.1e-7},
    LevelPreset{200.0F, 0.02F, 0.21F, 2.1e-7},
    LevelPreset{600.0F, 0.01F, 0.43F, 2.3e-7},
};
/// A level's depth half range (level +0x30C: 1000, 1300 for the last,
/// 0x82EA0254..0x82EA0270), capped at 2000 (0x82255EA4) when fitted.
constexpr float kLevelDepthRange = 1000.0F;
constexpr float kLastLevelDepthRange = 1300.0F;
constexpr float kDepthRangeCap = 2000.0F;
/// The multiplier of the direction tolerance (+0x224, 0x78 in the
/// constructor) and of the angle tolerance (+0x228, 1.0).
constexpr double kDirectionToleranceScale = 120.0;
constexpr float kAngleToleranceScale = 1.0F;
/// The height whose shadow length the angle test compares (0x82000DA4).
constexpr float kAngleTestHeight = 20.0F;
/// Each level waiting to be drawn scales the next ones' movement test, and
/// each level's kernel, by this (0x82000E18; +0xF80).
constexpr float kStagger = 0.75F;
/// The first level's kernel scale (+0xF7C).
constexpr float kFirstLevelFilter = 1.3F;
/// Levels 1 and 2 split their box at 1 - 0.6 of it (0x82255EF8).
constexpr float kInnerSplit = 0.6F;

/// Far ends and depth half ranges (0x82E9F500, 0x82EA04CC); kernel scale
/// and per-cascade factor (+0x2094, +0x2098).
constexpr std::array<float, ShadowCascades::kCascades> kCascadeFar{4.0F, 15.0F, 40.0F};
constexpr float kCascadeDepthStep = 100.0F;
constexpr std::array<int, ShadowCascades::kCascades> kCascadeResolution{1024, 512, 512};
constexpr float kFirstCascadeFilter = 2.0F;
constexpr float kCascadeFilterStep = 0.35F;
/// The cascades' light direction follows the sun past this cosine: 1 -
/// +0x20B8 (50) × +0x20B0 (1e-7).
constexpr float kCascadeTurn = 1.0F - 50.0F * 1e-7F;

/// The executable's table of box corners (0x833A4720).
constexpr std::array<glm::vec3, 8> kCorners{
    glm::vec3(1.0F, 1.0F, 0.0F),
    glm::vec3(-1.0F, 1.0F, 0.0F),
    glm::vec3(-1.0F, -1.0F, 0.0F),
    glm::vec3(1.0F, -1.0F, 0.0F),
    glm::vec3(1.0F, 1.0F, 1.0F),
    glm::vec3(-1.0F, 1.0F, 1.0F),
    glm::vec3(-1.0F, -1.0F, 1.0F),
    glm::vec3(1.0F, -1.0F, 1.0F),
};

/// Rounds up to a whole number (0x82418368).
float roundUp(float value)
{
    return std::ceil(value);
}

} // namespace

namespace shadowmath {

glm::mat4 lightWorld(const glm::vec3& direction, const glm::vec3& origin)
{
    const glm::vec3& n = direction;
    glm::vec3 u(0.0F);
    if (std::abs(n.x) > std::abs(n.y)) {
        const float inverse = 1.0F / std::sqrt(n.x * n.x + n.z * n.z);
        u = glm::vec3(n.z * inverse, 0.0F, -n.x * inverse);
    } else {
        const float inverse = 1.0F / std::sqrt(n.y * n.y + n.z * n.z);
        u = glm::vec3(0.0F, -n.z * inverse, n.y * inverse);
    }
    const glm::vec3 v = glm::cross(n, u);
    return glm::mat4(glm::vec4(u, 0.0F), glm::vec4(v, 0.0F), glm::vec4(n, 0.0F), glm::vec4(origin, 1.0F));
}

glm::mat4 orthoOffCenter(float left, float right, float bottom, float top, float nearZ, float farZ)
{
    glm::mat4 m(0.0F);
    m[0][0] = 2.0F / (right - left);
    m[1][1] = 2.0F / (top - bottom);
    m[2][2] = 1.0F / (farZ - nearZ);
    m[3][0] = (left + right) / (left - right);
    m[3][1] = (top + bottom) / (bottom - top);
    m[3][2] = nearZ / (nearZ - farZ);
    m[3][3] = 1.0F;
    return m;
}

ShadowFrustum fit(const glm::vec3& direction, const glm::vec3& centre, float halfWidth, float halfHeight,
    float halfDepth, bool snap, int resolution)
{
    ShadowFrustum f;
    f.world = lightWorld(direction, centre);
    f.view = glm::inverse(f.world);
    f.projection = orthoOffCenter(-halfWidth, halfWidth, -halfHeight, halfHeight, -halfDepth, halfDepth);
    f.viewProjection = f.projection * f.view;
    if (!snap) {
        return f;
    }
    // The centre through the view-projection's linear part only, so the
    // grid it is snapped to stays put in the world as the centre moves.
    glm::vec3 p = glm::mat3(f.viewProjection) * centre;
    const double texel = 2.0 / static_cast<double>(resolution);
    for (int axis = 0; axis < 2; ++axis) {
        const double x = p[axis];
        p[axis] = static_cast<float>(texel - std::fmod(x, texel) + x);
    }
    const glm::vec3 origin = glm::mat3(glm::inverse(f.viewProjection)) * p;
    f.world[3] = glm::vec4(origin, 1.0F);
    f.view = glm::inverse(f.world);
    f.viewProjection = f.projection * f.view;
    return f;
}

std::array<glm::vec3, 8> boxCorners(const ShadowFrustum& frustum, float shrink)
{
    // The executable inverts in double precision (0x8241C168).
    const glm::dmat4 inverse = glm::inverse(glm::dmat4(frustum.viewProjection));
    const double keep = 1.0 - static_cast<double>(shrink);
    std::array<glm::vec3, 8> corners{};
    for (std::size_t i = 0; i < kCorners.size(); ++i) {
        const glm::dvec4 clip(kCorners[i].x * keep, kCorners[i].y * keep, kCorners[i].z, 1.0);
        const glm::dvec4 world = inverse * clip;
        corners[i] = glm::vec3(glm::dvec3(world) / world.w);
    }
    return corners;
}

std::array<glm::vec3, 8> sliceCorners(const ShadowCamera& camera, float nearZ, float farZ)
{
    std::array<glm::vec3, 8> corners{};
    for (std::size_t i = 0; i < kCorners.size(); ++i) {
        const float distance = kCorners[i].z == 0.0F ? nearZ : farZ;
        corners[i] = camera.position + camera.forward * distance
            + camera.right * (kCorners[i].x * distance * camera.tanHalfWidth)
            + camera.up * (kCorners[i].y * distance * camera.tanHalfHeight);
    }
    return corners;
}

ShadowSphere boundingSphere(std::span<const glm::vec3> points)
{
    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(-std::numeric_limits<float>::max());
    for (const glm::vec3& p : points) {
        low = glm::min(low, p);
        high = glm::max(high, p);
    }
    ShadowSphere sphere;
    sphere.centre = (low + high) * 0.5F;
    double largest = -1.0;
    for (const glm::vec3& p : points) {
        const glm::dvec3 d = glm::dvec3(p) - glm::dvec3(sphere.centre);
        largest = std::max(largest, glm::dot(d, d));
    }
    sphere.radius = static_cast<float>(std::sqrt(std::max(largest, 0.0)));
    return sphere;
}

std::array<glm::vec4, 2> filterTaps(float scale)
{
    return {
        glm::vec4(scale, scale, 0.5F * scale, -0.5F * scale), glm::vec4(-scale, -scale, -0.5F * scale, 0.5F * scale)};
}

glm::vec2 depthFade(float distance)
{
    const float start = distance * 0.9F;
    const float end = distance * 0.9999F;
    const float scale = 1.0F / (end - start);
    return {scale, -scale * start};
}

} // namespace shadowmath

ShadowCascades::ShadowCascades()
{
    for (std::size_t i = 0; i < kLevels; ++i) {
        Level& level = m_levels[i];
        level.range = kLevelPresets[i].range;
        level.margin = kLevelPresets[i].margin;
        level.angleTolerance = kLevelPresets[i].angleTolerance;
        level.directionTolerance = kLevelPresets[i].directionTolerance;
        level.depthRange = i + 1 == kLevels ? kLastLevelDepthRange : kLevelDepthRange;
    }
    for (std::size_t i = 0; i < kCascades; ++i) {
        m_cascades[i].farDistance = kCascadeFar[i];
        m_cascades[i].resolution = kCascadeResolution[i];
    }
}

float ShadowCascades::levelFilter(std::size_t level)
{
    return kFirstLevelFilter * std::pow(kStagger, static_cast<float>(level));
}

void ShadowCascades::place(Level& level, std::size_t index, const glm::vec3& cameraPosition)
{
    level.centre = cameraPosition;
    level.placed = true;
    level.dirty = true;
    const float half = level.range * (1.0F + level.margin);
    const float depth = std::min(level.depthRange, kDepthRangeCap);
    level.frustum = shadowmath::fit(level.direction, level.centre, half, half, depth, true, kLevelResolution);
    level.shrink
        = (levelFilter(index) + 0.5F) / static_cast<float>(kLevelResolution) + level.margin / (1.0F + level.margin);
    level.innerShrink
        = index == 0 || index + 1 == kLevels ? level.shrink : std::max(1.0F - level.shrink - kInnerSplit, 0.0F);
}

void ShadowCascades::update(const ShadowCamera& camera, const glm::vec3& lightDirection)
{
    // The levels (0x82EA0D24..0x82EA1014).
    std::size_t waiting = 0;
    for (const Level& level : m_levels) {
        waiting += level.dirty && level.placed ? 1 : 0;
    }
    float moveScale = std::pow(kStagger, static_cast<float>(waiting));
    // 0x824896A0 is the runtime's arc cosine (inferred from its use).
    const float elevation = std::numbers::pi_v<float> * 0.5F - std::acos(std::min(std::abs(lightDirection.y), 1.0F));
    const float shadowLength = kAngleTestHeight / std::tan(elevation);
    for (std::size_t i = 0; i < kLevels; ++i) {
        Level& level = m_levels[i];
        const bool moved
            = !level.placed || glm::length(camera.position - level.centre) * moveScale > level.margin * level.range;
        const bool angled = std::abs(shadowLength - level.shadowLength) > level.angleTolerance * kAngleToleranceScale;
        const bool turned = static_cast<double>(glm::dot(lightDirection, level.direction))
            < 1.0 - level.directionTolerance * kDirectionToleranceScale;
        if (angled || turned) {
            level.shadowLength = shadowLength;
            level.direction = lightDirection;
        }
        if (moved || angled || turned || m_first) {
            place(level, i, camera.position);
            ++waiting;
            moveScale *= kStagger;
            if (m_turn == i) {
                m_turn = (m_turn + 1) % kLevels;
            }
        }
    }
    if (waiting == 0) {
        place(m_levels[m_turn], m_turn, camera.position);
        m_turn = (m_turn + 1) % kLevels;
    }
    m_first = false;

    // The cascades (0x82EA1018..0x82EA15B0).
    if (glm::dot(m_cascadeDirection, lightDirection) < kCascadeTurn) {
        m_cascadeDirection = lightDirection;
    }
    float filter = kFirstCascadeFilter;
    const int smallest = *std::min_element(kCascadeResolution.begin(), kCascadeResolution.end());
    for (std::size_t i = 0; i < kCascades; ++i) {
        Cascade& cascade = m_cascades[i];
        cascade.nearDistance = i == 0 ? camera.nearPlane : m_cascades[i - 1].farDistance;
        const std::array<glm::vec3, 8> corners
            = shadowmath::sliceCorners(camera, cascade.nearDistance, cascade.farDistance);
        cascade.sphere = shadowmath::boundingSphere(corners);
        cascade.sphere.radius = roundUp(cascade.sphere.radius);
        cascade.reach = glm::length(cascade.sphere.centre + camera.forward * cascade.sphere.radius - camera.position);
        const float depth = std::min(kCascadeDepthStep * static_cast<float>(i + 1), kDepthRangeCap);
        cascade.frustum = shadowmath::fit(m_cascadeDirection, cascade.sphere.centre, cascade.sphere.radius,
            cascade.sphere.radius, depth, true, cascade.resolution);
        cascade.shrink = (filter + 1.5F) / static_cast<float>(smallest);
        cascade.filter = filter;
        filter *= kCascadeFilterStep;
    }
}

} // namespace openhorizon::render
