// The realtime sky's cloud data: clouddefs.xml, the cloud curves with the
// parameter block's defaults, the texture scrolling and turning, the lowest
// sky height and the two domes, on synthetic data and, when FH1_GAME_DIR
// names an extracted disc, on the disc's files and Colorado's curves.

#include "support/GlContext.h"
#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "game/TimeOfDay.h"
#include "platform/Window.h"
#include "render/CloudDefs.h"
#include "render/Clouds.h"
#include "render/FrameRenderer.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

using openhorizon::test::gameDir;

namespace {

using openhorizon::render::CloudDefinitions;

constexpr const char* kTwoSets = R"(<?xml version="1.0" encoding="utf-8"?>
<cloudDefinitions>
    <cloudTransition name="base">
        <layer name="closeCloudFront" time="5.0"/>
        <layer name="farCloudBack" time="2.5"/>
    </cloudTransition>
    <cloudSet name="base" overcast="false" closeCloudFront="0" closeCloudBack="1" farCloudFront="2" farCloudBack="3"/>
    <cloudSet name="grey" overcast="true" closeCloudFront="0" closeCloudBack="-1" farCloudFront="0" farCloudBack="0"/>
</cloudDefinitions>
)";

TEST_CASE("cloud_definitions_parse_sets_and_transitions", "[clouds]")
{
    const CloudDefinitions defs = CloudDefinitions::parse(kTwoSets);
    CHECK(defs.transitions.size() == std::size_t{1});
    CHECK(defs.transitions[0].layers.size() == std::size_t{2});
    CHECK(defs.transitions[0].layers[0].layer == CloudDefinitions::Layer::CloseFront);
    CHECK_THAT(defs.transitions[0].layers[0].seconds, WithinAbs(5.0F, 1e-6F));
    CHECK(defs.transitions[0].layers[1].layer == CloudDefinitions::Layer::FarBack);
    CHECK_THAT(defs.transitions[0].layers[1].seconds, WithinAbs(2.5F, 1e-6F));

    CHECK(defs.sets.size() == std::size_t{2});
    const CloudDefinitions::Set* base = defs.findSet("base");
    CHECK(base != nullptr);
    if (base != nullptr) {
        CHECK(!base->overcast);
        CHECK(base->valid());
        CHECK(base->texture(CloudDefinitions::Layer::CloseFront) == 0);
        CHECK(base->texture(CloudDefinitions::Layer::CloseBack) == 1);
        CHECK(base->texture(CloudDefinitions::Layer::FarFront) == 2);
        CHECK(base->texture(CloudDefinitions::Layer::FarBack) == 3);
    }
    const CloudDefinitions::Set* grey = defs.findSet("grey");
    CHECK(grey != nullptr);
    if (grey != nullptr) {
        CHECK(grey->overcast);
        // The reader marks a set with any negative index invalid.
        CHECK(!grey->valid());
    }
    CHECK(defs.findSet("missing") == nullptr);
}

TEST_CASE("cloud_definitions_reject_bad_documents", "[clouds]")
{
    CHECK_THROWS_AS(CloudDefinitions::parse("<other/>"), std::exception);
    CHECK_THROWS_AS(
        CloudDefinitions::parse("<cloudDefinitions><cloudTransition name=\"x\">"
                                "<layer name=\"midCloud\" time=\"1\"/></cloudTransition></cloudDefinitions>"),
        std::exception);
    CHECK_THROWS_AS(CloudDefinitions::parse("not xml <"), std::exception);
}

TEST_CASE("cloud_texture_paths_follow_the_format_strings", "[clouds]")
{
    CHECK(openhorizon::render::farCloudMapPath("Front", 0) == std::string("media/realtimesky/FarCloudMapFront0.xds"));
    CHECK(openhorizon::render::farCloudMaskPath() == std::string("media/realtimesky/FarCloudMask.xds"));
    CHECK(openhorizon::render::closeCloudAlphaDensityPath("FrontSmall", 3)
        == std::string("media/realtimesky/CloseCloudAlphaDensityFrontSmall3.xds"));
    CHECK(openhorizon::render::closeCloudTopBottomAmbientPath("Front", 0)
        == std::string("media/realtimesky/CloseCloudTopBottomAmbientFront0.xds"));
    CHECK(openhorizon::render::closeCloudAmbientPath("Back", 1)
        == std::string("media/realtimesky/CloseCloudAmbientBack1.xds"));
}

TEST_CASE("cloud_parameters_keep_block_defaults_for_missing_curves", "[clouds]")
{
    const openhorizon::render::CloudParameters p = openhorizon::render::sampleCloudParameters(
        [](std::string_view) -> std::optional<glm::vec3> { return std::nullopt; });
    // 0x82655D30.
    CHECK_THAT(p.farFront.speedX, WithinAbs(9.0F, 1e-6F));
    CHECK_THAT(p.farBack.speedZ, WithinAbs(9.0F, 1e-6F));
    CHECK_THAT(p.farFront.coverage, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(p.farFront.denseColour.g, WithinAbs(0.49F, 1e-6F));
    CHECK_THAT(p.farBack.thinColour.b, WithinAbs(0.64F, 1e-6F));
    CHECK_THAT(p.farFront.centreAlpha, WithinAbs(0.0F, 1e-6F));
    CHECK_THAT(p.farBack.alphaPower, WithinAbs(5.0F, 1e-6F));
    // 0x82655838.
    CHECK_THAT(p.close.thinColour.r, WithinAbs(0.5F, 1e-6F));
    CHECK_THAT(p.close.thinColourBack.g, WithinAbs(0.5F, 1e-6F));
    CHECK_THAT(p.close.denseColour.r, WithinAbs(0.0F, 1e-6F));
    CHECK_THAT(p.close.denseColourBackIntensity, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(p.close.topAlpha, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(p.close.bottomAlpha, WithinAbs(0.0F, 1e-6F));
    CHECK_THAT(p.close.alphaCurve, WithinAbs(13.01F, 1e-5F));
    CHECK_THAT(p.close.frontRevs, WithinAbs(0.0F, 1e-6F));
    CHECK_THAT(p.close.backRevs, WithinAbs(4.0F, 1e-6F));
    CHECK_THAT(p.close.uvScales.x, WithinAbs(1.0F, 1e-6F));
}

TEST_CASE("cloud_parameters_take_the_curves_found", "[clouds]")
{
    const std::map<std::string, glm::vec3> curves{
        {"FarCloudSpeedX", glm::vec3(3.0F, 7.0F, 8.0F)},
        {"FarCloudBackCoverage", glm::vec3(0.25F, 0.0F, 0.0F)},
        {"FarCloudDenseColour", glm::vec3(0.1F, 0.2F, 0.3F)},
        {"CloseCloudTopColour", glm::vec3(1.0F, 2.0F, 3.0F)},
        {"CloseCloudFrontRevs", glm::vec3(12.0F, 0.0F, 0.0F)},
    };
    const openhorizon::render::CloudParameters p
        = openhorizon::render::sampleCloudParameters([&](std::string_view name) -> std::optional<glm::vec3> {
        const auto it = curves.find(std::string(name));
        return it == curves.end() ? std::nullopt : std::optional<glm::vec3>(it->second);
    });
    // A scalar takes the first value only.
    CHECK_THAT(p.farFront.speedX, WithinAbs(3.0F, 1e-6F));
    CHECK_THAT(p.farFront.speedZ, WithinAbs(9.0F, 1e-6F));
    CHECK_THAT(p.farBack.coverage, WithinAbs(0.25F, 1e-6F));
    CHECK_THAT(p.farFront.coverage, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(p.farFront.denseColour.b, WithinAbs(0.3F, 1e-6F));
    CHECK_THAT(p.farBack.denseColour.b, WithinAbs(0.49F, 1e-6F));
    CHECK_THAT(p.close.topColour.g, WithinAbs(2.0F, 1e-6F));
    CHECK_THAT(p.close.frontRevs, WithinAbs(12.0F, 1e-6F));
}

TEST_CASE("cloud_motion_scrolls_per_hour_and_wraps", "[clouds]")
{
    openhorizon::render::CloudParameters p;
    p.farFront.speedX = 36.0F;
    p.farFront.speedZ = -18.0F;
    p.farBack.speedX = 0.0F;
    p.farBack.speedZ = 0.0F;
    p.close.frontRevs = 360.0F;
    p.close.backRevs = 0.0F;
    openhorizon::render::CloudMotion motion;
    CHECK_THAT(motion.closeFront, WithinAbs(0.5F, 1e-6F));
    CHECK_THAT(motion.closeBack, WithinAbs(0.0F, 1e-6F));
    // 10 s: 36 per hour is 0.1 of the texture along u, -0.05 along v.
    motion.advance(p, 10.0F);
    CHECK_THAT(motion.farFront.mask1.x, WithinAbs(0.1F, 1e-5F));
    CHECK_THAT(motion.farFront.mask2.x, WithinAbs(0.1F, 1e-5F));
    CHECK_THAT(motion.farFront.mask1.y, WithinAbs(0.95F, 1e-5F));
    // The map moves at 0.4 of the masks' rate.
    CHECK_THAT(motion.farFront.map.x, WithinAbs(0.04F, 1e-5F));
    CHECK_THAT(motion.farFront.map.y, WithinAbs(0.98F, 1e-5F));
    CHECK_THAT(motion.farBack.mask1.x, WithinAbs(0.0F, 1e-6F));
    // 360 turns an hour is one turn per 10 s: back where it started.
    CHECK_THAT(motion.closeFront, WithinAbs(0.5F, 1e-4F));
    motion.advance(p, 5.0F);
    CHECK_THAT(motion.closeFront, WithinAbs(1.0F, 1e-4F));
    motion.advance(p, 2.5F);
    CHECK_THAT(motion.closeFront, WithinAbs(0.25F, 1e-4F));
}

TEST_CASE("cloud_min_sky_height", "[clouds]")
{
    const float expected = 300.0F * std::sin(std::atan2(-1000.0F, 15000.0F)) + 1.0F;
    // Starts above -1000 m are clamped to -1000 m.
    CHECK_THAT(openhorizon::render::cloudMinSkyHeight(0.0F), WithinAbs(expected, 1e-4F));
    CHECK_THAT(openhorizon::render::cloudMinSkyHeight(500.0F), WithinAbs(expected, 1e-4F));
    const float lower = 300.0F * std::sin(std::atan2(-4000.0F, 15000.0F)) + 1.0F;
    CHECK_THAT(openhorizon::render::cloudMinSkyHeight(-4000.0F), WithinAbs(lower, 1e-4F));
}

TEST_CASE("far_cloud_dome_shape", "[clouds]")
{
    const auto dome = openhorizon::render::buildFarCloudDome();
    CHECK(dome.vertices.size() == std::size_t{1501});
    CHECK(dome.indices.size() == std::size_t{8700});
    CHECK(std::all_of(dome.indices.begin(), dome.indices.end(), [&](std::uint32_t i) { return i < 1501; }));
    // Every vertex lies on the 10 km sphere 9.7 km below the camera, except
    // the top, which sits 300 m up.
    for (std::size_t i = 0; i < 1500; ++i) {
        const glm::vec3 p = dome.vertices[i].position;
        CHECK_THAT(glm::length(p + glm::vec3(0.0F, 9700.0F, 0.0F)), WithinAbs(10000.0F, 1.0F));
    }
    CHECK_THAT(dome.vertices[1500].position.y, WithinAbs(300.0F, 1e-4F));
    // The lowest ring is seen 6 degrees below the horizon.
    const glm::vec3 low = dome.vertices[0].position;
    const float angle = std::atan2(low.y, std::sqrt(low.x * low.x + low.z * low.z));
    CHECK_THAT(angle, WithinAbs(-6.0F * std::numbers::pi_v<float> / 180.0F, 1e-3F));
    // Ring weights run from 1 at the bottom down by fifteenths.
    CHECK_THAT(dome.vertices[0].texcoord.z, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(dome.vertices[1400].texcoord.z, WithinAbs(1.0F / 15.0F, 1e-6F));
    CHECK_THAT(dome.vertices[0].texcoord.y, WithinAbs(0.5F, 1e-6F));
}

TEST_CASE("close_cloud_dome_shape", "[clouds]")
{
    const auto dome = openhorizon::render::buildCloseCloudDome();
    CHECK(dome.vertices.size() == std::size_t{1617});
    CHECK(dome.indices.size() == std::size_t{9300});
    CHECK(std::all_of(dome.indices.begin(), dome.indices.end(), [&](std::uint32_t i) { return i < 1617; }));
    for (const auto& v : dome.vertices) {
        CHECK_THAT(glm::length(glm::vec3(v.position)), WithinAbs(300.0F, 1e-2F));
        // The normal points inwards.
        CHECK_THAT(glm::dot(glm::vec3(v.normal), glm::vec3(v.position)), WithinAbs(-300.0F, 1e-2F));
    }
    // The first ring is 6 degrees below the horizon with height 0; each
    // ring ends with a copy of its first vertex at u = 1.
    CHECK_THAT(
        dome.vertices[0].position.y, WithinAbs(-300.0F * std::sin(6.0F * std::numbers::pi_v<float> / 180.0F), 1e-2F));
    CHECK_THAT(dome.vertices[0].position.w, WithinAbs(0.0F, 1e-6F));
    CHECK_THAT(dome.vertices[100].normal.w, WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(dome.vertices[100].position.x, WithinAbs(dome.vertices[0].position.x, 1e-4F));
    CHECK_THAT(dome.vertices[101].position.w, WithinAbs(1.0F / 15.0F, 1e-6F));
    CHECK_THAT(dome.vertices[1616].position.y, WithinAbs(300.0F, 1e-4F));
    CHECK_THAT(dome.vertices[1616].normal.w, WithinAbs(0.5F, 1e-6F));
}

TEST_CASE("disc_cloud_definitions", "[clouds]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const CloudDefinitions defs = CloudDefinitions::load(vfs);
    CHECK(defs.sets.size() == std::size_t{1});
    CHECK(defs.transitions.size() == std::size_t{1});
    const CloudDefinitions::Set* base = defs.findSet("base");
    CHECK(base != nullptr);
    if (base == nullptr) {
        return;
    }
    CHECK(base->valid());
    CHECK(!base->overcast);
    CHECK(std::all_of(base->textures.begin(), base->textures.end(), [](int i) { return i == 0; }));
    CHECK(defs.transitions[0].layers.size() == std::size_t{4});
    for (const auto& layer : defs.transitions[0].layers) {
        CHECK_THAT(layer.seconds, WithinAbs(5.0F, 1e-6F));
    }
    // Every texture the main view's clouds use is on the disc.
    CHECK(vfs.exists(openhorizon::render::farCloudMaskPath()));
    CHECK(vfs.exists(openhorizon::render::farCloudMapPath("Front", 0)));
    CHECK(vfs.exists(openhorizon::render::closeCloudAlphaDensityPath("Front", 0)));
    CHECK(vfs.exists(openhorizon::render::closeCloudTopBottomAmbientPath("Front", 0)));
    CHECK(vfs.exists(openhorizon::render::closeCloudAmbientPath("Front", 0)));
}

TEST_CASE("disc_colorado_cloud_curves", "[clouds]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const openhorizon::TimeOfDayCurves curves = openhorizon::TimeOfDayCurves::load(vfs, "colorado");
    const float noon = 12.0F * 60.0F;
    const openhorizon::render::CloudParameters p
        = openhorizon::render::sampleCloudParameters([&](std::string_view name) -> std::optional<glm::vec3> {
        if (!curves.has(name)) {
            return std::nullopt;
        }
        return curves.sample(name, noon);
    });
    // Colorado has no keys for these, so the block's defaults stand.
    CHECK(!curves.has("FarCloudSpeedX"));
    CHECK_THAT(p.farFront.speedX, WithinAbs(9.0F, 1e-6F));
    CHECK(!curves.has("FarCloudBackAlphaPower"));
    CHECK_THAT(p.farBack.alphaPower, WithinAbs(5.0F, 1e-6F));
    CHECK(!curves.has("CloseCloudDenseColourBackIntensity"));
    CHECK_THAT(p.close.denseColourBackIntensity, WithinAbs(1.0F, 1e-6F));
    // And keys for these.
    CHECK(curves.has("CloseCloudBackRevs"));
    CHECK_THAT(p.close.backRevs, WithinAbs(curves.sampleScalar("CloseCloudBackRevs", noon), 1e-6F));
    CHECK(curves.has("FarCloudAlphaPower"));
    CHECK_THAT(p.farFront.alphaPower, WithinAbs(curves.sampleScalar("FarCloudAlphaPower", noon), 1e-6F));
    CHECK(curves.has("CloseCloudTopColour"));
    CHECK_THAT(p.close.topColour.b, WithinAbs(curves.sample("CloseCloudTopColour", noon).z, 1e-6F));
}

/// Draws Colorado's noon clouds into a 1920x1080 target cleared to the far
/// plane, looking up over the horizon: the shaders must link, the layers
/// must cover part of the view, and the GPU time is reported.
TEST_CASE("disc_clouds_draw", "[clouds]")
{
    const char* dir = gameDir();
    openhorizon::test::requireGlContext();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const CloudDefinitions defs = CloudDefinitions::load(vfs);
    openhorizon::render::Clouds clouds(vfs, defs.sets.front());

    openhorizon::TimeOfDay time(openhorizon::TimeOfDayCurves::load(vfs, "colorado"));
    time.setSeconds(12.0F * 3600.0F);
    const openhorizon::TimeOfDayCurves curves = openhorizon::TimeOfDayCurves::load(vfs, "colorado");
    const openhorizon::render::CloudParameters parameters
        = openhorizon::render::sampleCloudParameters([&](std::string_view name) -> std::optional<glm::vec3> {
        if (!curves.has(name)) {
            return std::nullopt;
        }
        return curves.sample(name, time.minutes());
    });
    const openhorizon::render::CloudMotion motion;

    constexpr int kWidth = 1920;
    constexpr int kHeight = 1080;
    GLuint colour = 0;
    GLuint depth = 0;
    GLuint framebuffer = 0;
    glCreateTextures(GL_TEXTURE_2D, 1, &colour);
    glTextureStorage2D(colour, 1, GL_RGBA16F, kWidth, kHeight);
    glCreateTextures(GL_TEXTURE_2D, 1, &depth);
    glTextureStorage2D(depth, 1, GL_DEPTH_COMPONENT32F, kWidth, kHeight);
    glCreateFramebuffers(1, &framebuffer);
    glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, colour, 0);
    glNamedFramebufferTexture(framebuffer, GL_DEPTH_ATTACHMENT, depth, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, kWidth, kHeight);
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glEnable(GL_DEPTH_TEST);

    openhorizon::render::Camera camera;
    camera.position = glm::vec3(0.0F, 2.0F, 0.0F);
    camera.target = glm::vec3(0.0F, 2.3F, 1.0F);
    openhorizon::render::FrameParams frame;
    frame.view = camera.view();
    frame.projection = camera.projection(static_cast<float>(kWidth) / static_cast<float>(kHeight));
    frame.cameraPosition = camera.position;
    frame.lighting = time.lighting();

    const float clearColour[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    GLuint query = 0;
    glGenQueries(1, &query);
    constexpr int kRuns = 20;
    GLuint64 total = 0;
    for (int run = 0; run < kRuns; ++run) {
        glClearNamedFramebufferfv(framebuffer, GL_COLOR, 0, clearColour);
        glClearNamedFramebufferfi(framebuffer, GL_DEPTH_STENCIL, 0, 0.0F, 0);
        glBeginQuery(GL_TIME_ELAPSED, query);
        clouds.draw(frame, parameters, motion);
        glEndQuery(GL_TIME_ELAPSED);
        GLuint64 elapsed = 0;
        glGetQueryObjectui64v(query, GL_QUERY_RESULT, &elapsed);
        total += elapsed;
    }
    std::fprintf(
        stderr, "clouds: %.3f ms per draw at %dx%d\n", static_cast<double>(total) / kRuns / 1e6, kWidth, kHeight);
    glDeleteQueries(1, &query);

    std::vector<float> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_FLOAT, pixels.data());
    std::size_t covered = 0;
    bool finite = true;
    for (std::size_t i = 0; i < pixels.size(); i += 4) {
        finite = finite && std::isfinite(pixels[i]) && std::isfinite(pixels[i + 1]) && std::isfinite(pixels[i + 2]);
        covered += pixels[i + 3] > 0.0F ? 1 : 0;
    }
    CHECK(finite);
    CHECK(covered > pixels.size() / 4 / 20);
    std::fprintf(stderr, "clouds: %zu of %d pixels covered\n", covered, kWidth * kHeight);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &colour);
    glDeleteTextures(1, &depth);
}

} // namespace
