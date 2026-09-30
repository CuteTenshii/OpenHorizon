// The realtime sky's cloud data: clouddefs.xml, the cloud curves with the
// parameter block's defaults, the texture scrolling and turning, the lowest
// sky height and the two domes, on synthetic data and, when FH1_GAME_DIR
// names an extracted disc, on the disc's files and Colorado's curves.

#include "TestHarness.h"

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

namespace {

using fh::render::CloudDefinitions;

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

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

TEST(cloud_definitions_parse_sets_and_transitions)
{
    const CloudDefinitions defs = CloudDefinitions::parse(kTwoSets);
    CHECK_EQ(defs.transitions.size(), std::size_t{1});
    CHECK_EQ(defs.transitions[0].layers.size(), std::size_t{2});
    CHECK(defs.transitions[0].layers[0].layer == CloudDefinitions::Layer::CloseFront);
    CHECK_NEAR(defs.transitions[0].layers[0].seconds, 5.0F, 1e-6F);
    CHECK(defs.transitions[0].layers[1].layer == CloudDefinitions::Layer::FarBack);
    CHECK_NEAR(defs.transitions[0].layers[1].seconds, 2.5F, 1e-6F);

    CHECK_EQ(defs.sets.size(), std::size_t{2});
    const CloudDefinitions::Set* base = defs.findSet("base");
    CHECK(base != nullptr);
    if (base != nullptr) {
        CHECK(!base->overcast);
        CHECK(base->valid());
        CHECK_EQ(base->texture(CloudDefinitions::Layer::CloseFront), 0);
        CHECK_EQ(base->texture(CloudDefinitions::Layer::CloseBack), 1);
        CHECK_EQ(base->texture(CloudDefinitions::Layer::FarFront), 2);
        CHECK_EQ(base->texture(CloudDefinitions::Layer::FarBack), 3);
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

TEST(cloud_definitions_reject_bad_documents)
{
    CHECK_THROWS(CloudDefinitions::parse("<other/>"));
    CHECK_THROWS(CloudDefinitions::parse("<cloudDefinitions><cloudTransition name=\"x\">"
                                         "<layer name=\"midCloud\" time=\"1\"/></cloudTransition></cloudDefinitions>"));
    CHECK_THROWS(CloudDefinitions::parse("not xml <"));
}

TEST(cloud_texture_paths_follow_the_format_strings)
{
    CHECK_EQ(fh::render::farCloudMapPath("Front", 0), std::string("media/realtimesky/FarCloudMapFront0.xds"));
    CHECK_EQ(fh::render::farCloudMaskPath(), std::string("media/realtimesky/FarCloudMask.xds"));
    CHECK_EQ(fh::render::closeCloudAlphaDensityPath("FrontSmall", 3),
        std::string("media/realtimesky/CloseCloudAlphaDensityFrontSmall3.xds"));
    CHECK_EQ(fh::render::closeCloudTopBottomAmbientPath("Front", 0),
        std::string("media/realtimesky/CloseCloudTopBottomAmbientFront0.xds"));
    CHECK_EQ(fh::render::closeCloudAmbientPath("Back", 1), std::string("media/realtimesky/CloseCloudAmbientBack1.xds"));
}

TEST(cloud_parameters_keep_block_defaults_for_missing_curves)
{
    const fh::render::CloudParameters p
        = fh::render::sampleCloudParameters([](std::string_view) -> std::optional<glm::vec3> { return std::nullopt; });
    // 0x82655D30.
    CHECK_NEAR(p.farFront.speedX, 9.0F, 1e-6F);
    CHECK_NEAR(p.farBack.speedZ, 9.0F, 1e-6F);
    CHECK_NEAR(p.farFront.coverage, 1.0F, 1e-6F);
    CHECK_NEAR(p.farFront.denseColour.g, 0.49F, 1e-6F);
    CHECK_NEAR(p.farBack.thinColour.b, 0.64F, 1e-6F);
    CHECK_NEAR(p.farFront.centreAlpha, 0.0F, 1e-6F);
    CHECK_NEAR(p.farBack.alphaPower, 5.0F, 1e-6F);
    // 0x82655838.
    CHECK_NEAR(p.close.thinColour.r, 0.5F, 1e-6F);
    CHECK_NEAR(p.close.thinColourBack.g, 0.5F, 1e-6F);
    CHECK_NEAR(p.close.denseColour.r, 0.0F, 1e-6F);
    CHECK_NEAR(p.close.denseColourBackIntensity, 1.0F, 1e-6F);
    CHECK_NEAR(p.close.topAlpha, 1.0F, 1e-6F);
    CHECK_NEAR(p.close.bottomAlpha, 0.0F, 1e-6F);
    CHECK_NEAR(p.close.alphaCurve, 13.01F, 1e-5F);
    CHECK_NEAR(p.close.frontRevs, 0.0F, 1e-6F);
    CHECK_NEAR(p.close.backRevs, 4.0F, 1e-6F);
    CHECK_NEAR(p.close.uvScales.x, 1.0F, 1e-6F);
}

TEST(cloud_parameters_take_the_curves_found)
{
    const std::map<std::string, glm::vec3> curves{
        {"FarCloudSpeedX", glm::vec3(3.0F, 7.0F, 8.0F)},
        {"FarCloudBackCoverage", glm::vec3(0.25F, 0.0F, 0.0F)},
        {"FarCloudDenseColour", glm::vec3(0.1F, 0.2F, 0.3F)},
        {"CloseCloudTopColour", glm::vec3(1.0F, 2.0F, 3.0F)},
        {"CloseCloudFrontRevs", glm::vec3(12.0F, 0.0F, 0.0F)},
    };
    const fh::render::CloudParameters p
        = fh::render::sampleCloudParameters([&](std::string_view name) -> std::optional<glm::vec3> {
        const auto it = curves.find(std::string(name));
        return it == curves.end() ? std::nullopt : std::optional<glm::vec3>(it->second);
    });
    // A scalar takes the first value only.
    CHECK_NEAR(p.farFront.speedX, 3.0F, 1e-6F);
    CHECK_NEAR(p.farFront.speedZ, 9.0F, 1e-6F);
    CHECK_NEAR(p.farBack.coverage, 0.25F, 1e-6F);
    CHECK_NEAR(p.farFront.coverage, 1.0F, 1e-6F);
    CHECK_NEAR(p.farFront.denseColour.b, 0.3F, 1e-6F);
    CHECK_NEAR(p.farBack.denseColour.b, 0.49F, 1e-6F);
    CHECK_NEAR(p.close.topColour.g, 2.0F, 1e-6F);
    CHECK_NEAR(p.close.frontRevs, 12.0F, 1e-6F);
}

TEST(cloud_motion_scrolls_per_hour_and_wraps)
{
    fh::render::CloudParameters p;
    p.farFront.speedX = 36.0F;
    p.farFront.speedZ = -18.0F;
    p.farBack.speedX = 0.0F;
    p.farBack.speedZ = 0.0F;
    p.close.frontRevs = 360.0F;
    p.close.backRevs = 0.0F;
    fh::render::CloudMotion motion;
    CHECK_NEAR(motion.closeFront, 0.5F, 1e-6F);
    CHECK_NEAR(motion.closeBack, 0.0F, 1e-6F);
    // 10 s: 36 per hour is 0.1 of the texture along u, -0.05 along v.
    motion.advance(p, 10.0F);
    CHECK_NEAR(motion.farFront.mask1.x, 0.1F, 1e-5F);
    CHECK_NEAR(motion.farFront.mask2.x, 0.1F, 1e-5F);
    CHECK_NEAR(motion.farFront.mask1.y, 0.95F, 1e-5F);
    // The map moves at 0.4 of the masks' rate.
    CHECK_NEAR(motion.farFront.map.x, 0.04F, 1e-5F);
    CHECK_NEAR(motion.farFront.map.y, 0.98F, 1e-5F);
    CHECK_NEAR(motion.farBack.mask1.x, 0.0F, 1e-6F);
    // 360 turns an hour is one turn per 10 s: back where it started.
    CHECK_NEAR(motion.closeFront, 0.5F, 1e-4F);
    motion.advance(p, 5.0F);
    CHECK_NEAR(motion.closeFront, 1.0F, 1e-4F);
    motion.advance(p, 2.5F);
    CHECK_NEAR(motion.closeFront, 0.25F, 1e-4F);
}

TEST(cloud_min_sky_height)
{
    const float expected = 300.0F * std::sin(std::atan2(-1000.0F, 15000.0F)) + 1.0F;
    // Starts above -1000 m are clamped to -1000 m.
    CHECK_NEAR(fh::render::cloudMinSkyHeight(0.0F), expected, 1e-4F);
    CHECK_NEAR(fh::render::cloudMinSkyHeight(500.0F), expected, 1e-4F);
    const float lower = 300.0F * std::sin(std::atan2(-4000.0F, 15000.0F)) + 1.0F;
    CHECK_NEAR(fh::render::cloudMinSkyHeight(-4000.0F), lower, 1e-4F);
}

TEST(far_cloud_dome_shape)
{
    const auto dome = fh::render::buildFarCloudDome();
    CHECK_EQ(dome.vertices.size(), std::size_t{1501});
    CHECK_EQ(dome.indices.size(), std::size_t{8700});
    CHECK(std::all_of(dome.indices.begin(), dome.indices.end(), [&](std::uint32_t i) { return i < 1501; }));
    // Every vertex lies on the 10 km sphere 9.7 km below the camera, except
    // the top, which sits 300 m up.
    for (std::size_t i = 0; i < 1500; ++i) {
        const glm::vec3 p = dome.vertices[i].position;
        CHECK_NEAR(glm::length(p + glm::vec3(0.0F, 9700.0F, 0.0F)), 10000.0F, 1.0F);
    }
    CHECK_NEAR(dome.vertices[1500].position.y, 300.0F, 1e-4F);
    // The lowest ring is seen 6 degrees below the horizon.
    const glm::vec3 low = dome.vertices[0].position;
    const float angle = std::atan2(low.y, std::sqrt(low.x * low.x + low.z * low.z));
    CHECK_NEAR(angle, -6.0F * std::numbers::pi_v<float> / 180.0F, 1e-3F);
    // Ring weights run from 1 at the bottom down by fifteenths.
    CHECK_NEAR(dome.vertices[0].texcoord.z, 1.0F, 1e-6F);
    CHECK_NEAR(dome.vertices[1400].texcoord.z, 1.0F / 15.0F, 1e-6F);
    CHECK_NEAR(dome.vertices[0].texcoord.y, 0.5F, 1e-6F);
}

TEST(close_cloud_dome_shape)
{
    const auto dome = fh::render::buildCloseCloudDome();
    CHECK_EQ(dome.vertices.size(), std::size_t{1617});
    CHECK_EQ(dome.indices.size(), std::size_t{9300});
    CHECK(std::all_of(dome.indices.begin(), dome.indices.end(), [&](std::uint32_t i) { return i < 1617; }));
    for (const auto& v : dome.vertices) {
        CHECK_NEAR(glm::length(glm::vec3(v.position)), 300.0F, 1e-2F);
        // The normal points inwards.
        CHECK_NEAR(glm::dot(glm::vec3(v.normal), glm::vec3(v.position)), -300.0F, 1e-2F);
    }
    // The first ring is 6 degrees below the horizon with height 0; each
    // ring ends with a copy of its first vertex at u = 1.
    CHECK_NEAR(dome.vertices[0].position.y, -300.0F * std::sin(6.0F * std::numbers::pi_v<float> / 180.0F), 1e-2F);
    CHECK_NEAR(dome.vertices[0].position.w, 0.0F, 1e-6F);
    CHECK_NEAR(dome.vertices[100].normal.w, 1.0F, 1e-6F);
    CHECK_NEAR(dome.vertices[100].position.x, dome.vertices[0].position.x, 1e-4F);
    CHECK_NEAR(dome.vertices[101].position.w, 1.0F / 15.0F, 1e-6F);
    CHECK_NEAR(dome.vertices[1616].position.y, 300.0F, 1e-4F);
    CHECK_NEAR(dome.vertices[1616].normal.w, 0.5F, 1e-6F);
}

TEST(disc_cloud_definitions)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const CloudDefinitions defs = CloudDefinitions::load(vfs);
    CHECK_EQ(defs.sets.size(), std::size_t{1});
    CHECK_EQ(defs.transitions.size(), std::size_t{1});
    const CloudDefinitions::Set* base = defs.findSet("base");
    CHECK(base != nullptr);
    if (base == nullptr) {
        return;
    }
    CHECK(base->valid());
    CHECK(!base->overcast);
    CHECK(std::all_of(base->textures.begin(), base->textures.end(), [](int i) { return i == 0; }));
    CHECK_EQ(defs.transitions[0].layers.size(), std::size_t{4});
    for (const auto& layer : defs.transitions[0].layers) {
        CHECK_NEAR(layer.seconds, 5.0F, 1e-6F);
    }
    // Every texture the main view's clouds use is on the disc.
    CHECK(vfs.exists(fh::render::farCloudMaskPath()));
    CHECK(vfs.exists(fh::render::farCloudMapPath("Front", 0)));
    CHECK(vfs.exists(fh::render::closeCloudAlphaDensityPath("Front", 0)));
    CHECK(vfs.exists(fh::render::closeCloudTopBottomAmbientPath("Front", 0)));
    CHECK(vfs.exists(fh::render::closeCloudAmbientPath("Front", 0)));
}

TEST(disc_colorado_cloud_curves)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const fh::TimeOfDayCurves curves = fh::TimeOfDayCurves::load(vfs, "colorado");
    const float noon = 12.0F * 60.0F;
    const fh::render::CloudParameters p
        = fh::render::sampleCloudParameters([&](std::string_view name) -> std::optional<glm::vec3> {
        if (!curves.has(name)) {
            return std::nullopt;
        }
        return curves.sample(name, noon);
    });
    // Colorado has no keys for these, so the block's defaults stand.
    CHECK(!curves.has("FarCloudSpeedX"));
    CHECK_NEAR(p.farFront.speedX, 9.0F, 1e-6F);
    CHECK(!curves.has("FarCloudBackAlphaPower"));
    CHECK_NEAR(p.farBack.alphaPower, 5.0F, 1e-6F);
    CHECK(!curves.has("CloseCloudDenseColourBackIntensity"));
    CHECK_NEAR(p.close.denseColourBackIntensity, 1.0F, 1e-6F);
    // And keys for these.
    CHECK(curves.has("CloseCloudBackRevs"));
    CHECK_NEAR(p.close.backRevs, curves.sampleScalar("CloseCloudBackRevs", noon), 1e-6F);
    CHECK(curves.has("FarCloudAlphaPower"));
    CHECK_NEAR(p.farFront.alphaPower, curves.sampleScalar("FarCloudAlphaPower", noon), 1e-6F);
    CHECK(curves.has("CloseCloudTopColour"));
    CHECK_NEAR(p.close.topColour.b, curves.sample("CloseCloudTopColour", noon).z, 1e-6F);
}

/// One hidden offscreen context for the draw test, or null when this
/// machine has none (the test then skips).
fh::Window* window()
{
    static std::unique_ptr<fh::Window> instance = [] {
        fh::WindowOptions options;
        options.width = 64;
        options.height = 64;
        options.hidden = true;
        options.vsync = false;
        try {
            return std::make_unique<fh::Window>(options);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "skipped: no OpenGL context (%s)\n", e.what());
            return std::unique_ptr<fh::Window>();
        }
    }();
    return instance.get();
}

/// Draws Colorado's noon clouds into a 1920x1080 target cleared to the far
/// plane, looking up over the horizon: the shaders must link, the layers
/// must cover part of the view, and the GPU time is reported.
TEST(disc_clouds_draw)
{
    const char* dir = gameDir();
    if (!dir || window() == nullptr) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const CloudDefinitions defs = CloudDefinitions::load(vfs);
    fh::render::Clouds clouds(vfs, defs.sets.front());

    fh::TimeOfDay time(fh::TimeOfDayCurves::load(vfs, "colorado"));
    time.setSeconds(12.0F * 3600.0F);
    const fh::TimeOfDayCurves curves = fh::TimeOfDayCurves::load(vfs, "colorado");
    const fh::render::CloudParameters parameters
        = fh::render::sampleCloudParameters([&](std::string_view name) -> std::optional<glm::vec3> {
        if (!curves.has(name)) {
            return std::nullopt;
        }
        return curves.sample(name, time.minutes());
    });
    const fh::render::CloudMotion motion;

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

    fh::render::Camera camera;
    camera.position = glm::vec3(0.0F, 2.0F, 0.0F);
    camera.target = glm::vec3(0.0F, 2.3F, 1.0F);
    fh::render::FrameParams frame;
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

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
