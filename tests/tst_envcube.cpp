// The dynamic environment cube: face schedule, face cameras and centre.

#include "support/GlContext.h"
#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>

#include "base/Error.h"
#include "game/RenderScenario.h"
#include "platform/Window.h"
#include "render/EnvironmentCube.h"
#include "render/EnvironmentCubeMath.h"
#include "render/FrameRenderer.h"
#include "render/Gl.h"
#include "vfs/Vfs.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

namespace {

namespace envcube = openhorizon::render::envcube;

bool near(float a, float b, float tolerance = 1e-4F)
{
    return std::abs(a - b) <= tolerance;
}

/// OpenGL's cube map addressing (OpenGL 4.5, table 8.19): the face a
/// direction selects and its s and t in 0..1.
struct Addressed {
    int face = 0;
    float s = 0.0F;
    float t = 0.0F;
};

Addressed address(const glm::vec3& d)
{
    const glm::vec3 m = glm::abs(d);
    float sc = 0.0F;
    float tc = 0.0F;
    float ma = 0.0F;
    int face = 0;
    if (m.x >= m.y && m.x >= m.z) {
        face = d.x >= 0.0F ? 0 : 1;
        sc = d.x >= 0.0F ? -d.z : d.z;
        tc = -d.y;
        ma = m.x;
    } else if (m.y >= m.z) {
        face = d.y >= 0.0F ? 2 : 3;
        sc = d.x;
        tc = d.y >= 0.0F ? d.z : -d.z;
        ma = m.y;
    } else {
        face = d.z >= 0.0F ? 4 : 5;
        sc = d.z >= 0.0F ? d.x : -d.x;
        tc = -d.y;
        ma = m.z;
    }
    return {face, (sc / ma + 1.0F) * 0.5F, (tc / ma + 1.0F) * 0.5F};
}

/// This renderer's world is the game's with z negated.
glm::vec3 fromGame(const glm::vec3& v)
{
    return {v.x, v.y, -v.z};
}

TEST_CASE("faces_per_frame_follow_env_map_frequency_scale", "[envcube]")
{
    // 6 x scale, truncated, within 1..6; no update at all at 0 or below.
    CHECK(envcube::facesPerFrame(1.0F) == 6);
    CHECK(envcube::facesPerFrame(0.99F) == 5);
    CHECK(envcube::facesPerFrame(0.5F) == 3);
    CHECK(envcube::facesPerFrame(0.1F) == 1);
    CHECK(envcube::facesPerFrame(3.0F) == 6);
    CHECK(envcube::facesPerFrame(0.0F) == 0);
    CHECK(envcube::facesPerFrame(-1.0F) == 0);
}

TEST_CASE("schedule_walks_the_face_table_and_builds_mips_once_complete", "[envcube]")
{
    envcube::FaceSchedule schedule;
    CHECK(!schedule.complete());
    // Half rate: three faces a frame from the table at 0x82253E38.
    envcube::FaceSchedule::Frame frame = schedule.next(0.5F);
    CHECK(frame.count == 3);
    CHECK(frame.faces[0] == 0);
    CHECK(frame.faces[1] == 4);
    CHECK(frame.faces[2] == 2);
    CHECK(!schedule.complete());
    CHECK(!schedule.mipsDue());
    frame = schedule.next(0.5F);
    CHECK(frame.count == 3);
    CHECK(frame.faces[0] == 1);
    CHECK(frame.faces[1] == 3);
    CHECK(frame.faces[2] == 5);
    CHECK(schedule.complete());
    CHECK(schedule.mipsDue());
    schedule.markMipsBuilt();
    CHECK(!schedule.mipsDue());
    // A face drawn since the mips were built makes them due again.
    frame = schedule.next(0.1F);
    CHECK(frame.count == 1);
    CHECK(frame.faces[0] == 0);
    CHECK(schedule.mipsDue());
    CHECK(schedule.cursor() == 1);
    // Full rate from there: all six, starting where the cursor stands.
    frame = schedule.next(1.0F);
    CHECK(frame.count == 6);
    CHECK(frame.faces[0] == 4);
    CHECK(frame.faces[5] == 0);
    CHECK(schedule.cursor() == 1);
    // Nothing is drawn, nor any bit changed, at a scale of 0.
    schedule.markMipsBuilt();
    frame = schedule.next(0.0F);
    CHECK(frame.count == 0);
    CHECK(!schedule.mipsDue());
    schedule.reset();
    CHECK(!schedule.complete());
}

TEST_CASE("centre_is_half_a_metre_above_the_car_origin", "[envcube]")
{
    const glm::mat4 body = glm::rotate(
        glm::translate(glm::mat4(1.0F), glm::vec3(10.0F, 2.0F, -30.0F)), 0.7F, glm::vec3(0.0F, 0.0F, 1.0F));
    const glm::vec3 c = envcube::centre(body);
    CHECK((near(c.x, 10.0F) && near(c.y, 2.5F) && near(c.z, -30.0F)));
}

TEST_CASE("face_cameras_put_each_direction_where_gl_addresses_it", "[envcube]")
{
    const glm::vec3 centre(123.0F, 45.0F, -678.0F);
    const glm::mat4 projection = envcube::faceProjection();
    int checked = 0;
    // Directions over the whole sphere, on and between the face centres.
    for (int i = 0; i < 24; ++i) {
        for (int j = 1; j < 12; ++j) {
            const float azimuth = static_cast<float>(i) * 0.2618F + 0.05F;
            const float elevation = -1.5F + static_cast<float>(j) * 0.25F;
            const glm::vec3 game(
                std::cos(elevation) * std::cos(azimuth), std::sin(elevation), std::cos(elevation) * std::sin(azimuth));
            const Addressed a = address(game);
            const glm::vec3 world = centre + fromGame(game) * 37.0F;
            const glm::vec4 clip = projection * envcube::faceView(a.face, centre) * glm::vec4(world, 1.0F);
            CHECK(clip.w > 0.0F);
            const glm::vec3 ndc = glm::vec3(clip) / clip.w;
            CHECK(near(ndc.x, 2.0F * a.s - 1.0F, 1e-3F));
            CHECK(near(ndc.y, 2.0F * a.t - 1.0F, 1e-3F));
            CHECK((ndc.z > 0.0F && ndc.z < 1.0F));
            // faceDirection() is the inverse of the addressing.
            const glm::vec3 back = envcube::faceDirection(a.face, a.s, a.t);
            CHECK(near(glm::dot(back, glm::normalize(game)), 1.0F, 1e-4F));
            ++checked;
        }
    }
    CHECK(checked == 24 * 11);
}

TEST_CASE("face_transforms_are_the_face_cameras_about_the_centre", "[envcube]")
{
    const glm::vec3 centre(-40.0F, 12.0F, 3000.0F);
    const glm::mat4 toCentre = glm::translate(glm::mat4(1.0F), -centre);
    for (int face = 0; face < 6; ++face) {
        const glm::mat4 whole = envcube::faceProjection() * envcube::faceView(face, centre);
        const glm::mat4 split = openhorizon::render::EnvironmentCube::faceTransform(face) * toCentre;
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                CHECK(near(whole[c][r], split[c][r], 1e-2F));
            }
        }
    }
}

TEST_CASE("face_depth_runs_from_near_to_far_reversed", "[envcube]")
{
    const glm::vec3 centre(0.0F);
    const glm::mat4 viewProjection = envcube::faceProjection() * envcube::faceView(0, centre);
    const glm::vec4 atNear = viewProjection * glm::vec4(envcube::kNear, 0.0F, 0.0F, 1.0F);
    const glm::vec4 atFar = viewProjection * glm::vec4(envcube::kFar, 0.0F, 0.0F, 1.0F);
    CHECK(near(atNear.z / atNear.w, 1.0F, 1e-5F));
    CHECK(near(atFar.z / atFar.w, 0.0F, 1e-5F));
}

TEST_CASE("face_cameras_keep_the_scene_winding", "[envcube]")
{
    // Same winding as the scene's camera, so the scene's culling state serves the faces.
    const glm::vec3 centre(5.0F, 1.0F, 5.0F);
    const std::array<glm::vec3, 6> towards{glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(-1.0F, 0.0F, 0.0F),
        glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F),
        glm::vec3(0.0F, 0.0F, -1.0F)};
    for (int face = 0; face < 6; ++face) {
        const glm::vec3 axis = fromGame(towards[static_cast<std::size_t>(face)]);
        openhorizon::render::Camera camera;
        camera.position = centre;
        camera.target = centre + axis;
        camera.up = std::abs(axis.y) > 0.5F ? glm::vec3(0.0F, 0.0F, 1.0F) : glm::vec3(0.0F, 1.0F, 0.0F);
        const glm::mat4 scene = camera.projection(1.0F) * camera.view();
        const glm::mat4 cube = envcube::faceProjection() * envcube::faceView(face, centre);
        // A small triangle ten metres along the axis.
        const glm::vec3 side = std::abs(axis.y) > 0.5F ? glm::vec3(1.0F, 0.0F, 0.0F) : glm::vec3(0.0F, 1.0F, 0.0F);
        const glm::vec3 other = glm::cross(axis, side);
        const std::array<glm::vec3, 3> triangle{
            centre + axis * 10.0F, centre + axis * 10.0F + side, centre + axis * 10.0F + other};
        const auto signedArea = [&](const glm::mat4& m) {
            std::array<glm::vec2, 3> p{};
            for (std::size_t k = 0; k < 3; ++k) {
                const glm::vec4 clip = m * glm::vec4(triangle[k], 1.0F);
                p[k] = glm::vec2(clip) / clip.w;
            }
            return (p[1].x - p[0].x) * (p[2].y - p[0].y) - (p[2].x - p[0].x) * (p[1].y - p[0].y);
        };
        const float a = signedArea(scene);
        const float b = signedArea(cube);
        CHECK(a != 0.0F);
        CHECK((a > 0.0F) == (b > 0.0F));
    }
}

TEST_CASE("sphere_map_round_trips_through_the_face_blit", "[envcube]")
{
    // Round trip through the shader's polynomial atan2 and acos, away from poles and seam.
    for (int i = 1; i < 32; ++i) {
        for (int j = 1; j < 16; ++j) {
            const float u = static_cast<float>(i) / 32.0F;
            const float v = static_cast<float>(j) / 16.0F;
            const glm::vec2 back = envcube::sphereCoordinates(envcube::sphereDirection(u, v));
            CHECK(near(back.x, u, 2e-4F));
            CHECK(near(back.y, v, 2e-4F));
        }
    }
    // Straight up is the top row, the seam faces -x, +x the middle.
    CHECK(near(envcube::sphereCoordinates(glm::vec3(0.0F, 1.0F, 0.0F)).y, 0.0F, 1e-3F));
    CHECK(near(envcube::sphereCoordinates(glm::vec3(0.0F, -1.0F, 0.0F)).y, 1.0F, 1e-3F));
    CHECK(near(envcube::sphereCoordinates(glm::vec3(1.0F, 0.0F, 0.0F)).x, 0.5F, 1e-4F));
    CHECK(near(envcube::sphereCoordinates(glm::vec3(0.0F, 0.0F, 1.0F)).x, 0.75F, 1e-4F));
    CHECK(near(envcube::sphereCoordinates(glm::vec3(0.0F, 0.0F, -1.0F)).x, 0.25F, 1e-4F));
    const glm::vec3 seam = envcube::sphereDirection(0.0F, 0.5F);
    CHECK(near(seam.x, -1.0F, 1e-5F));
}

TEST_CASE("polynomial_atan2_and_acos_stay_within_their_error", "[envcube]")
{
    // The HLSL expansions are good to about 1e-5 (atan) and 7e-5 (acos) radians.
    for (int i = 0; i <= 200; ++i) {
        const float angle = -3.1F + 6.2F * static_cast<float>(i) / 200.0F;
        const float x = std::cos(angle);
        const float y = std::sin(angle);
        CHECK(near(envcube::gameAtan2(y, x), std::atan2(y, x), 2e-5F));
        const float c = -1.0F + 2.0F * static_cast<float>(i) / 200.0F;
        CHECK(near(envcube::gameAcos(c), std::acos(c), 1e-4F));
    }
}

TEST_CASE("sphere_levels_halve_down_to_six_by_three", "[envcube]")
{
    CHECK(envcube::kSphereWidth >> (envcube::kSphereLevels - 1) == 6);
    CHECK(envcube::kSphereHeight >> (envcube::kSphereLevels - 1) == 3);
    CHECK(envcube::kSize >> (envcube::kLevels - 1) == 1);
}

TEST_CASE("render_scenario_reads_env_map_frequency_scale", "[envcube]")
{
    const openhorizon::RenderScenario set = openhorizon::RenderScenario::parse(R"(<DynamicRenderSettings>
  <FrameRate value="30"/>
  <EnvMapFrequencyScale value="0.5"/>
</DynamicRenderSettings>)");
    CHECK(set.envMapFrequencyScale.has_value());
    CHECK(near(set.envMapFrequencyScale.value_or(0.0F), 0.5F));
    const openhorizon::RenderScenario unset = openhorizon::RenderScenario::parse(
        "<DynamicRenderSettings><TilingScenario value=\"1\"/></DynamicRenderSettings>");
    CHECK(!unset.envMapFrequencyScale.has_value());
    bool threw = false;
    try {
        openhorizon::RenderScenario::parse("<Other/>");
    } catch (const openhorizon::DataError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("disc_in_game_scenario_draws_every_face_each_frame", "[envcube]")
{
    const char* disc = openhorizon::test::gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(disc);
    const openhorizon::RenderScenario scenario
        = openhorizon::RenderScenario::stacked(vfs, openhorizon::RenderScenario::kInGame);
    CHECK(scenario.envMapFrequencyScale.has_value());
    CHECK(envcube::facesPerFrame(scenario.envMapFrequencyScale.value_or(0.0F)) == 6);
}

/// Clears colour attachment 0 of the framebuffer bound for drawing.
void clearBound(const std::array<float, 4>& colour)
{
    GLint framebuffer = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
    glClearNamedFramebufferfv(static_cast<GLuint>(framebuffer), GL_COLOR, 0, colour.data());
}

TEST_CASE("mip_chain_keeps_each_face_where_it_was_drawn", "[envcube]")
{
    openhorizon::test::requireGlContext();
    // Each face its own colour (stored as square roots); the chain goes through the
    // lat-long map and back.
    constexpr std::array<glm::vec3, 6> kColours{glm::vec3(0.9F, 0.1F, 0.1F), glm::vec3(0.1F, 0.9F, 0.1F),
        glm::vec3(0.1F, 0.1F, 0.9F), glm::vec3(0.9F, 0.9F, 0.1F), glm::vec3(0.1F, 0.9F, 0.9F),
        glm::vec3(0.9F, 0.1F, 0.9F)};
    openhorizon::render::EnvironmentCube cube;
    int facesSeen = 0;
    const openhorizon::render::FrameParams frame;
    cube.update(
        glm::vec3(0.0F), 1.0F, glm::vec4(0.0F), frame, nullptr, [&](const openhorizon::render::FrameParams& face) {
        // The view's third row is the face's axis, z negated.
        const glm::vec3 axis(face.view[0][2], face.view[1][2], -face.view[2][2]);
        int index = 0;
        const std::array<glm::vec3, 6> axes{glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(-1.0F, 0.0F, 0.0F),
            glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F),
            glm::vec3(0.0F, 0.0F, -1.0F)};
        for (int i = 0; i < 6; ++i) {
            if (glm::dot(axis, axes[static_cast<std::size_t>(i)]) > 0.9F) {
                index = i;
            }
        }
        const glm::vec3& c = kColours[static_cast<std::size_t>(index)];
        const std::array<float, 4> colour{c.r, c.g, c.b, 1.0F};
        clearBound(colour);
        ++facesSeen;
    });
    CHECK(facesSeen == 6);
    CHECK(cube.mipBuilds() == 1);
    // Level 2 (64 texels): the centre texel of each face.
    constexpr int kLevel = 2;
    constexpr int kSize = envcube::kSize >> kLevel;
    std::vector<float> texels(static_cast<std::size_t>(kSize * kSize * 6 * 4));
    glGetTextureImage(
        cube.texture(), kLevel, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(texels.size() * sizeof(float)), texels.data());
    for (int face = 0; face < 6; ++face) {
        const std::size_t at = (static_cast<std::size_t>(face * kSize * kSize + (kSize / 2) * kSize + kSize / 2)) * 4;
        const glm::vec3 got(texels[at], texels[at + 1], texels[at + 2]);
        const glm::vec3& want = kColours[static_cast<std::size_t>(face)];
        std::fprintf(stderr, "face %d centre %.3f %.3f %.3f\n", face, got.r, got.g, got.b);
        // The nearest of the six colours is the face's own.
        for (int other = 0; other < 6; ++other) {
            CHECK((other == face
                || glm::length(got - want) < glm::length(got - kColours[static_cast<std::size_t>(other)])));
        }
        // At a pole u turns round within a texel, so the fetch mixes in the
        // neighbouring faces, as the game's shader does.
        if (face != 2 && face != 3) {
            CHECK((near(got.r, want.r, 0.02F) && near(got.g, want.g, 0.02F) && near(got.b, want.b, 0.02F)));
        }
    }
    // A second frame: all faces again, so the mips are rebuilt.
    cube.update(glm::vec3(0.0F), 1.0F, glm::vec4(0.0F), frame, nullptr, [&](const openhorizon::render::FrameParams&) {
        const std::array<float, 4> grey{0.5F, 0.5F, 0.5F, 1.0F};
        clearBound(grey);
    });
    CHECK(cube.mipBuilds() == 2);
    for (int level = 1; level < envcube::kLevels; ++level) {
        const int size = envcube::kSize >> level;
        std::vector<float> all(static_cast<std::size_t>(size * size * 6 * 4));
        glGetTextureImage(
            cube.texture(), level, GL_RGBA, GL_FLOAT, static_cast<GLsizei>(all.size() * sizeof(float)), all.data());
        float worst = 0.0F;
        for (std::size_t i = 0; i < all.size(); i += 4) {
            worst = std::max(worst, std::abs(all[i] - 0.5F));
        }
        CHECK(worst < 0.01F);
    }
}

} // namespace
