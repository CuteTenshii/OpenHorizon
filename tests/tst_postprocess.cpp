// The post-processing: the template, zone and TrackSettings readers, the
// zone blend, the colour grading tables and the maths of the final combine,
// against the executable's formulas; the GPU chain on a uniform picture in
// an offscreen context; and Colorado's files when FH1_GAME_DIR is set.
#include "support/GlContext.h"
#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "base/Error.h"
#include "base/Host.h"
#include "game/PostProcessing.h"
#include "platform/Window.h"
#include "render/Gl.h"
#include "render/PostMath.h"
#include "render/PostProcess.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using fh::PostZone;
using fh::PostZoneTracker;
namespace post = fh::render::post;

constexpr const char* kBloom = R"(<Templates>
    <Template Name="Hazy" Cutoff="1" Scale="1.2" Persist="0.001" Weight0="3.8" Weight1="0.8" />
    <Template Name="Soft" Cutoff="1.5" Scale="0.5" Persist="0.002" weight0="0.295" />
</Templates>)";

constexpr const char* kColour = R"(<Templates>
    <Template Name="Bright" DesatAmount="0.9" DesatLuminosityR="0.21" DesatLuminosityG="0.71"
        DesatLuminosityB="0.07" DesatPostModR="1.1" DesatPostModG="1.2" DesatPostModB="1.3" ExposureBias="0.5"
        Contrast="0.25" Brightness="0.125" />
</Templates>)";

constexpr const char* kVignette = R"(<Templates>
    <Template Name="Top" Angle="-2.3" PosX="0.1" PosY="0.2" ScaleX="0.01" ScaleY="1.8" Power="2" ColorR="0.5"
        ColorG="0.811" ColorB="0.992" ColorA="1" MinIntensity="0.75" MaxIntensity="1.0" />
    <Template Name="Bottom" Angle="0" PosX="0" PosY="0" ScaleX="1" ScaleY="1" Power="3" ColorR="0" ColorG="0"
        ColorB="0" ColorA="0" MinIntensity="0.2" MaxIntensity="1.0" />
</Templates>)";

constexpr const char* kFog = R"(<Templates>
    <Template Name="Thick" ColorR="0.84" Density="2.35" SunScatterStartDistance="0" />
</Templates>)";

constexpr const char* kZones = R"(<PostProcessingZones>
    <PostProcessingZone Name="A" NumTriangles="1" BloomTemplate="Hazy" ColorGradingMap="a.dds"
        ColorGradingMapNight="an.dds" ColorSettings="Bright" Fog="Thick" VignetteTop="Top" VignetteBottom="Bottom"
        LerpTime="10.0">
        <PostProcessingZoneTriangle>
            <PostProcessingZoneTrianglePoint posX="0" posZ="0"/>
            <PostProcessingZoneTrianglePoint posX="100" posZ="0"/>
            <PostProcessingZoneTrianglePoint posX="0" posZ="100"/>
            <PostProcessingZoneTrianglePoint posX="-500" posZ="-500"/>
        </PostProcessingZoneTriangle>
        <PostProcessingZoneTriangle>
            <PostProcessingZoneTrianglePoint posX="1000" posZ="1000"/>
            <PostProcessingZoneTrianglePoint posX="1100" posZ="1000"/>
            <PostProcessingZoneTrianglePoint posX="1000" posZ="1100"/>
        </PostProcessingZoneTriangle>
    </PostProcessingZone>
    <PostProcessingZone Name="Broken" NumTriangles="1" BloomTemplate="Missing" ColorSettings="Bright" Fog="Thick"
        VignetteTop="Top" VignetteBottom="Bottom" LerpTime="5.0">
        <PostProcessingZoneTriangle>
            <PostProcessingZoneTrianglePoint posX="0" posZ="0"/>
            <PostProcessingZoneTrianglePoint posX="10" posZ="0"/>
            <PostProcessingZoneTrianglePoint posX="0" posZ="10"/>
        </PostProcessingZoneTriangle>
    </PostProcessingZone>
    <PostProcessingZone Name="B" NumTriangles="1" BloomTemplate="Soft" ColorGradingMap="b.dds"
        ColorGradingMapNight="bn.dds" ColorSettings="Bright" Fog="Thick" VignetteTop="Bottom" VignetteBottom="Top"
        LerpTime="4.0">
        <PostProcessingZoneTriangle>
            <PostProcessingZoneTrianglePoint posX="100" posZ="0"/>
            <PostProcessingZoneTrianglePoint posX="100" posZ="100"/>
            <PostProcessingZoneTrianglePoint posX="0" posZ="100"/>
        </PostProcessingZoneTriangle>
    </PostProcessingZone>
</PostProcessingZones>)";

constexpr const char* kTrackSettings = R"(<TrackSettings Version="64">
  <DesaturateAmount value="0.9"/>
  <DesatLuminosity r="0.2126" g="0.7152" b="0.0722" a="1"/>
  <DesatPostMod r="1.04" g="1.02" b="1.0" a="0"/>
  <ToneMapAdaptiveExposure Key_ExtraDark="0" Key_Dark="4" Key_Light="13" Key_ExtraLight="65" Exp_ExtraDark="1.3"
      Exp_Dark="0.437" Exp_Light="0.001" Exp_ExtraLight="-0.8"/>
  <bloom>
      <cutoff value="1.0"/>
      <curve lowerGradient="0.5" upperGradient="2.0"/>
      <scale value="1.5"/>
      <persist value="0.001"/>
      <weights layer0="1.0" layer1="2.0" layer2="3.0" layer3="4.0" layer4="5.0"/>
  </bloom>
  <vignette>
    <angle value="10.0"/>
    <pos x="0.1" y="0.2"/>
    <scale x="1.0" y="2.0"/>
    <power value="1.5"/>
    <color r="0.1" g="0.2" b="0.3"/>
    <minIntensity value="0.25"/>
    <maxIntensity value="1.0"/>
  </vignette>
  <ToneMapeDelay val="0.075"/>
  <CameraExposureRange minval="-2" maxval="6"/>
  <filmicTone Version="2" white="1" whiteNight="11" shoulderStrength="0.7" shoulderStrengthNight="0.23"
      linearStrength="0.038" linearStrengthNight="0.1" linearAngle="0.634" linearAngleNight="0.334"
      toeStrength="0.22" toeStrengthNight="0.32" toeNumerator="0.003" toeDenominator="0.107" exposure="0.31"/>
  <ExposureBias value="0.0"/>
  <Contrast value="0.1"/>
  <Brightness value="0.2"/>
</TrackSettings>)";

fh::PostTemplates templates()
{
    return fh::parsePostTemplates(kBloom, kColour, kVignette, kFog);
}

/// A DDS volume of 16x16x16 A8R8G8B8 texels holding colour(x, y, z).
template <typename F> std::vector<std::uint8_t> ddsVolume(F colour)
{
    std::vector<std::uint8_t> dds(128 + 16 * 16 * 16 * 4, 0);
    const auto put = [&](std::size_t at, std::uint32_t value) {
        for (std::size_t i = 0; i < 4; ++i) {
            dds[at + i] = static_cast<std::uint8_t>(value >> (8 * i));
        }
    };
    dds[0] = 'D';
    dds[1] = 'D';
    dds[2] = 'S';
    dds[3] = ' ';
    put(4, 124);
    put(8, 0x801007);
    put(12, 16);
    put(16, 16);
    put(24, 16);
    put(76, 32);
    put(80, 0x41);
    put(88, 32);
    put(92, 0xFF0000);
    put(96, 0xFF00);
    put(100, 0xFF);
    put(104, 0xFF000000);
    put(108, 0x1002);
    put(112, 0x200000);
    for (std::uint32_t z = 0; z < 16; ++z) {
        for (std::uint32_t y = 0; y < 16; ++y) {
            for (std::uint32_t x = 0; x < 16; ++x) {
                const std::array<std::uint8_t, 3> c = colour(x, y, z);
                put(128 + ((z * 16 + y) * 16 + x) * 4,
                    0xFF000000U | (std::uint32_t(c[0]) << 16) | (std::uint32_t(c[1]) << 8) | c[2]);
            }
        }
    }
    return dds;
}

fh::GradingLut identityLut()
{
    return fh::parseGradingLut(ddsVolume([](std::uint32_t x, std::uint32_t y, std::uint32_t z) {
        return std::array<std::uint8_t, 3>{
            static_cast<std::uint8_t>(x * 17), static_cast<std::uint8_t>(y * 17), static_cast<std::uint8_t>(z * 17)};
    }),
        "identity");
}

TEST_CASE("templatesReadTheAttributesTheGameReads", "[postprocess]")
{
    const fh::PostTemplates t = templates();
    CHECK(t.bloom.size() == std::size_t{2});
    CHECK(t.bloom[0].name == std::string("Hazy"));
    CHECK_THAT(t.bloom[0].cutoff, WithinAbs(1.0, 1e-6));
    CHECK_THAT(t.bloom[0].scale, WithinAbs(1.2, 1e-6));
    CHECK_THAT(t.bloom[0].persist, WithinAbs(0.001, 1e-7));

    // The colour settings' blue post modulation comes from the red
    // attribute, as the game's loader reads it.
    const fh::PostTemplates::ColourSettings& c = t.colourSettings.at(0);
    CHECK_THAT(c.desaturation, WithinAbs(0.9, 1e-6));
    CHECK_THAT(c.luminosity.y, WithinAbs(0.71, 1e-6));
    CHECK_THAT(c.postMod.x, WithinAbs(1.1, 1e-6));
    CHECK_THAT(c.postMod.y, WithinAbs(1.2, 1e-6));
    CHECK_THAT(c.postMod.z, WithinAbs(1.1, 1e-6));
    CHECK_THAT(c.exposureBias, WithinAbs(0.5, 1e-6));
    CHECK_THAT(c.brightness, WithinAbs(0.125, 1e-6));
    CHECK_THAT(c.contrast, WithinAbs(0.25, 1e-6));

    const fh::VignetteSettings& v = t.vignette.at(0).settings;
    CHECK_THAT(v.angleDegrees, WithinAbs(-2.3, 1e-6));
    CHECK_THAT(v.position.y, WithinAbs(0.2, 1e-6));
    CHECK_THAT(v.scale.x, WithinAbs(0.01, 1e-6));
    CHECK_THAT(v.power, WithinAbs(2.0, 1e-6));
    CHECK_THAT(v.colour.g, WithinAbs(0.811, 1e-6));
    CHECK_THAT(v.minIntensity, WithinAbs(0.75, 1e-6));

    // The fog loader's own defaults stand for missing attributes.
    const auto& f = t.fog.at(0).settings;
    CHECK_THAT(f.colour.r, WithinAbs(0.84, 1e-6));
    CHECK_THAT(f.colour.g, WithinAbs(0.5, 1e-6));
    CHECK_THAT(f.density, WithinAbs(2.35, 1e-6));
    CHECK_THAT(f.startDistance, WithinAbs(10.0, 1e-6));
    CHECK_THAT(f.scatterPower, WithinAbs(10.0, 1e-6));
    CHECK_THAT(f.scatterStartDistance, WithinAbs(0.0, 1e-6));
    CHECK_THAT(f.scatterRampDistance, WithinAbs(2000.0, 1e-6));
    CHECK_THAT(f.heightFadeAmount, WithinAbs(0.8, 1e-6));
    CHECK_THAT(f.heightFadeStart, WithinAbs(100.0, 1e-6));
    CHECK_THAT(f.heightFadeEnd, WithinAbs(400.0, 1e-6));

    // Bloom, colour and vignette templates have no defaults in the game.
    bool threw = false;
    try {
        fh::parsePostTemplates(
            R"(<Templates><Template Name="X" Cutoff="1" Scale="1"/></Templates>)", kColour, kVignette, kFog);
    } catch (const fh::DataError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("zonesResolveTheirTemplatesAndKeepTheirTriangles", "[postprocess]")
{
    const std::vector<PostZone> zones = fh::parsePostZones(kZones, templates());
    // The zone naming a missing template is dropped, as the loader drops it.
    CHECK(zones.size() == std::size_t{2});
    if (zones.size() != 2) {
        return;
    }
    const PostZone& a = zones[0];
    CHECK(a.name == std::string("A"));
    CHECK(a.bloom == std::size_t{0});
    CHECK(a.vignetteTop == std::size_t{0});
    CHECK(a.vignetteBottom == std::size_t{1});
    CHECK(a.gradingMapNight == std::string("an.dds"));
    CHECK_THAT(a.lerpTime, WithinAbs(10.0, 1e-6));
    // NumTriangles caps the triangles read; a fourth point is ignored.
    CHECK(a.triangles.size() == std::size_t{1});
    CHECK(a.contains({10.0F, 10.0F}));
    CHECK(!a.contains({1050.0F, 1050.0F}));
    CHECK(!a.contains({-10.0F, 10.0F}));
    // A point on an edge is outside.
    CHECK(!a.contains({50.0F, 0.0F}));
    // Either winding counts.
    CHECK(zones[1].contains({90.0F, 90.0F}));
}

TEST_CASE("zoneBlendSnapsFromOutsideAndEasesBetweenZones", "[postprocess]")
{
    const std::vector<PostZone> zones = fh::parsePostZones(kZones, templates());
    PostZoneTracker tracker;
    const float dt = 0.5F;
    tracker.update(zones, glm::vec2(-50.0F, -50.0F), dt);
    CHECK(tracker.target() == PostZoneTracker::kNone);
    CHECK_THAT(tracker.blend(zones), WithinAbs(1.0, 1e-6));

    // Entering a zone from outside every zone snaps.
    tracker.update(zones, glm::vec2(10.0F, 10.0F), dt);
    CHECK(tracker.target() == 0);
    CHECK(tracker.source() == PostZoneTracker::kNone);
    CHECK_THAT(tracker.blend(zones), WithinAbs(1.0, 1e-6));

    // From A into B the blend starts again and eases over B's 4 s.
    tracker.update(zones, glm::vec2(90.0F, 90.0F), dt);
    CHECK(tracker.target() == 1);
    CHECK(tracker.source() == 0);
    CHECK_THAT(tracker.elapsed(), WithinAbs(0.0, 1e-6));
    CHECK_THAT(tracker.blend(zones), WithinAbs(0.0, 1e-6));
    tracker.update(zones, glm::vec2(90.0F, 90.0F), 1.0F);
    tracker.update(zones, glm::vec2(90.0F, 90.0F), 1.0F);
    // Half way through: smoothstep(0.5) is 0.5.
    CHECK_THAT(tracker.blend(zones), WithinAbs(0.5, 1e-6));
    tracker.update(zones, glm::vec2(90.0F, 90.0F), 0.5F);
    const float x = 2.5F / 4.0F;
    CHECK_THAT(tracker.blend(zones), WithinAbs(x * x * (3.0F - 2.0F * x), 1e-6));

    // Turning back into A mirrors the time: 2.5 s of B's 4 s leaves 3/8 of
    // A's 10 s to run back.
    tracker.update(zones, glm::vec2(10.0F, 10.0F), dt);
    CHECK(tracker.target() == 0);
    CHECK(tracker.source() == 1);
    CHECK_THAT(tracker.elapsed(), WithinAbs((1.0 - 2.5 / 4.0) * 10.0, 1e-5));

    // No player: everything back to the settings outside every zone.
    tracker.update(zones, std::nullopt, dt);
    CHECK(tracker.target() == PostZoneTracker::kNone);
    CHECK(tracker.source() == PostZoneTracker::kNone);
    CHECK_THAT(tracker.blend(zones), WithinAbs(1.0, 1e-6));
}

TEST_CASE("trackSettingsGiveTheDefaultBlock", "[postprocess]")
{
    const fh::TrackPostSettings track = fh::parseTrackPostSettings(kTrackSettings);
    const fh::PostSettings& s = track.settings;
    CHECK_THAT(s.bloomCutoff, WithinAbs(1.0, 1e-6));
    CHECK_THAT(s.bloomLowerGradient, WithinAbs(0.5, 1e-6));
    CHECK_THAT(s.bloomUpperGradient, WithinAbs(2.0, 1e-6));
    CHECK_THAT(s.bloomScale, WithinAbs(1.5, 1e-6));
    CHECK_THAT(s.bloomWeights[4], WithinAbs(5.0, 1e-6));
    CHECK_THAT(s.desatPostMod.x, WithinAbs(1.04, 1e-6));
    CHECK_THAT(s.contrast, WithinAbs(0.1, 1e-6));
    CHECK_THAT(s.brightness, WithinAbs(0.2, 1e-6));
    CHECK_THAT(s.tonemapDelay, WithinAbs(0.075, 1e-7));
    CHECK_THAT(s.adaptiveKeys.w, WithinAbs(65.0, 1e-6));
    CHECK_THAT(s.adaptiveExposures.y, WithinAbs(0.437, 1e-6));
    CHECK_THAT(s.filmic.shoulderStrength, WithinAbs(0.7, 1e-6));
    CHECK_THAT(s.filmic.exposure, WithinAbs(0.31, 1e-6));
    // The bottom vignette starts as a copy of the top.
    CHECK_THAT(s.vignetteTop.angleDegrees, WithinAbs(10.0, 1e-6));
    CHECK_THAT(s.vignetteBottom.scale.y, WithinAbs(2.0, 1e-6));
    CHECK_THAT(s.vignetteBottom.minIntensity, WithinAbs(0.25, 1e-6));
    // The night curve: its own attributes over the day's.
    CHECK_THAT(track.filmicNight.white, WithinAbs(11.0, 1e-6));
    CHECK_THAT(track.filmicNight.shoulderStrength, WithinAbs(0.23, 1e-6));
    CHECK_THAT(track.filmicNight.toeNumerator, WithinAbs(0.003, 1e-7));
    CHECK_THAT(track.filmicNight.exposure, WithinAbs(0.31, 1e-6));
}

TEST_CASE("gradingTablesReadAndBlendAsTheGameRebuildsThem", "[postprocess]")
{
    const fh::GradingLut identity = identityLut();
    CHECK(identity[(((5 * 16) + 3) * 16 + 2) * 4 + 0] == std::uint8_t{34});
    CHECK(identity[(((5 * 16) + 3) * 16 + 2) * 4 + 1] == std::uint8_t{51});
    CHECK(identity[(((5 * 16) + 3) * 16 + 2) * 4 + 2] == std::uint8_t{85});
    const fh::GradingLut red = fh::parseGradingLut(ddsVolume([](std::uint32_t, std::uint32_t y, std::uint32_t z) {
        return std::array<std::uint8_t, 3>{255, static_cast<std::uint8_t>(y * 17), static_cast<std::uint8_t>(z * 17)};
    }),
        "red");
    // Day fully graded: the day table of the zone blended towards.
    const fh::GradingLut day = fh::blendGradingLuts(identity, identity, red, identity, 1.0F, 1.0F, 0.0F);
    CHECK(day == red);
    // Nothing graded: identity whatever the tables.
    const fh::GradingLut none = fh::blendGradingLuts(red, red, red, red, 0.3F, 0.0F, 0.0F);
    CHECK(none == identity);
    // Half way from identity to red: red at texel x = 0 is 0 + 0.5 * 1.
    const fh::GradingLut half = fh::blendGradingLuts(identity, identity, red, identity, 0.5F, 1.0F, 0.0F);
    CHECK(half[0] == std::uint8_t{128});
    CHECK(half[1] == std::uint8_t{0});

    bool threw = false;
    try {
        std::vector<std::uint8_t> flat = ddsVolume(
            [](std::uint32_t, std::uint32_t, std::uint32_t) { return std::array<std::uint8_t, 3>{0, 0, 0}; });
        // Not a volume: DDSCAPS2_VOLUME cleared.
        flat[114] = 0;
        fh::parseGradingLut(flat, "flat");
    } catch (const fh::DataError&) {
        threw = true;
    }
    CHECK(threw);
}

TEST_CASE("filmicCurveAndExposureFollowTheCombine", "[postprocess]")
{
    post::Filmic k{1.0F, 0.7F, 0.038F, 0.634F, 0.22F, 0.003F, 0.107F};
    const auto expected = [&](float x) {
        const float a = k.a;
        const float b = k.b;
        const float c = k.c;
        const float d = k.d;
        const float e = k.e;
        const float f = k.f;
        return (x * (a * x + c * b) + d * e) / (x * (a * x + b) + d * f) - e / f;
    };
    CHECK_THAT(post::filmic(k, 0.0F), WithinAbs(0.0, 1e-6));
    CHECK_THAT(post::filmic(k, 0.5F), WithinAbs(expected(0.5F), 1e-6));
    CHECK_THAT(post::filmic(k, 1.0F), WithinAbs(expected(1.0F), 1e-6));

    const glm::vec4 keys = glm::vec4(0.0F, 4.0F, 13.0F, 65.0F) / 255.0F;
    const glm::vec4 exposures(1.3F, 0.437F, 0.001F, -0.8F);
    // Below the dark key: the first segment.
    CHECK_THAT(post::exposureLog2(0.0F, keys, exposures, 0.0F), WithinAbs(1.3, 1e-6));
    CHECK_THAT(post::exposureLog2(2.0F / 255.0F, keys, exposures, 0.0F), WithinAbs(1.3 + 0.5 * (0.437 - 1.3), 1e-5));
    // Between dark and light: the middle segment.
    CHECK_THAT(
        post::exposureLog2(8.5F / 255.0F, keys, exposures, 0.0F), WithinAbs(0.437 + 0.5 * (0.001 - 0.437), 1e-5));
    // At or above the light key: the last, clamped at its end.
    CHECK_THAT(post::exposureLog2(13.0F / 255.0F, keys, exposures, 0.0F), WithinAbs(0.001, 1e-6));
    CHECK_THAT(post::exposureLog2(1.0F, keys, exposures, 0.31F), WithinAbs(-0.8 + 0.31, 1e-6));
}

TEST_CASE("colourTransformAppliesContrastBrightnessSaturationAndScale", "[postprocess]")
{
    const glm::vec3 w(0.2126F, 0.7152F, 0.0722F);
    const glm::vec3 c(0.2F, 0.5F, 0.9F);
    const auto apply = [](const glm::mat4& m, const glm::vec3& v) { return glm::vec3(m * glm::vec4(v, 1.0F)); };
    const glm::vec3 same = apply(post::colourTransform(0.0F, 0.0F, 1.0F, w, glm::vec3(1.0F)), c);
    CHECK_THAT(same.x, WithinAbs(0.2, 1e-6));
    CHECK_THAT(same.z, WithinAbs(0.9, 1e-6));
    // Contrast k: (c - 0.5) (1 + k) + 0.5.
    const glm::vec3 contrast = apply(post::colourTransform(0.5F, 0.0F, 1.0F, w, glm::vec3(1.0F)), c);
    CHECK_THAT(contrast.x, WithinAbs((0.2 - 0.5) * 1.5 + 0.5, 1e-6));
    // Then brightness adds, then saturation s c + (1 - s) dot(c, w), then
    // the post modulation scales.
    const float s = 0.9F;
    const glm::vec3 mod(1.04F, 1.02F, 1.0F);
    const glm::vec3 all = apply(post::colourTransform(0.5F, 0.1F, s, w, mod), c);
    const glm::vec3 afterContrast = (c - 0.5F) * 1.5F + 0.5F + 0.1F;
    const float grey = glm::dot(afterContrast, w);
    const glm::vec3 wanted = (afterContrast * s + (1.0F - s) * grey) * mod;
    CHECK_THAT(all.x, WithinAbs(wanted.x, 1e-5));
    CHECK_THAT(all.y, WithinAbs(wanted.y, 1e-5));
    CHECK_THAT(all.z, WithinAbs(wanted.z, 1e-5));
}

TEST_CASE("vignetteTransformRotatesAndScales", "[postprocess]")
{
    const glm::vec4 t = post::vignetteTransform(0.0F, glm::vec2(0.5F, 2.0F), 16.0F / 9.0F);
    CHECK_THAT(t.x, WithinAbs(0.5 * 16.0 / 9.0, 1e-6));
    CHECK_THAT(t.y, WithinAbs(0.0, 1e-6));
    CHECK_THAT(t.z, WithinAbs(0.0, 1e-6));
    CHECK_THAT(t.w, WithinAbs(2.0, 1e-6));
    const float a = 0.3F;
    const glm::vec4 r = post::vignetteTransform(a, glm::vec2(1.0F), 1.0F);
    CHECK_THAT(r.x, WithinAbs(std::cos(a), 1e-6));
    CHECK_THAT(r.y, WithinAbs(std::sin(a), 1e-6));
    CHECK_THAT(r.z, WithinAbs(-std::sin(a), 1e-6));
    CHECK_THAT(r.w, WithinAbs(std::cos(a), 1e-6));
}

struct Picture {
    GLuint texture = 0;
    GLuint framebuffer = 0;
};

Picture picture(int width, int height, GLenum format)
{
    Picture p;
    glCreateTextures(GL_TEXTURE_2D, 1, &p.texture);
    glTextureStorage2D(p.texture, 1, format, width, height);
    glCreateFramebuffers(1, &p.framebuffer);
    glNamedFramebufferTexture(p.framebuffer, GL_COLOR_ATTACHMENT0, p.texture, 0);
    return p;
}

void release(Picture& p)
{
    glDeleteFramebuffers(1, &p.framebuffer);
    glDeleteTextures(1, &p.texture);
}

/// The combine's output for a uniform scene of square-root colour `s` with
/// no bloom, grading or vignette, from the traced formulas.
float expectedUniform(float s, const fh::render::PostFrame& frame, float bloomSquared)
{
    const float linear = s * s + bloomSquared;
    const float luminance = s * s;
    const float x
        = linear * std::exp2(post::exposureLog2(luminance, frame.luminanceKeys, frame.exposures, frame.exposureBias));
    const float mapped = post::filmic(frame.filmic, x) / post::filmic(frame.filmic, frame.filmic.white);
    return std::sqrt(std::clamp(mapped, 0.0F, 1.0F));
}

fh::render::PostFrame uniformFrame()
{
    fh::render::PostFrame f;
    f.luminanceKeys = glm::vec4(0.0F, 4.0F, 13.0F, 65.0F) / 255.0F;
    f.exposures = glm::vec4(1.3F, 0.437F, 0.001F, -0.8F);
    f.exposureBias = 0.31F;
    f.filmic = post::Filmic{1.0F, 0.7F, 0.038F, 0.634F, 0.22F, 0.003F, 0.107F};
    f.adaptation = 0.075F;
    f.resetAdaptation = true;
    f.bloomWeights = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    return f;
}

TEST_CASE("gpuChainTonemapsAUniformPictureAsTheFormulasDo", "[postprocess]")
{
    fh::test::requireGlContext();
    constexpr int kWidth = 160;
    constexpr int kHeight = 90;
    Picture scene = picture(kWidth, kHeight, GL_RGBA16F);
    Picture out = picture(kWidth, kHeight, GL_RGBA8);
    const float s = 0.5F;
    const float colour[4] = {s, s, s, 1.0F};
    glClearNamedFramebufferfv(scene.framebuffer, GL_COLOR, 0, colour);
    fh::render::PostProcess postProcess;
    const auto centre = [&] {
        std::array<std::uint8_t, 4> pixel{};
        glBindFramebuffer(GL_FRAMEBUFFER, out.framebuffer);
        glReadPixels(kWidth / 2, kHeight / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        return static_cast<float>(pixel[0]) / 255.0F;
    };

    // No bloom (scale 0): the tonemapped scene alone.
    fh::render::PostFrame frame = uniformFrame();
    postProcess.run(scene.texture, kWidth, kHeight, out.framebuffer, frame);
    CHECK_THAT(centre(), WithinAbs(expectedUniform(s, frame, 0.0F), 1.5 / 255.0));

    // Bloom: a uniform hot extract survives the blurs (their weights sum
    // to 1), so the bloom squared is 0.2 times the weights' sum times it.
    frame.bloomScale = 1.0F;
    frame.bloomCutoff = 1.0F;
    frame.bloomLowerGradient = 1.0F;
    frame.bloomUpperGradient = 1.0F;
    postProcess.run(scene.texture, kWidth, kHeight, out.framebuffer, frame);
    const float exposure
        = std::exp2(post::exposureLog2(s * s, frame.luminanceKeys, frame.exposures, frame.exposureBias));
    const float hot = std::max(s * s * exposure - 1.0F / 255.0F, 0.0F);
    CHECK_THAT(centre(), WithinAbs(expectedUniform(s, frame, 0.2F * 5.0F * hot), 1.5 / 255.0));

    // The adapted luminance moves by 0.075 of the way per frame after a
    // reset: a darker scene is exposed with a luminance barely changed.
    const float dark = 0.1F;
    const float darkColour[4] = {dark, dark, dark, 1.0F};
    glClearNamedFramebufferfv(scene.framebuffer, GL_COLOR, 0, darkColour);
    frame = uniformFrame();
    frame.resetAdaptation = false;
    postProcess.run(scene.texture, kWidth, kHeight, out.framebuffer, frame);
    const float adapted = s * s + (dark * dark - s * s) * 0.075F;
    const float x = dark * dark
        * std::exp2(post::exposureLog2(adapted, frame.luminanceKeys, frame.exposures, frame.exposureBias));
    const float wanted
        = std::sqrt(std::clamp(post::filmic(frame.filmic, x) / post::filmic(frame.filmic, 1.0F), 0.0F, 1.0F));
    CHECK_THAT(centre(), WithinAbs(wanted, 1.5 / 255.0));

    release(scene);
    release(out);
}

TEST_CASE("gpuCombineDrawsEachHalfWithItsVignette", "[postprocess]")
{
    fh::test::requireGlContext();
    constexpr int kWidth = 64;
    constexpr int kHeight = 36;
    Picture scene = picture(kWidth, kHeight, GL_RGBA16F);
    Picture out = picture(kWidth, kHeight, GL_RGBA8);
    const float colour[4] = {0.5F, 0.5F, 0.5F, 1.0F};
    glClearNamedFramebufferfv(scene.framebuffer, GL_COLOR, 0, colour);
    fh::render::PostFrame frame = uniformFrame();
    // A top vignette with power 0 covers everything: black at full
    // strength. The bottom one is off.
    frame.vignetteTop.transform = glm::vec4(1.0F, 0.0F, 0.0F, 1.0F);
    frame.vignetteTop.power = 0.0F;
    frame.vignetteTop.colour = glm::vec4(0.0F, 0.0F, 0.0F, 1.0F);
    fh::render::PostProcess postProcess;
    postProcess.run(scene.texture, kWidth, kHeight, out.framebuffer, frame);
    std::array<std::uint8_t, 4> top{};
    std::array<std::uint8_t, 4> bottom{};
    glBindFramebuffer(GL_FRAMEBUFFER, out.framebuffer);
    glReadPixels(kWidth / 2, kHeight - 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top.data());
    glReadPixels(kWidth / 2, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom.data());
    CHECK(top[0] == std::uint8_t{0});
    CHECK_THAT(static_cast<float>(bottom[0]) / 255.0F, WithinAbs(expectedUniform(0.5F, frame, 0.0F), 1.5 / 255.0));
    release(scene);
    release(out);
}

// Not a check: the GPU time of the whole chain at the console's 1280x720,
// for the record.
TEST_CASE("gpuChainTimeAt720p", "[postprocess]")
{
    fh::test::requireGlContext();
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    Picture scene = picture(kWidth, kHeight, GL_RGBA16F);
    Picture out = picture(kWidth, kHeight, GL_RGBA8);
    const float colour[4] = {0.5F, 0.4F, 0.3F, 1.0F};
    glClearNamedFramebufferfv(scene.framebuffer, GL_COLOR, 0, colour);
    fh::render::PostProcess postProcess;
    fh::render::PostFrame frame = uniformFrame();
    frame.bloomScale = 1.0F;
    const fh::GradingLut lut = identityLut();
    frame.gradingLut = lut;
    GLuint query = 0;
    glGenQueries(1, &query);
    double total = 0.0;
    constexpr int kRuns = 50;
    for (int i = 0; i < kRuns + 5; ++i) {
        glBeginQuery(GL_TIME_ELAPSED, query);
        postProcess.run(scene.texture, kWidth, kHeight, out.framebuffer, frame);
        glEndQuery(GL_TIME_ELAPSED);
        GLuint64 ns = 0;
        glGetQueryObjectui64v(query, GL_QUERY_RESULT, &ns);
        if (i >= 5) {
            total += static_cast<double>(ns);
        }
    }
    std::fprintf(stderr, "post-processing at 1280x720: %.3f ms GPU\n", total / kRuns / 1.0e6);
    glDeleteQueries(1, &query);
    release(scene);
    release(out);
}

/// The whole combine for a uniform scene of square-root colour `s` on the
/// first frame after a reset, from the traced formulas: the bloom of the
/// hot extract with no persistence, the exposure from the scene's own
/// luminance, the filmic curve, the grading table sampled trilinearly at
/// the texel centres, the vignette at `v` (the pixel's offset from the
/// picture's centre, y down) and the colour matrix. Red channel, 0..1.
float referenceCombine(float s, const fh::render::PostFrame& f, const fh::render::PostVignette& vignette, glm::vec2 v)
{
    const float luminance = s * s;
    const float exposure = std::exp2(post::exposureLog2(luminance, f.luminanceKeys, f.exposures, f.exposureBias));
    const float x = s * s * exposure;
    const float l = x;
    const float gain = l > 0.0F
        ? std::max(l * f.bloomUpperGradient + (f.bloomLowerGradient - f.bloomUpperGradient) * f.bloomCutoff,
              l * f.bloomLowerGradient)
            / l
        : 0.0F;
    const float hot = std::clamp(std::max(x * gain - 1.0F / 255.0F, 0.0F) * f.bloomScale, 0.0F, 16.0F);
    float weights = 0.0F;
    for (float w : f.bloomWeights) {
        weights += w;
    }
    const float bloom = 0.2F * weights * hot;
    const float exposed = (s * s + bloom) * exposure;
    float mapped = post::filmic(f.filmic, exposed) / post::filmic(f.filmic, f.filmic.white);
    glm::vec3 c(mapped);
    if (!f.gradingLut.empty()) {
        const auto texel = [&](int xi, int yi, int zi, int channel) {
            return static_cast<float>(f.gradingLut[static_cast<std::size_t>(((zi * 16 + yi) * 16 + xi) * 4 + channel)])
                / 255.0F;
        };
        // Texel coordinate of mapped * 15/16 + 1/32 over 16 texels: mapped * 15.
        const float t = std::clamp(mapped, 0.0F, 1.0F) * 15.0F;
        const int i0 = std::min(static_cast<int>(t), 14);
        const float fr = t - static_cast<float>(i0);
        glm::vec3 out(0.0F);
        for (int corner = 0; corner < 8; ++corner) {
            const int dx = corner & 1;
            const int dy = (corner >> 1) & 1;
            const int dz = (corner >> 2) & 1;
            const float w = (dx ? fr : 1.0F - fr) * (dy ? fr : 1.0F - fr) * (dz ? fr : 1.0F - fr);
            for (int ch = 0; ch < 3; ++ch) {
                out[ch] += w * texel(i0 + dx, i0 + dy, i0 + dz, ch);
            }
        }
        c = out;
    }
    c = glm::sqrt(glm::abs(c));
    const glm::vec2 p = v + vignette.position;
    const glm::vec2 d(glm::dot(p, glm::vec2(vignette.transform.x, vignette.transform.y)),
        glm::dot(p, glm::vec2(vignette.transform.z, vignette.transform.w)));
    const float fade = std::clamp(std::exp2(0.5F * vignette.power * std::log2(glm::dot(d, d))), 0.0F, 1.0F);
    c = glm::mix(c, glm::clamp(c + glm::vec3(vignette.colour) - 1.0F, 0.0F, 1.0F), fade * vignette.colour.a);
    const glm::vec3 graded = glm::clamp(glm::vec3(f.colourTransform * glm::vec4(c * c, 1.0F)), 0.0F, 1.0F);
    return std::sqrt(graded.r);
}

// "The post-processing crushes the low end": a sunlit road and a shadowed
// one, 0.22 and 0.10 in the square-root scene target, through the whole
// chain with Colorado's noon settings at the free-roam start (the
// Inner_Festival zone), against the traced formulas.
TEST_CASE("coloradoNoonShadowAndSunComeOutAsTheFormulasGive", "[postprocess]")
{
    const char* dir = fh::test::gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(fh::pathFromUtf8(dir));
    fh::PostProcessing p = fh::PostProcessing::load(vfs, "colorado");
    const fh::TimeOfDayCurves curves = fh::TimeOfDayCurves::load(vfs, "colorado");
    p.update(glm::vec2(-1366.0F, -377.0F), 1.0F / 60.0F, curves, 12.0F * 60.0F);
    CHECK(std::string(p.zoneName()) == std::string("Inner_Festival"));
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    const fh::render::PostFrame frame = p.frame(static_cast<float>(kWidth) / static_cast<float>(kHeight), true);
    Picture scene = picture(kWidth, kHeight, GL_RGBA16F);
    Picture out = picture(kWidth, kHeight, GL_RGBA8);
    // A pixel low in the picture, where the road is: in the bottom half.
    const int px = kWidth / 2;
    const int py = kHeight / 4;
    const glm::vec2 v(
        (static_cast<float>(px) + 0.5F) / kWidth - 0.5F, 1.0F - (static_cast<float>(py) + 0.5F) / kHeight - 0.5F);
    std::array<float, 2> measured{};
    std::array<float, 2> expected{};
    const std::array<float, 2> values{0.22F, 0.10F};
    for (std::size_t i = 0; i < values.size(); ++i) {
        const float colour[4] = {values[i], values[i], values[i], 1.0F};
        glClearNamedFramebufferfv(scene.framebuffer, GL_COLOR, 0, colour);
        fh::render::PostProcess postProcess;
        postProcess.run(scene.texture, kWidth, kHeight, out.framebuffer, frame);
        std::array<std::uint8_t, 4> pixel{};
        glBindFramebuffer(GL_FRAMEBUFFER, out.framebuffer);
        glReadPixels(px, py, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
        measured[i] = static_cast<float>(pixel[0]) / 255.0F;
        expected[i] = referenceCombine(values[i], frame, frame.vignetteBottom, v);
        std::fprintf(stderr, "scene %.2f: chain %.0f, formulas %.1f (of 255)\n", values[i], measured[i] * 255.0F,
            expected[i] * 255.0F);
        CHECK_THAT(measured[i], WithinAbs(expected[i], 2.0 / 255.0));
    }
    release(scene);
    release(out);
}

TEST_CASE("coloradoPostProcessingLoadsAndFollowsItsZones", "[postprocess]")
{
    const char* dir = fh::test::gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(fh::pathFromUtf8(dir));
    fh::PostProcessing p = fh::PostProcessing::load(vfs, "colorado");
    // Every one of the sixteen zones names templates that exist.
    CHECK(p.zones().size() == std::size_t{16});
    const fh::TimeOfDayCurves curves = fh::TimeOfDayCurves::load(vfs, "colorado");
    const float noon = 12.0F * 60.0F;
    // Outside every zone: TrackSettings' block times the curves.
    p.update(glm::vec2(20000.0F, 20000.0F), 1.0F / 60.0F, curves, noon);
    CHECK(p.zoneName().empty());
    CHECK_THAT(p.settings().bloomScale, WithinAbs(1.0 * curves.sampleScalar("BloomScaleMultiplier", noon), 1e-5));
    CHECK_THAT(p.settings().filmic.shoulderStrength, WithinAbs(0.700001, 1e-5));
    // The fog outside every zone multiplies the curves by one and scatters
    // the sun from 2000 m over 2000 m.
    CHECK_THAT(p.fog().density, WithinAbs(1.0, 1e-6));
    CHECK_THAT(p.fog().scatterStartDistance, WithinAbs(2000.0, 1e-6));
    // Entering Redrock (the centroid of its first triangle) snaps to its
    // Hot_hazy bloom, Saturated colour settings and Redrock fog.
    p.update(glm::vec2(-6384.0F, -4888.0F), 1.0F / 60.0F, curves, noon);
    CHECK(std::string(p.zoneName()) == std::string("Redrock"));
    CHECK_THAT(p.settings().bloomScale, WithinAbs(1.0 * curves.sampleScalar("BloomScaleMultiplier", noon), 1e-5));
    CHECK_THAT(p.settings().bloomCutoff, WithinAbs(1.0 * curves.sampleScalar("BloomCutoffMultiplier", noon), 1e-5));
    CHECK_THAT(p.fog().density, WithinAbs(2.35, 1e-5));
    CHECK_THAT(p.settings().vignetteTop.angleDegrees, WithinAbs(-2.3, 1e-5));
    CHECK_THAT(p.settings().vignetteBottom.minIntensity, WithinAbs(0.749, 1e-5));
    // At noon the day grading applies in full: the Redrock table, not the
    // identity.
    const fh::GradingLut identity = identityLut();
    CHECK(p.gradingLut() != identity);
    // At night the filmic curve is the night one.
    const float midnight = 0.0F;
    p.update(glm::vec2(-6384.0F, -4888.0F), 1.0F / 60.0F, curves, midnight);
    CHECK_THAT(p.settings().filmic.white, WithinAbs(11.0, 1e-5));
}

} // namespace

namespace {

/// Keeps the cases' shader programs out of the player's own cache.
class CacheFolder : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo& /*info*/) override
    {
        fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Cache),
            fh::pathToUtf8(std::filesystem::temp_directory_path() / "fh1-tst-postprocess-cache"));
    }
};

} // namespace

CATCH_REGISTER_LISTENER(CacheFolder)
