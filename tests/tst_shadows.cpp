// The sun shadow cascades' fitting and update rules, and the ShadowIntensity curve.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include "game/TimeOfDay.h"
#include "render/ShadowCascades.h"
#include "render/WorldRenderer.h"
#include "vfs/Vfs.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdlib>
#include <string>

namespace {

using openhorizon::render::ShadowCamera;
using openhorizon::render::ShadowCascades;
namespace shadowmath = openhorizon::render::shadowmath;

/// Colorado's ShadowLightDirection (TrackSettings.xml), used as written while
/// OverrideShadowLightDirection is set.
const glm::vec3 kColoradoLight(0.75F, -0.65F, 0.59F);

bool near(float a, float b, float tolerance = 1e-4F)
{
    return std::abs(a - b) <= tolerance;
}

ShadowCamera cameraAt(const glm::vec3& position)
{
    ShadowCamera camera;
    camera.position = position;
    camera.right = glm::vec3(1.0F, 0.0F, 0.0F);
    camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
    camera.forward = glm::vec3(0.0F, 0.0F, 1.0F);
    camera.nearPlane = 0.1F;
    camera.tanHalfHeight = std::tan(0.525F);
    camera.tanHalfWidth = camera.tanHalfHeight * 16.0F / 9.0F;
    return camera;
}

TEST_CASE("light_basis_follows_the_executable", "[shadows]")
{
    const glm::mat4 world = shadowmath::lightWorld(kColoradoLight, glm::vec3(10.0F, 20.0F, 30.0F));
    const glm::vec3 u(world[0]);
    const glm::vec3 v(world[1]);
    const glm::vec3 n(world[2]);
    // |n.x| > |n.y|: u = (n.z, 0, -n.x) / sqrt(n.x² + n.z²).
    const float length = std::sqrt(0.75F * 0.75F + 0.59F * 0.59F);
    CHECK(near(u.x, 0.59F / length));
    CHECK(near(u.y, 0.0F));
    CHECK(near(u.z, -0.75F / length));
    CHECK(near(glm::dot(u, n), 0.0F));
    CHECK(near(glm::length(v - glm::cross(n, u)), 0.0F));
    // The direction is kept as written, not normalised.
    CHECK(near(glm::length(n), glm::length(kColoradoLight)));
    CHECK((near(world[3].x, 10.0F) && near(world[3].y, 20.0F) && near(world[3].z, 30.0F)));
    // Mostly vertical light takes the other branch: u = (0, -n.z, n.y) / ...
    const glm::mat4 steep = shadowmath::lightWorld(glm::vec3(0.1F, -0.9F, 0.2F), glm::vec3(0.0F));
    CHECK(near(steep[0].x, 0.0F));
    CHECK(near(glm::dot(glm::vec3(steep[0]), glm::vec3(steep[2])), 0.0F));
}

TEST_CASE("ortho_maps_its_box_to_direct3d_clip_space", "[shadows]")
{
    const glm::mat4 p = shadowmath::orthoOffCenter(-2.0F, 6.0F, -3.0F, 5.0F, -10.0F, 30.0F);
    const glm::vec4 low = p * glm::vec4(-2.0F, -3.0F, -10.0F, 1.0F);
    const glm::vec4 high = p * glm::vec4(6.0F, 5.0F, 30.0F, 1.0F);
    CHECK((near(low.x, -1.0F) && near(low.y, -1.0F) && near(low.z, 0.0F)));
    CHECK((near(high.x, 1.0F) && near(high.y, 1.0F) && near(high.z, 1.0F)));
}

TEST_CASE("snapping_puts_the_world_origin_on_a_texel_corner", "[shadows]")
{
    const glm::vec3 centre(1234.567F, 89.1F, -2345.678F);
    const int resolution = 512;
    const auto f = shadowmath::fit(kColoradoLight, centre, 26.25F, 26.25F, 1000.0F, true, resolution);
    const float texel = 2.0F / static_cast<float>(resolution);
    const glm::vec4 origin = f.viewProjection * glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
    for (int axis = 0; axis < 2; ++axis) {
        const float cells = origin[axis] / texel;
        CHECK(near(cells, std::round(cells), 2e-2F));
    }
    // 0x82497DB0 moves p to t - fmod(p, t) + p: up to one texel off for positive p,
    // one to two for negative p, as fmod keeps the sign.
    const auto unsnapped = shadowmath::fit(kColoradoLight, centre, 26.25F, 26.25F, 1000.0F, false, resolution);
    const glm::vec3 p = glm::mat3(unsnapped.viewProjection) * centre;
    const glm::vec4 projected = f.viewProjection * glm::vec4(centre, 1.0F);
    for (int axis = 0; axis < 2; ++axis) {
        const double shift = texel - std::fmod(static_cast<double>(p[axis]), static_cast<double>(texel));
        CHECK(near(projected[axis], static_cast<float>(-shift), 1e-4F));
    }
    CHECK(near(projected.z, 0.5F, 1e-3F));
}

TEST_CASE("box_corners_invert_the_projection_and_shrink", "[shadows]")
{
    const auto f = shadowmath::fit(kColoradoLight, glm::vec3(5.0F, 6.0F, 7.0F), 10.0F, 10.0F, 50.0F, false, 512);
    const auto corners = shadowmath::boxCorners(f, 0.0F);
    const glm::vec4 first = f.viewProjection * glm::vec4(corners[0], 1.0F);
    const glm::vec4 last = f.viewProjection * glm::vec4(corners[6], 1.0F);
    CHECK((near(first.x, 1.0F) && near(first.y, 1.0F) && near(first.z, 0.0F)));
    CHECK((near(last.x, -1.0F) && near(last.y, -1.0F) && near(last.z, 1.0F)));
    const auto shrunk = shadowmath::boxCorners(f, 0.25F);
    const glm::vec4 inner = f.viewProjection * glm::vec4(shrunk[0], 1.0F);
    CHECK((near(inner.x, 0.75F) && near(inner.y, 0.75F)));
}

TEST_CASE("sphere_is_around_the_bounding_box_centre", "[shadows]")
{
    const std::array<glm::vec3, 3> points{glm::vec3(0.0F), glm::vec3(4.0F, 0.0F, 0.0F), glm::vec3(0.0F, 2.0F, 0.0F)};
    const auto sphere = shadowmath::boundingSphere(points);
    CHECK((near(sphere.centre.x, 2.0F) && near(sphere.centre.y, 1.0F) && near(sphere.centre.z, 0.0F)));
    CHECK(near(sphere.radius, std::sqrt(5.0F)));
}

TEST_CASE("filter_taps_and_depth_fade", "[shadows]")
{
    const auto taps = shadowmath::filterTaps(0.01F);
    CHECK((near(taps[0].x, 0.01F) && near(taps[0].y, 0.01F) && near(taps[0].z, 0.005F) && near(taps[0].w, -0.005F)));
    CHECK((near(taps[1].x, -0.01F) && near(taps[1].y, -0.01F) && near(taps[1].z, -0.005F) && near(taps[1].w, 0.005F)));
    const glm::vec2 fade = shadowmath::depthFade(600.0F);
    CHECK(near(fade.x * 540.0F + fade.y, 0.0F, 1e-3F));
    CHECK(near(fade.x * 599.94F + fade.y, 1.0F, 1e-3F));
}

TEST_CASE("levels_are_placed_on_the_camera_and_shrunk", "[shadows]")
{
    ShadowCascades cascades;
    cascades.update(cameraAt(glm::vec3(100.0F, 5.0F, 200.0F)), kColoradoLight);
    const auto& levels = cascades.levels();
    for (const auto& level : levels) {
        CHECK(level.dirty);
        CHECK((near(level.centre.x, 100.0F) && near(level.centre.z, 200.0F)));
    }
    CHECK((near(levels[0].range, 25.0F) && near(levels[3].range, 600.0F)));
    const float shrink0 = (1.3F + 0.5F) / 512.0F + 0.05F / 1.05F;
    CHECK(near(levels[0].shrink, shrink0));
    CHECK(near(levels[0].innerShrink, shrink0));
    const float shrink1 = (1.3F * 0.75F + 0.5F) / 512.0F + 0.05F / 1.05F;
    CHECK(near(levels[1].shrink, shrink1));
    CHECK(near(levels[1].innerShrink, 0.4F - shrink1));
    CHECK(near(levels[3].innerShrink, levels[3].shrink));
    // The half extent is range × (1 + margin): the projection's x scale.
    CHECK(near(levels[2].frustum.projection[0][0], 1.0F / (200.0F * 1.02F)));
    CHECK(near(levels[3].frustum.projection[2][2], 1.0F / 2600.0F, 1e-7F));
}

TEST_CASE("levels_move_with_the_camera_past_their_margin", "[shadows]")
{
    ShadowCascades cascades;
    cascades.update(cameraAt(glm::vec3(0.0F)), kColoradoLight);
    for (std::size_t i = 0; i < ShadowCascades::kLevels; ++i) {
        cascades.markDrawn(i);
    }
    // Past level 0's 1.25 m; level 1's 2.5 m is compared with 2 m × 0.75
    // (one level waiting).
    cascades.update(cameraAt(glm::vec3(2.0F, 0.0F, 0.0F)), kColoradoLight);
    const auto& levels = cascades.levels();
    CHECK(levels[0].dirty);
    CHECK(near(levels[0].centre.x, 2.0F));
    CHECK(!levels[1].dirty);
    CHECK(!levels[2].dirty);
    CHECK(!levels[3].dirty);
}

TEST_CASE("idle_levels_are_refreshed_in_turn", "[shadows]")
{
    ShadowCascades cascades;
    cascades.update(cameraAt(glm::vec3(0.0F)), kColoradoLight);
    for (std::size_t i = 0; i < ShadowCascades::kLevels; ++i) {
        cascades.markDrawn(i);
    }
    for (std::size_t frame = 0; frame < ShadowCascades::kLevels; ++frame) {
        cascades.update(cameraAt(glm::vec3(0.0F)), kColoradoLight);
        std::size_t dirty = 0;
        for (std::size_t i = 0; i < ShadowCascades::kLevels; ++i) {
            dirty += cascades.levels()[i].dirty ? 1 : 0;
            if (cascades.levels()[i].dirty) {
                cascades.markDrawn(i);
            }
        }
        CHECK(dirty == std::size_t{1});
    }
}

TEST_CASE("cascades_cover_the_near_slices", "[shadows]")
{
    ShadowCascades cascades;
    const ShadowCamera camera = cameraAt(glm::vec3(10.0F, 2.0F, 10.0F));
    cascades.update(camera, kColoradoLight);
    const auto& c = cascades.cascades();
    CHECK((near(c[0].nearDistance, 0.1F) && near(c[0].farDistance, 4.0F)));
    CHECK((near(c[1].nearDistance, 4.0F) && near(c[1].farDistance, 15.0F)));
    CHECK((near(c[2].nearDistance, 15.0F) && near(c[2].farDistance, 40.0F)));
    CHECK(c[0].resolution == 1024);
    CHECK(c[2].resolution == 512);
    for (const auto& cascade : c) {
        CHECK(near(cascade.sphere.radius, std::round(cascade.sphere.radius)));
        const auto corners = shadowmath::sliceCorners(camera, cascade.nearDistance, cascade.farDistance);
        for (const glm::vec3& corner : corners) {
            CHECK(glm::length(corner - cascade.sphere.centre) <= cascade.sphere.radius + 1e-3F);
        }
        CHECK(near(cascade.reach,
            glm::length(cascade.sphere.centre + camera.forward * cascade.sphere.radius - camera.position), 1e-3F));
    }
    CHECK((near(c[0].filter, 2.0F) && near(c[1].filter, 0.7F) && near(c[2].filter, 0.245F)));
    CHECK(near(c[0].shrink, 3.5F / 512.0F));
    // Depth half ranges of 100, 200, 300 m.
    CHECK(near(c[1].frustum.projection[2][2], 1.0F / 400.0F, 1e-7F));
}

TEST_CASE("shadow_intensity_follows_the_time_of_day", "[shadows]")
{
    const char* disc = openhorizon::test::gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(disc);
    openhorizon::TimeOfDay time(openhorizon::TimeOfDayCurves::load(vfs, "colorado"));
    time.setSeconds(12.0F * 3600.0F);
    // Colorado's ShadowIntensity is 1 from 07:50 to 19:09.
    CHECK(near(time.lighting().shadowIntensity, 1.0F, 1e-3F));
    time.setSeconds(2.0F * 3600.0F);
    CHECK(time.lighting().shadowIntensity < 0.0F);
}

} // namespace
