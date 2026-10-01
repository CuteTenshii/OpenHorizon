// Scenarios of the world as it is drawn and streamed: event props, forests,
// the track's animation clock, lights, shadows, post-processing, clouds,
// reflections, animated objects, crowds, water, flags, and chunks streaming
// in on time while driving the roads.

#include "scenarios/RoadFollower.h"
#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "car/CarPhysics.h"
#include "render/CrowdRenderer.h"
#include "render/EnvironmentCube.h"
#include "render/FrameStats.h"
#include "render/Gl.h"
#include "render/TrackPrograms.h"
#include "render/WorldRenderer.h"
#include "ui/Navigation.h"
#include "world/RenderMesh.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openhorizon::test {
namespace {

using Catch::Matchers::WithinAbs;

/// GPU milliseconds of the world pass (the frame statistics' DrawWorld
/// part), averaged over one second of `session` standing still.
double worldPassMs(Session& session)
{
    double seconds = 0.0;
    int frames = 0;
    for (float t = 0.0F; t < 1.0F; t += kStep) {
        session.run(kStep, {});
        seconds += session.game()
                       .frameStats()
                       .gpuFrameSeconds()[static_cast<std::size_t>(openhorizon::render::FrameStats::Part::DrawWorld)];
        ++frames;
    }
    return seconds * 1000.0 / frames;
}

// Free roam draws its own forests: the procedural tree sets behind an event
// block for route 0 (models_proc_clrd_trees_mountains_area4a[freeroam]_*
// stand around here), which were never loaded.
TEST_CASE_METHOD(GameScenario, "freeRoamDrawsItsForests", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(4330.0F, -250.0F);
    options.spawnHeading = 0.0F;
    options.startTime = 12.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(4.0F, {});
    const openhorizon::render::WorldRenderer* world = session.game().worldRenderer();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }
    CHECK(world->eventRoute() == 0);
    CHECK(world->eventChunksIncluded() > 0);
    std::fprintf(stderr, "forest: world pass GPU %.3f ms\n", worldPassMs(session));
    session.capture("forest_area4a");
}

// A race puts out its route's props (barriers, chevrons, gantries) and
// leaving it takes them away again, back to free roam's.
TEST_CASE_METHOD(GameScenario, "raceShowsItsRoutePropsUntilItEnds", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    openhorizon::Game& game = session.game();
    const openhorizon::render::WorldRenderer* world = game.worldRenderer();
    CHECK((world != nullptr && game.race() != nullptr));
    if (world == nullptr || game.race() == nullptr) {
        return;
    }
    // FR02 runs on route 96 (FESTIVAL_PLNS_006).
    CHECK(world->eventRoute() == game.race()->event().routeId);
    CHECK(world->eventRoute() == 96);
    const std::size_t raceChunks = world->eventChunksIncluded();
    CHECK(raceChunks > 0);
    std::fprintf(stderr, "race grid: %zu event chunks, world pass GPU %.3f ms\n", raceChunks, worldPassMs(session));
    session.capture("race_props_grid");

    session.press(openhorizon::Button::Start);
    const openhorizon::ui::PauseMenu* menu = game.pauseMenu();
    CHECK(menu != nullptr);
    if (menu == nullptr) {
        return;
    }
    CHECK(session.focusPauseItem(openhorizon::ui::PauseItem::QuitRace));
    session.press(openhorizon::Button::A);
    CHECK(menu->confirming());
    session.press(openhorizon::Button::A);
    session.run(2.0F, {});
    CHECK(game.race() == nullptr);
    CHECK(world->eventRoute() == 0);
    CHECK(world->eventChunksIncluded() != raceChunks);
    session.capture("race_props_after");
}

// Flags, water, waterfalls, flipbook screens and swaying trees animate on
// TimeGain, the presentation's clock: it runs with the game and stops while
// the game is paused.
TEST_CASE_METHOD(GameScenario, "trackAnimationClockStopsWhilePaused", "[scenario][world]")
{
    Session session({});
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    openhorizon::Game& game = session.game();
    const openhorizon::render::WorldRenderer* world = game.worldRenderer();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }
    const float running = world->timeGain().x;
    CHECK(running > 1.0F);
    CHECK_THAT(running, WithinAbs(openhorizon::render::TrackFrameConstants::timeGain(game.animationTime()).x, 0.05));
    CHECK_THAT(world->timeGain().w, WithinAbs(1.0, 1e-6));

    session.press(openhorizon::Button::Start);
    CHECK(game.pauseShown());
    session.run(0.1F, {});
    const float paused = world->timeGain().x;
    session.run(2.0F, {});
    CHECK_THAT(world->timeGain().x, WithinAbs(paused, 1e-6));

    session.press(openhorizon::Button::B);
    CHECK(!game.pauseShown());
    session.run(1.0F, {});
    CHECK(world->timeGain().x > paused + 0.9F);
    CHECK_THAT(openhorizon::render::TrackFrameConstants::timeGain(28800.0F + 5.0F).x, WithinAbs(5.0, 0.01));
}

// "The world still feels horrible": the track effects read scene
// constants that were never set. Night lights the emissive maps (signs,
// windows) and day leaves them off, as SwitchOnLights says, and the
// festival's reflective materials sample the track's own cube maps.
TEST_CASE_METHOD(GameScenario, "trackShadingSwitchesLightsAndReflects", "[scenario][world]")
{
    struct Shot {
        const char* name;
        float seconds;
        bool lit;
    };
    const Shot shots[] = {
        {"shading_noon", 12.0F * 3600.0F, false},
        {"shading_night", 23.0F * 3600.0F, true},
    };
    for (const Shot& shot : shots) {
        openhorizon::GameOptions options;
        options.startTime = shot.seconds;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(4.0F, {});
        const openhorizon::render::WorldRenderer* world = session.game().worldRenderer();
        CHECK(world != nullptr);
        if (world == nullptr) {
            return;
        }
        const glm::vec4 emissive = world->emissiveSwitchOn();
        CHECK(emissive.z == 1.0F);
        if (shot.lit) {
            CHECK(emissive.x >= openhorizon::render::TrackFrameConstants::kSwitchOnLightsFloor);
        } else {
            CHECK(emissive.x == -1.0F);
        }
        CHECK(world->cubeMapsLoaded() > 0);
        std::fprintf(stderr, "%s: world pass GPU %.3f ms\n", shot.name, worldPassMs(session));
        session.capture(shot.name);
    }
}

/// The share of the sun shadow mask's pixels that are shadowed (below
/// white), and the GPU milliseconds of the shadow passes over one second
/// of `session` standing still.
struct ShadowReading {
    double shadowed = 0.0;
    double gpuMs = 0.0;
    /// Median scene luminance (linear colour) of shadowed and of sunlit
    /// pixels in the lower half of the picture, below the sky.
    double shadowedLuminance = 0.0;
    double litLuminance = 0.0;
};

ShadowReading readShadows(Session& session)
{
    ShadowReading reading;
    int frames = 0;
    for (float t = 0.0F; t < 1.0F; t += kStep) {
        session.run(kStep, {});
        reading.gpuMs += session.game().frameStats().gpuFrameSeconds()[static_cast<std::size_t>(
            openhorizon::render::FrameStats::Part::DrawShadows)];
        ++frames;
    }
    reading.gpuMs *= 1000.0 / frames;
    const openhorizon::render::ShadowPass* pass = session.game().shadowPass();
    if (pass == nullptr || pass->width() <= 0) {
        return reading;
    }
    std::vector<std::uint8_t> red(static_cast<std::size_t>(pass->width()) * static_cast<std::size_t>(pass->height()));
    glGetTextureImage(pass->mask(), 0, GL_RED, GL_UNSIGNED_BYTE, static_cast<GLsizei>(red.size()), red.data());
    const auto shadowed = std::count_if(red.begin(), red.end(), [](std::uint8_t v) { return v < 250; });
    reading.shadowed = static_cast<double>(shadowed) / static_cast<double>(red.size());
    const int w = pass->width();
    const int h = pass->height();
    // The scene target holds the square root of linear colour (see
    // FrameRenderer); each channel is squared back before the luminance.
    std::vector<float> hdr(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    glGetTextureImage(session.game().frameRenderer()->sceneColour(), 0, GL_RGBA, GL_FLOAT,
        static_cast<GLsizei>(hdr.size() * sizeof(float)), hdr.data());
    std::vector<double> dark;
    std::vector<double> lit;
    for (int y = h / 2; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // The mask's rows run from the top, the scene's from the bottom.
            const std::size_t i
                = (static_cast<std::size_t>(h - 1 - y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
            const auto linear = [&](std::size_t channel) { return static_cast<double>(hdr[channel]) * hdr[channel]; };
            const double luminance = 0.2126 * linear(i) + 0.7152 * linear(i + 1) + 0.0722 * linear(i + 2);
            const std::uint8_t m
                = red[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
            if (m == 0) {
                dark.push_back(luminance);
            } else if (m == 255) {
                lit.push_back(luminance);
            }
        }
    }
    const auto median = [](std::vector<double> values) {
        if (values.empty()) {
            return 0.0;
        }
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
        return values[values.size() / 2];
    };
    reading.shadowedLuminance = median(dark);
    reading.litLuminance = median(lit);
    return reading;
}

// "The world still feels horrible": nothing cast a shadow. By day the sun
// shadows darken the ground under the car and the trees and props around
// it (ShadowIntensity is 1 on Colorado from 07:50 to 19:09), and by night
// the mask stays white.
TEST_CASE_METHOD(GameScenario, "sunShadowsFallByDayOnly", "[scenario][world]")
{
    struct Shot {
        const char* name;
        float hours;
        bool shadows;
    };
    for (const Shot shot :
        {Shot{"shadows_noon", 12.0F, true}, Shot{"shadows_low_sun", 18.5F, true}, Shot{"shadows_night", 2.0F, false}}) {
        openhorizon::GameOptions options;
        options.spawnAt = glm::vec2(4330.0F, -250.0F);
        options.spawnHeading = 0.0F;
        options.startTime = shot.hours * 3600.0F;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(3.0F, {});
        const ShadowReading reading = readShadows(session);
        std::fprintf(stderr, "%s: %.1f%% shadowed, shadow passes GPU %.3f ms\n", shot.name, reading.shadowed * 100.0,
            reading.gpuMs);
        if (shot.shadows) {
            CHECK(reading.shadowed > 0.02);
            // "The shadowed road is close to black": the mask takes away the
            // sun only; the track effects keep the hemisphere ambient
            // (skyColor, groundColor) times (0.05 + 0.95 AO) (the terrain's
            // h_blnd3_spec_norm2_ao, slots 43-58). At noon on Colorado that is
            // at least 0.1 in luminance (HLTopColour 0.21, AO at least 0.5)
            // against a sun of at most 0.6 (SunColor times SunColorMult), so a
            // shadowed pixel keeps at least 0.14 of a sunlit one.
            if (shot.hours == 12.0F) {
                std::fprintf(stderr, "%s: shadowed %.4f, sunlit %.4f\n", shot.name, reading.shadowedLuminance,
                    reading.litLuminance);
                CHECK(reading.shadowedLuminance >= 0.14 * reading.litLuminance);
            }
        } else {
            CHECK(reading.shadowed == 0.0);
        }
        session.capture(shot.name);
    }
}

// "The world still feels horrible": the picture the post-processing makes
// at noon, dusk and night at the free-roam start, and at noon inside the
// Redrock post-processing zone (the centroid of its first triangle in
// PostProcessingZones_safe.xml), each after 5 s standing still so the
// exposure has settled.
TEST_CASE_METHOD(GameScenario, "postProcessingAtTimesAndInAZone", "[scenario][world]")
{
    struct Shot {
        const char* name;
        float seconds;
        std::optional<glm::vec2> at;
        /// The zone the player is in, and the range of the filmic white
        /// point: TrackSettings' is 1 by day and 11 at night, blended by
        /// DayNightPostProcess, which is part way at dusk.
        const char* zone;
        float whiteMin;
        float whiteMax;
    };
    const Shot shots[] = {
        {"post_noon", 12.0F * 3600.0F, std::nullopt, "Inner_Festival", 1.0F, 1.0F},
        {"post_dusk", 1150.0F * 60.0F, std::nullopt, "Inner_Festival", 1.1F, 10.9F},
        {"post_night", 23.0F * 3600.0F, std::nullopt, "Inner_Festival", 11.0F, 11.0F},
        {"post_zone_redrock", 12.0F * 3600.0F, glm::vec2(-6384.0F, -4888.0F), "Redrock", 1.0F, 1.0F},
    };
    for (const Shot& shot : shots) {
        openhorizon::GameOptions options;
        options.startTime = shot.seconds;
        options.spawnAt = shot.at;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(4.0F, {});
        // The GPU time of the post-processing (the frame statistics' tonemap
        // part), averaged over the last second, for the report.
        double tonemapSeconds = 0.0;
        int frames = 0;
        for (float t = 0.0F; t < 1.0F; t += kStep) {
            session.run(kStep, {});
            tonemapSeconds += session.game().frameStats().gpuFrameSeconds()[static_cast<std::size_t>(
                openhorizon::render::FrameStats::Part::Tonemap)];
            ++frames;
        }
        std::fprintf(stderr, "%s: post-processing GPU %.3f ms\n", shot.name, tonemapSeconds * 1000.0 / frames);
        session.capture(shot.name);
        CHECK(std::string(session.game().postProcessingZone()) == std::string(shot.zone));
        const openhorizon::PostSettings* settings = session.game().postSettings();
        CHECK(settings != nullptr);
        if (settings != nullptr) {
            CHECK((settings->filmic.white >= shot.whiteMin - 1e-4F && settings->filmic.white <= shot.whiteMax + 1e-4F));
        }
    }
}

// The sky had no clouds: CRealtimeSky draws its far and close cloud layers
// over the dome.
TEST_CASE_METHOD(GameScenario, "skyDrawsItsClouds", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(4330.0F, -250.0F);
    options.spawnHeading = 0.0F;
    options.startTime = 13.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(3.0F, {});
    CHECK(session.game().clouds() != nullptr);
    session.capture("clouds_sky");
}

// Chrome, glass and paint reflected nothing: the car effect's
// reflectionScaler stayed at its default of 0 and envSampler was left
// empty. The game renders the environment cube around the focus car, all six
// faces a frame under the in-game render scenario, and sets
// reflectionScaler to 1 for every car draw. Captured where
// sunShadowsFallByDayOnly captures, at noon and in the low evening sun, so
// the two sets of pictures compare.
TEST_CASE_METHOD(GameScenario, "carsReflectTheEnvironmentCube", "[scenario][world]")
{
    struct Shot {
        const char* name;
        float hours;
    };
    for (const Shot shot : {Shot{"cube_noon", 12.0F}, Shot{"cube_low_sun", 18.5F}}) {
        openhorizon::GameOptions options;
        options.spawnAt = glm::vec2(4330.0F, -250.0F);
        options.spawnHeading = 0.0F;
        options.startTime = shot.hours * 3600.0F;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(3.0F, {});
        const openhorizon::Game& game = session.game();
        const openhorizon::render::EnvironmentCube* cube = game.environmentCube();
        CHECK(cube != nullptr);
        const openhorizon::render::CarRenderer* car = game.playerCarModel();
        CHECK(car != nullptr);
        if (cube == nullptr || car == nullptr) {
            continue;
        }
        CHECK(cube->facesDrawn() == 6);
        CHECK(cube->schedule().complete());
        CHECK(cube->mipBuilds() > 0);
        CHECK(car->boundTexture("envSampler") == cube->texture());
        const std::optional<glm::vec4> scaler = car->pixelConstant("reflectionScaler");
        CHECK(scaler.has_value());
        if (scaler) {
            CHECK(*scaler == glm::vec4(1.0F));
        }
        // No face is black. Level 3 (32 texels) of each face, in the square
        // root encoding.
        constexpr int kLevel = 3;
        constexpr int kSize = openhorizon::render::envcube::kSize >> kLevel;
        std::vector<float> texels(static_cast<std::size_t>(kSize * kSize * 6 * 4));
        glGetTextureImage(cube->texture(), kLevel, GL_RGBA, GL_FLOAT,
            static_cast<GLsizei>(texels.size() * sizeof(float)), texels.data());
        for (int face = 0; face < 6; ++face) {
            double sum = 0.0;
            float brightest = 0.0F;
            for (int i = 0; i < kSize * kSize; ++i) {
                const std::size_t at = (static_cast<std::size_t>(face * kSize * kSize + i)) * 4;
                const float luminance = 0.2126F * texels[at] + 0.7152F * texels[at + 1] + 0.0722F * texels[at + 2];
                sum += luminance;
                brightest = std::max(brightest, luminance);
            }
            const double mean = sum / (kSize * kSize);
            std::fprintf(stderr, "%s: face %d mean %.3f brightest %.3f\n", shot.name, face, mean, brightest);
            CHECK(mean > 0.05);
            // Straight up (+Y) the noon sky is far brighter than the clear
            // colour (at most 0.2, see TrackSettings::cubeClearColour()).
            if (face == 2 && shot.hours == 12.0F) {
                CHECK(brightest > 0.3F);
            }
        }
        const openhorizon::render::FrameStats& stats = game.frameStats();
        const auto part = static_cast<std::size_t>(openhorizon::render::FrameStats::Part::DrawEnvironmentCube);
        std::fprintf(stderr, "%s: environment cube CPU %.3f ms, GPU %.3f ms\n", shot.name,
            stats.frameSeconds()[part] * 1000.0, stats.gpuFrameSeconds()[part] * 1000.0);
        session.capture(shot.name);
    }
}

/// The composite matrices the last frame posed the animated object placed
/// at `position` with, and its index; nothing when it was not drawn.
std::optional<std::pair<std::size_t, std::vector<glm::mat4>>> animatedPose(
    const openhorizon::Game& game, const glm::vec3& position)
{
    const openhorizon::animated::AnimatedObjects* objects = game.animatedObjects();
    if (objects == nullptr) {
        return std::nullopt;
    }
    const auto& visible = objects->visible();
    for (std::size_t v = 0; v < visible.size(); ++v) {
        const std::size_t instance = visible[v].instance;
        if (glm::length(objects->instances()[instance].placement.position - position) > 0.5F) {
            continue;
        }
        const std::size_t end = v + 1 < visible.size() ? visible[v + 1].firstMatrix : objects->pose().size();
        return std::pair(instance,
            std::vector<glm::mat4>(objects->pose().begin() + visible[v].firstMatrix,
                objects->pose().begin() + static_cast<std::ptrdiff_t>(end)));
    }
    return std::nullopt;
}

/// Starts facing the animated object at `object` from `distance` metres
/// along `from` (a horizontal direction away from it), at noon, and checks
/// that the object is drawn, moves from frame to frame, stands still while
/// the game is paused and moves on after.
void checkAnimatedObject(const glm::vec3& object, const glm::vec2& from, float distance, const std::string& name)
{
    openhorizon::GameOptions options;
    const glm::vec2 away = glm::normalize(from);
    options.spawnAt = glm::vec2(object.x, object.z) + away * distance;
    options.spawnHeading = std::atan2(-away.x, -away.y);
    options.startTime = 12.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    openhorizon::Game& game = session.game();
    session.run(1.0F, {});
    session.capture(name + "_a");
    CHECK(game.animatedObjects() != nullptr);
    CHECK(game.animatedObjectRenderer() != nullptr);
    const auto first = animatedPose(game, object);
    CHECK(first.has_value());
    if (!first || game.animatedObjectRenderer() == nullptr) {
        return;
    }
    const std::vector<std::uint32_t>& drawn = game.animatedObjectRenderer()->drawnInstances();
    CHECK(std::ranges::find(drawn, static_cast<std::uint32_t>(first->first)) != drawn.end());
    // Two frames on, its parts have moved.
    session.run(2.0F * kStep, {});
    session.capture(name + "_b");
    const auto second = animatedPose(game, object);
    CHECK(second.has_value());
    CHECK((second && second->second != first->second));
    const openhorizon::animated::AnimatedObjects& objects = *game.animatedObjects();
    const float played = objects.clipTime(first->first);

    session.press(openhorizon::Button::Start);
    CHECK(game.pauseShown());
    const auto paused = animatedPose(game, object);
    const float pausedAt = objects.clipTime(first->first);
    session.run(1.0F, {});
    CHECK_THAT(objects.clipTime(first->first), WithinAbs(pausedAt, 1e-6));
    const auto still = animatedPose(game, object);
    CHECK((paused && still && paused->second == still->second));

    session.press(openhorizon::Button::B);
    CHECK(!game.pauseShown());
    session.run(1.0F, {});
    CHECK(objects.clipTime(first->first) > played + 0.9F);
    const auto after = animatedPose(game, object);
    CHECK((after && still && after->second != still->second));
}

// The wind turbines and fairground rides of Colorado play their Granny
// animations on the presentation clock (see animated::AnimatedObjects): a
// wind turbine's rotor turns (a rigid part), and the ferris wheel turns (a
// skinned model), both stopping while paused. The windmills are not drawn:
// the textures their sets name (11101, 11170) are not on the disc.
TEST_CASE_METHOD(GameScenario, "windTurbineTurnsAndStopsWhilePaused", "[scenario][world]")
{
    // The first wind turbine of the row placed by set __R00G00211.
    checkAnimatedObject({-2240.8F, 17.0F, -1876.3F}, {0.0F, 1.0F}, 400.0F, "animated_turbine");
}

TEST_CASE_METHOD(GameScenario, "ferrisWheelTurnsAndStopsWhilePaused", "[scenario][world]")
{
    // The festival's ferris wheel (set __R00G00076), seen across its Z axis
    // (0.990, 0, -0.139), about which it turns.
    checkAnimatedObject({-1084.47F, -1.51F, -614.58F}, {-0.990F, 0.139F}, 150.0F, "animated_ferris");
}

// Free roam starts at the festival, among its crowds: the people near the
// camera are drawn as models, the rest as sprites, and their clips move on
// with the game's clock and stop while it is paused.
TEST_CASE_METHOD(GameScenario, "festivalCrowdIsDrawnAndAnimated", "[scenario][world]")
{
    Session session({});
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    openhorizon::Game& game = session.game();
    const openhorizon::render::CrowdRenderer* crowd = game.crowdRenderer();
    CHECK(crowd != nullptr);
    if (crowd == nullptr) {
        return;
    }
    const openhorizon::render::CrowdRenderer::Stats& stats = crowd->stats();
    std::fprintf(stderr,
        "festival crowd: models %zu %zu %zu %zu, walkers %zu, %zu sets in view, %zu sprites in %zu sets\n",
        stats.models[0], stats.models[1], stats.models[2], stats.models[3], stats.walkers, stats.visibleSets,
        stats.spriteInstances, stats.spriteSets);
    CHECK(stats.modelTotal() > 0);
    // The level bands cap the finer levels (5 and 10 people) and the queue
    // holds 500.
    CHECK((stats.models[0] <= 5 && stats.models[1] <= 10));
    CHECK(stats.modelTotal() <= 500);
    CHECK(stats.spriteInstances > 1000);
    session.capture("crowd_festival_near");

    // A second of play moves the clips on by the game's 30 frames, give or
    // take one where the second's ends fall.
    std::uint64_t before = crowd->ticksApplied();
    session.run(1.0F, {});
    CHECK((crowd->ticksApplied() - before >= 29 && crowd->ticksApplied() - before <= 31));
    session.press(openhorizon::Button::Start);
    CHECK(game.pauseShown());
    session.run(0.1F, {});
    before = crowd->ticksApplied();
    session.run(1.0F, {});
    CHECK(crowd->ticksApplied() == before);
    session.press(openhorizon::Button::B);
    session.run(1.0F, {});
    CHECK(crowd->ticksApplied() > before + 25);
}

// From 300 m behind free roam's start, looking the same way, the
// festival's crowd shows as sprites.
TEST_CASE_METHOD(GameScenario, "festivalCrowdFromAfarIsSprites", "[scenario][world]")
{
    // Free roam starts at (-836.6, -250.6) heading -63 degrees.
    const float heading = -63.0F * std::numbers::pi_v<float> / 180.0F;
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(-836.6F, -250.6F) - 300.0F * glm::vec2(std::sin(heading), std::cos(heading));
    options.spawnHeading = heading;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    const openhorizon::render::CrowdRenderer* crowd = session.game().crowdRenderer();
    CHECK(crowd != nullptr);
    if (crowd == nullptr) {
        return;
    }
    std::fprintf(stderr, "festival from afar: %zu models, %zu sprites in %zu sets\n", crowd->stats().modelTotal(),
        crowd->stats().spriteInstances, crowd->stats().spriteSets);
    CHECK(crowd->stats().spriteInstances > 1000);
    CHECK(crowd->stats().spriteSets > 0);
    session.capture("crowd_festival_far");
}

// A race brings out its own crowds: FR04's grid is lined with the sets
// tagged FR04 or listing its route, 108.
TEST_CASE_METHOD(GameScenario, "raceCrowdShowsAtTheStart", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.race = "FR04";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    openhorizon::Game& game = session.game();
    const openhorizon::render::CrowdRenderer* crowd = game.crowdRenderer();
    CHECK((crowd != nullptr && game.race() != nullptr));
    if (crowd == nullptr || game.race() == nullptr) {
        return;
    }
    const openhorizon::render::CrowdRenderer::Stats& stats = crowd->stats();
    std::fprintf(stderr, "FR04 start: %zu event sets in view of %zu, %zu models, %zu sprites\n", stats.visibleEventSets,
        stats.visibleSets, stats.modelTotal(), stats.spriteInstances);
    CHECK(game.race()->event().routeId == 108);
    CHECK(stats.visibleEventSets > 0);
    CHECK(stats.modelTotal() > 0);
    session.capture("crowd_race_start_near");
    // Far along the grid, once the race is on and the car has left the
    // start behind.
    session.runUntil(30.0F, [&] { return game.race()->started(); });
    session.run(8.0F, {.throttle = 1.0F});
    session.capture("crowd_race_start_far");
}

/// Watches the world streaming in during a drive, by the game's own rule
/// for what it draws: a chunk is wanted while the camera's zone lists it
/// (WorldChunk::zones) and the camera's 3D distance to its bounds lies in
/// its band (verified in WorldIndex and ScatterSet). Every frame it checks
/// the wanted chunks of the tiles near the camera against what the
/// renderer draws: a wanted chunk not drawn is late, a chunk drawn while
/// not wanted is out of place, and one that comes on screen beyond the end
/// of its band has popped in where the game would not show it.
class StreamingProbe {
public:
    explicit StreamingProbe(const openhorizon::render::WorldRenderer& world)
        : m_world(world)
    {
    }

    /// Frames a chunk waited from being wanted to being drawn.
    std::vector<int> delays;
    /// Chunks still waiting after the last frame, and the longest wait.
    std::size_t waiting() const { return m_waiting.size(); }
    /// Chunks that stopped being wanted before they were ever drawn.
    int neverShown = 0;
    /// Chunks wanted but not drawn at the first frame watched.
    std::size_t missingAtStart = 0;
    /// For every chunk that came on screen in view, how far past the end
    /// of its band it was, in metres (0 inside it).
    std::vector<float> appearedBeyondBand;
    /// Chunk-frames drawn while not wanted.
    std::size_t drawnUnwanted = 0;

    void frame(const openhorizon::render::Camera& camera)
    {
        const openhorizon::world::WorldTileGrid& grid = m_world.grid();
        const auto& chunks = grid.index().chunks();
        const glm::mat4 viewProjection = camera.projection(16.0F / 9.0F) * camera.view();
        const openhorizon::world::ZoneGrid* zones = grid.index().zoneGrid();
        const int zone = zones != nullptr ? zones->zoneAt(camera.position.x, camera.position.z) : -1;
        m_drawnBefore.resize(chunks.size(), 0);
        m_residentFrame.resize(chunks.size(), -1);
        for (std::size_t t = 0; t < grid.tiles().size(); ++t) {
            const openhorizon::world::WorldTileGrid::Tile& tile = grid.tiles()[t];
            if (grid.distanceTo(tile, camera.position.x, camera.position.z) > kStreamingWatchDistance) {
                continue;
            }
            for (const std::uint32_t c : m_world.drawnChunks(t)) {
                m_residentFrame[c] = m_frame;
            }
            for (const std::uint32_t c : tile.chunks) {
                const openhorizon::world::WorldChunk& chunk = chunks[c];
                const glm::vec3 nearest = glm::clamp(camera.position, chunk.boundsMin, chunk.boundsMax);
                const float distance = glm::length(camera.position - nearest);
                const bool wanted = distance >= chunk.bandStart && distance < chunk.bandEnd && chunk.visibleFrom(zone);
                const bool drawn = m_world.chunkDrawn(c) && m_residentFrame[c] == m_frame;
                const auto waiting = m_waiting.find(c);
                if (wanted && !drawn && waiting == m_waiting.end()) {
                    m_waiting.emplace(c, m_frame);
                } else if (waiting != m_waiting.end() && (drawn || !wanted)) {
                    if (drawn) {
                        delays.push_back(m_frame - waiting->second);
                    } else {
                        ++neverShown;
                    }
                    m_waiting.erase(waiting);
                }
                drawnUnwanted += drawn && !wanted ? 1 : 0;
                if (drawn && m_drawnBefore[c] == 0 && m_frame > 0
                    && inView(viewProjection, chunk.boundsMin, chunk.boundsMax)) {
                    appearedBeyondBand.push_back(std::max(distance - chunk.bandEnd, 0.0F));
                }
                m_drawnBefore[c] = drawn ? 1 : 0;
            }
        }
        if (m_frame == 0) {
            missingAtStart = m_waiting.size();
        }
        ++m_frame;
    }

private:
    /// Tiles farther than this are not watched, to keep the frames quick;
    /// it covers the bands of every prop and procedural copy.
    static constexpr float kStreamingWatchDistance = 2000.0F;

    static bool inView(const glm::mat4& viewProjection, const glm::vec3& lo, const glm::vec3& hi)
    {
        for (int plane = 0; plane < 5; ++plane) {
            int beyond = 0;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec4 p = viewProjection
                    * glm::vec4(
                        (corner & 1) ? hi.x : lo.x, (corner & 2) ? hi.y : lo.y, (corner & 4) ? hi.z : lo.z, 1.0F);
                const float value = plane == 0 ? p.x + p.w
                    : plane == 1               ? p.w - p.x
                    : plane == 2               ? p.y + p.w
                    : plane == 3               ? p.w - p.y
                                               : p.w;
                beyond += value < 0.0F ? 1 : 0;
            }
            if (beyond == 8) {
                return false;
            }
        }
        return true;
    }

    const openhorizon::render::WorldRenderer& m_world;
    std::unordered_map<std::uint32_t, int> m_waiting;
    std::vector<std::uint8_t> m_drawnBefore;
    /// The last frame each chunk's tile had it on the GPU.
    std::vector<int> m_residentFrame;
    int m_frame = 0;
};

int percentileOf(std::vector<int> values, double fraction)
{
    if (values.empty()) {
        return 0;
    }
    std::sort(values.begin(), values.end());
    return values[static_cast<std::size_t>(fraction * static_cast<double>(values.size() - 1))];
}

/// How long the streaming scenario drives.
constexpr float kStreamingDriveSeconds = 45.0F;

/// The longest a chunk may wait to be shown once in its band, in frames.
constexpr int kStreamingMaxDelayFrames = 6;

/// How far past the end of its band a chunk may come on screen, in metres.
constexpr float kStreamingBandSlack = 25.0F;

// "I see models spawning from nowhere in the map": driving free roam along
// the roads, every chunk has to be on screen soon after the camera comes
// within its level-of-detail band.
TEST_CASE_METHOD(GameScenario, "drivingStreamsTheWorldInTime", "[scenario][world]")
{
    Session session({});
    CHECK(session.waitForCar());
    openhorizon::Game& game = session.game();
    const openhorizon::render::WorldRenderer* world = game.worldRenderer();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }
    const std::size_t swapsBefore = world->tileSwaps().size();
    StreamingProbe probe(*world);
    CHECK(game.navigation() != nullptr);
    if (game.navigation() == nullptr) {
        return;
    }
    RoadFollower follower(*game.navigation());
    float driven = 0.0F;
    for (float t = 0.0F; t < kStreamingDriveSeconds; t += kStep) {
        const glm::vec3 before = game.playerCar().position();
        CHECK(game.frame(follower.controls(game.playerCar()), kStep));
        driven += glm::length(game.playerCar().position() - before);
        probe.frame(game.camera());
    }
    const auto& swaps = world->tileSwaps();
    const std::size_t swapCount = swaps.size() - std::min(swapsBefore, swaps.size());
    std::vector<float> builds;
    std::vector<float> textures;
    for (auto it = swaps.end() - static_cast<std::ptrdiff_t>(swapCount); it != swaps.end(); ++it) {
        builds.push_back(it->buildSeconds);
        textures.push_back(it->textureSeconds);
    }
    std::sort(builds.begin(), builds.end());
    std::sort(textures.begin(), textures.end());
    const auto median = [](const std::vector<float>& v) { return v.empty() ? 0.0F : v[v.size() / 2]; };
    const auto worst = [](const std::vector<float>& v) { return v.empty() ? 0.0F : v.back(); };
    std::vector<float> beyond = probe.appearedBeyondBand;
    std::sort(beyond.begin(), beyond.end());
    const auto appearedBeyond = static_cast<std::size_t>(
        std::count_if(beyond.begin(), beyond.end(), [](float metres) { return metres > kStreamingBandSlack; }));
    std::fprintf(stderr,
        "streaming: %.0f m driven in %.0f s; %zu chunks came on screen in view, %zu more than %.0f m past the end "
        "of their band (median %.0f m, 95th %.0f m, worst %.0f m past); %zu chunk-frames drawn outside their band "
        "or zone\n",
        static_cast<double>(driven), static_cast<double>(kStreamingDriveSeconds), beyond.size(), appearedBeyond,
        static_cast<double>(kStreamingBandSlack),
        static_cast<double>(beyond.empty() ? 0.0F : beyond[beyond.size() / 2]),
        static_cast<double>(beyond.empty() ? 0.0F : beyond[(beyond.size() - 1) * 95 / 100]),
        static_cast<double>(beyond.empty() ? 0.0F : beyond.back()), probe.drawnUnwanted);
    std::fprintf(stderr,
        "streaming: %zu waits for a wanted chunk, frames median %d, 95th %d, worst %d; %d never drawn, %zu missing "
        "at the start, %zu still waiting; %zu tile swaps, build s median %.3f worst %.3f, texture wait s median "
        "%.3f worst %.3f\n",
        probe.delays.size(), percentileOf(probe.delays, 0.5), percentileOf(probe.delays, 0.95),
        percentileOf(probe.delays, 1.0), probe.neverShown, probe.missingAtStart, probe.waiting(), swapCount,
        static_cast<double>(median(builds)), static_cast<double>(worst(builds)), static_cast<double>(median(textures)),
        static_cast<double>(worst(textures)));
    CHECK(driven > 300.0F);
    CHECK(probe.missingAtStart == std::size_t{0});
    CHECK(probe.neverShown == 0);
    CHECK(probe.waiting() == std::size_t{0});
    CHECK(percentileOf(probe.delays, 1.0) <= kStreamingMaxDelayFrames);
    CHECK(appearedBeyond == std::size_t{0});
    CHECK(probe.drawnUnwanted == std::size_t{0});
    session.capture("streaming_end");
}

// Seen from the hill north of it at dusk, Redstone_Lake_01 was hidden
// under its own bed: its vertex shader lifts every vertex by vc360_3.x
// times a sine, and that constant came from the material's pixel group
// (65 there, a value of the pixel shader's), which sank the whole surface
// tens of metres. The water must be what the camera sees where the lake
// lies, at the height its model gives.
TEST_CASE_METHOD(GameScenario, "redstoneLakeShowsItsWater", "[scenario][world]")
{
    // The lake's model; the name is the disc's, whose one part is
    // Redstone_Lake_01_NOLOD.
    const std::optional<openhorizon::Bytes> file = disc().read("media/tracks/colorado/bin/coloradoout.12840.rmb.bin");
    CHECK(file.has_value());
    if (!file) {
        return;
    }
    const openhorizon::world::RenderMesh lake = openhorizon::world::rendermesh::parse(*file, "Redstone_Lake_01");
    std::vector<std::array<glm::vec3, 3>> triangles;
    for (const openhorizon::world::RenderMesh::Part& part : lake.parts) {
        for (const openhorizon::world::RenderMesh::Material& material : part.materials) {
            for (std::size_t t = 0; t + 2 < material.triangles.size(); t += 3) {
                triangles.push_back({part.positions[material.triangles[t]], part.positions[material.triangles[t + 1]],
                    part.positions[material.triangles[t + 2]]});
            }
        }
    }
    CHECK(!triangles.empty());
    // The water's height at (x, z), if the lake covers it.
    const auto surfaceAt = [&triangles](float x, float z) -> std::optional<float> {
        for (const auto& [a, b, c] : triangles) {
            const float det = (b.z - c.z) * (a.x - c.x) + (c.x - b.x) * (a.z - c.z);
            if (std::abs(det) < 1e-6F) {
                continue;
            }
            const float u = ((b.z - c.z) * (x - c.x) + (c.x - b.x) * (z - c.z)) / det;
            const float v = ((c.z - a.z) * (x - c.x) + (a.x - c.x) * (z - c.z)) / det;
            if (u >= 0.0F && v >= 0.0F && u + v <= 1.0F) {
                return u * a.y + v * b.y + (1.0F - u - v) * c.y;
            }
        }
        return std::nullopt;
    };

    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(1800.0F, 1650.0F);
    options.spawnHeading = std::numbers::pi_v<float>;
    options.startTime = 19.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(3.0F, {});
    session.capture("lake_redstone_dusk");
    const SceneReadback scene = readScene(session);
    CHECK(scene.width > 0);
    // Where the camera looks into the lake's outline, it sees the water, or
    // the shore where the ground rises above it; never ground below it.
    int onWater = 0;
    int belowWater = 0;
    for (int y = 0; y < scene.height; y += 2) {
        for (int x = 0; x < scene.width; x += 2) {
            if (scene.depth[scene.at(x, y)] <= 0.0F) {
                continue;
            }
            const glm::vec3 p = scene.worldAt(x, y);
            const std::optional<float> water = surfaceAt(p.x, p.z);
            if (!water) {
                continue;
            }
            // The waves (vc360_3 of the vertex group) move the surface by
            // centimetres; a quarter metre also covers the depth's rounding.
            if (p.y < *water - 0.25F) {
                ++belowWater;
            } else if (p.y < *water + 0.25F) {
                ++onWater;
            }
        }
    }
    std::fprintf(stderr, "redstone lake: %d samples on the water, %d below it\n", onWater, belowWater);
    CHECK(onWater > 1000);
    CHECK(belowWater * 20 < onWater);
}

// A player at dusk on the interstate west of the festival (colorado, x
// -3033, z -3517) saw huge dark sheets stand beyond the far carriageway,
// their tops following the ridges and their feet fading into the ground.
// They were the river under the interstate's bridge (Shape012 in
// coloradoout.11726, lake_anim_norm_opac_refl_3): its waves lifted the
// white-painted vertices by up to 65 m, the pixel constant the programs
// read in place of the model's vertex constant. The sky above the mesa
// across the river stays sky over a whole wave period of the river's
// material (vc360_3.y = 0.2 rad/s: 31.4 s).
TEST_CASE_METHOD(GameScenario, "riverWavesStayOnTheRiverAtTheInterstate", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(-3033.0F, -3517.0F);
    options.spawnHeading = -2.51F;
    options.startTime = 19.5F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    // Screen points above the mesa's top across the river, as fractions of
    // the picture from its top left: sky in the scene without the sheets.
    constexpr std::array<glm::vec2, 6> kSky{
        {{0.50F, 0.06F}, {0.60F, 0.06F}, {0.70F, 0.08F}, {0.80F, 0.08F}, {0.90F, 0.08F}, {0.97F, 0.10F}}};
    std::size_t covered = 0;
    std::size_t samples = 0;
    float worstAt = -1.0F;
    std::size_t worst = 0;
    for (float t = 0.0F; t < 32.0F; t += 0.5F) {
        session.run(0.5F, {});
        const SceneReadback scene = readScene(session);
        CHECK(!scene.depth.empty());
        if (scene.depth.empty()) {
            return;
        }
        std::size_t now = 0;
        for (const glm::vec2& point : kSky) {
            // The depth texture's rows start at the bottom of the picture;
            // reversed depth leaves the sky at 0.
            const int x = static_cast<int>(point.x * static_cast<float>(scene.width));
            const int y = scene.height - 1 - static_cast<int>(point.y * static_cast<float>(scene.height));
            now += scene.depth[scene.at(x, y)] > 0.0F ? 1 : 0;
        }
        covered += now;
        samples += kSky.size();
        if (now > worst) {
            worst = now;
            worstAt = t;
            session.capture("river_waves_interstate_worst");
        }
    }
    session.capture("river_waves_interstate");
    std::fprintf(stderr, "river waves: %zu of %zu sky samples covered, worst %zu at %.1f s\n", covered, samples, worst,
        static_cast<double>(worstAt));
    CHECK(covered == std::size_t{0});
}

// A player at the festival saw the red "HORIZON Colorado" feather flags
// by the large marquee stretched into sheets many metres wide. The flags'
// vertex shader (anim_flag_diff_spec_norm_3_lm) sways them about the
// model's origin, at the foot of the pole, and divides by the position's x,
// the distance from the pole: it was given world positions, hundreds of
// metres from the origin, rather than the model's own. Seen from the
// north-west, past a tree and a floodlight mast, the sky above the four
// flags (coloradoout.06265 at -716.6, -474.8; -695.7, -490.8; -724.1,
// -489.3; -721.2, -535.1) stays sky while they sway.
TEST_CASE_METHOD(GameScenario, "festivalFlagsKeepTheirSize", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(-735.0F, -455.0F);
    options.spawnHeading = 2.48F;
    Session session(options);
    CHECK(session.waitForCar());
    // Columns of the picture in 32nds from its left, and rows from its top
    // (1%, 4%, ... 13%): right of the tree (columns 0 to 4) and clear of
    // the mast (14 to 16).
    constexpr int kColumns = 32;
    constexpr std::array<std::pair<int, int>, 2> kSkyColumns{{{5, 13}, {17, 31}}};
    constexpr int kRows = 5;
    std::size_t covered = 0;
    std::size_t samples = 0;
    std::size_t worst = 0;
    float worstAt = -1.0F;
    for (float t = 0.0F; t < 20.0F; t += 0.5F) {
        session.run(0.5F, {});
        const SceneReadback scene = readScene(session);
        CHECK(!scene.depth.empty());
        if (scene.depth.empty()) {
            return;
        }
        std::size_t now = 0;
        for (const auto& [first, last] : kSkyColumns) {
            for (int column = first; column <= last; ++column) {
                for (int row = 0; row < kRows; ++row) {
                    const float x = (static_cast<float>(column) + 0.5F) / static_cast<float>(kColumns);
                    const float y = 0.01F + 0.03F * static_cast<float>(row);
                    // The depth texture's rows start at the bottom of the
                    // picture; reversed depth leaves the sky at 0.
                    const int px = static_cast<int>(x * static_cast<float>(scene.width));
                    const int py = scene.height - 1 - static_cast<int>(y * static_cast<float>(scene.height));
                    now += scene.depth[scene.at(px, py)] > 0.0F ? 1 : 0;
                    ++samples;
                }
            }
        }
        covered += now;
        if (now > worst) {
            worst = now;
            worstAt = t;
            session.capture("festival_flags_worst");
        }
    }
    session.capture("festival_flags");
    std::fprintf(stderr, "festival flags: %zu of %zu sky samples covered, worst %zu at %.1f s\n", covered, samples,
        worst, static_cast<double>(worstAt));
    CHECK(covered == std::size_t{0});
}

/// What the far tier (render::BillboardRenderer) should show and shows.
struct FarTierReading {
    /// Cards in view, on a set the loaded route shows, fully faded in by
    /// their shaders' rule.
    std::size_t expected = 0;
    /// Of those, the ones beyond their models' end: only their card shows
    /// the copy there.
    std::size_t beyondModels = 0;
    /// Of those, the ones handed to the GPU with their texture in.
    std::size_t drawn = 0;
};

FarTierReading readFarTier(openhorizon::Game& game)
{
    FarTierReading reading;
    const openhorizon::world::TrackWorld* world = game.trackWorld();
    const openhorizon::render::BillboardRenderer* renderer = game.billboardRenderer();
    const openhorizon::render::WorldRenderer* worldRenderer = game.worldRenderer();
    if (world == nullptr || !world->placements || renderer == nullptr || worldRenderer == nullptr) {
        return reading;
    }
    const openhorizon::render::Camera& camera = game.camera();
    const glm::mat4 viewProjection = camera.projection(16.0F / 9.0F) * camera.view();
    const int route = worldRenderer->eventRoute();
    constexpr float kZoom = openhorizon::render::BillboardRenderer::kZoomCompensation;
    const std::vector<openhorizon::world::ScatterSet>& sets = world->placements->scatterSets();
    for (std::size_t s = 0; s < sets.size(); ++s) {
        const openhorizon::world::ScatterSet& set = sets[s];
        const bool onRoute = set.eventRoutes.empty()
            || std::ranges::find(set.eventRoutes, static_cast<std::uint8_t>(route)) != set.eventRoutes.end();
        if (set.eventProp && !(route >= 0 && set.eventTag.empty() && onRoute)) {
            continue;
        }
        for (std::size_t c = 0; c < set.billboards.size(); ++c) {
            const openhorizon::world::ScatterSet::Billboard& card = set.billboards[c];
            // The types drawn, and colours that need no objTintColour.
            if ((card.type != 0 && card.type != 1 && card.type != 4 && card.type != 5) || !(card.colour.w > 0.5F)) {
                continue;
            }
            const float distance = glm::length(camera.position - card.position);
            const float fadeStart = card.fadeStart * kZoom;
            const float fromNear = distance - card.nearEnd * kZoom + 2.0F * fadeStart;
            const bool fadedIn = fadeStart > 0.0F ? fromNear >= fadeStart : fromNear > 0.0F;
            const bool fullyIn
                = distance <= card.farEnd * kZoom - openhorizon::render::BillboardRenderer::kFadeOutWidth;
            const glm::vec4 clip = viewProjection * glm::vec4(card.position, 1.0F);
            const bool inView = clip.w > 0.0F && std::abs(clip.x) < clip.w && std::abs(clip.y) < clip.w;
            if (!fadedIn || !fullyIn || !inView) {
                continue;
            }
            ++reading.expected;
            reading.beyondModels += distance >= card.nearEnd ? 1 : 0;
            reading.drawn += renderer->setDrawn(s) && renderer->cardTextureReady(s, c) ? 1 : 0;
        }
    }
    return reading;
}

/// Chunks of the tiles within `reach` of the camera that the game's rule
/// wants drawn (see StreamingProbe) but whose tile's mesh does not hold.
std::size_t wantedNotResident(const openhorizon::render::WorldRenderer& world, const glm::vec3& camera, float reach)
{
    const openhorizon::world::WorldTileGrid& grid = world.grid();
    const auto& chunks = grid.index().chunks();
    const openhorizon::world::ZoneGrid* zones = grid.index().zoneGrid();
    const int zone = zones != nullptr ? zones->zoneAt(camera.x, camera.z) : -1;
    std::size_t missing = 0;
    for (std::size_t t = 0; t < grid.tiles().size(); ++t) {
        const openhorizon::world::WorldTileGrid::Tile& tile = grid.tiles()[t];
        if (grid.distanceTo(tile, camera.x, camera.z) > reach) {
            continue;
        }
        const std::vector<std::uint32_t>& resident = world.drawnChunks(t);
        for (const std::uint32_t c : tile.chunks) {
            const openhorizon::world::WorldChunk& chunk = chunks[c];
            const float distance = glm::length(camera - glm::clamp(camera, chunk.boundsMin, chunk.boundsMax));
            if (distance >= chunk.bandStart && distance < chunk.bandEnd && chunk.visibleFrom(zone)
                && std::ranges::find(resident, c) == resident.end()) {
                ++missing;
            }
        }
    }
    return missing;
}

// Chunks the camera is within the bands of reach their tile, however many textures stream.
TEST_CASE_METHOD(GameScenario, "everyWantedChunkReachesItsTile", "[scenario][world]")
{
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(4374.65F, 751.4F);
    options.spawnHeading = -0.37F;
    options.startTime = 18.75F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(12.0F, {});
    const openhorizon::render::WorldRenderer* world = session.game().worldRenderer();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }
    const std::size_t missing = wantedNotResident(*world, session.game().camera().position, 2000.0F);
    std::fprintf(stderr, "wanted chunks not resident: %zu (textures %.0f MiB, %zu jobs pending)\n", missing,
        static_cast<double>(world->stats().textureBytes) / (1024.0 * 1024.0), world->stats().pendingJobs);
    CHECK(missing == std::size_t{0});
}

// Past their models, procedural copies go on as far-tier cards (to 2500 m for
// trees, 500 m for barriers): every card in view the game's rule shows is drawn.
TEST_CASE_METHOD(GameScenario, "farTierDrawsTheForestsAndBarriers", "[scenario][world]")
{
    struct Spot {
        const char* name;
        glm::vec2 at;
        float heading;
        /// Cards past their models the spot must show at the least: the
        /// point of the tier there.
        std::size_t beyondModels;
    };
    const std::array spots{
        Spot{"far_tier_reservoir_hills", {2002.0F, -2156.0F}, 2.31F, 100},
        Spot{"far_tier_festival_barriers", {-640.0F, -200.0F}, -1.5708F, 100},
        Spot{"far_tier_red_rock", {-5464.0F, -3021.0F}, -1.99F, 20},
    };
    for (const Spot& spot : spots) {
        openhorizon::GameOptions options;
        options.spawnAt = spot.at;
        options.spawnHeading = spot.heading;
        options.startTime = 18.75F * 3600.0F;
        Session session(options);
        CHECK(session.waitForCar());
        const openhorizon::render::BillboardRenderer* renderer = session.game().billboardRenderer();
        CHECK(renderer != nullptr);
        if (renderer == nullptr) {
            continue;
        }
        for (float t = 0.0F; t < 30.0F && !renderer->texturesSettled(); t += kStep) {
            session.run(kStep, {});
        }
        session.run(2.0F, {});
        const FarTierReading reading = readFarTier(session.game());
        std::fprintf(stderr,
            "%s: %zu cards in view to show, %zu past their models, %zu drawn (%zu sets, %zu cards handed over)\n",
            spot.name, reading.expected, reading.beyondModels, reading.drawn, renderer->stats().sets,
            renderer->stats().cards);
        CHECK(reading.beyondModels >= spot.beyondModels);
        CHECK(reading.drawn == reading.expected);
        session.capture(spot.name);
    }
}

} // namespace
} // namespace openhorizon::test
