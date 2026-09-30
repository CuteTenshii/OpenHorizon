// The race cutscenes as the game plays them: the files a session loads,
// the race-specific lookup, the clock and its looping, the triggers the
// flows wait on, the cuts' fades and the keyed cameras. On synthetic
// cutscenes, and on the disc's when FH1_GAME_DIR names it.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "game/Cutscenes.h"
#include "game/ScreenFade.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

using Catch::Matchers::WithinAbs;

using openhorizon::test::gameDir;

namespace {

constexpr float kStep = 1.0F / 60.0F;

openhorizon::CutsceneKey key(float time, glm::vec3 position, float yaw = 0.0F, float pitch = 0.0F, float fov = 30.0F)
{
    openhorizon::CutsceneKey k;
    k.time = time;
    k.position = position;
    k.yaw = yaw;
    k.pitch = pitch;
    k.fov = fov;
    return k;
}

/// One keyed cut of `duration` seconds around the player's car.
openhorizon::Cutscene singleCut(float duration, std::vector<openhorizon::CutsceneKey> keys)
{
    openhorizon::Cutscene cutscene;
    cutscene.name = "test";
    cutscene.duration = duration;
    openhorizon::CutsceneCamera camera;
    camera.type = "Animateable";
    camera.duration = duration;
    camera.keys = std::move(keys);
    cutscene.cameras.push_back(camera);
    return cutscene;
}

void play(openhorizon::CutscenePlayer& player, float seconds)
{
    for (float t = 0.0F; t < seconds - 1e-4F; t += kStep) {
        player.advance(kStep);
    }
}

} // namespace

TEST_CASE("cutscene_clock_holds_or_loops", "[cutscenes]")
{
    openhorizon::Cutscene normal;
    normal.duration = 8.0F;
    CHECK_THAT(normal.timeAt(3.0F), WithinAbs(3.0F, 1e-6F));
    CHECK_THAT(normal.timeAt(20.0F), WithinAbs(8.0F, 1e-6F));
    openhorizon::Cutscene looping;
    looping.looping = true;
    looping.duration = 27.0F;
    looping.loopFrom = 7.0F;
    CHECK_THAT(looping.timeAt(26.0F), WithinAbs(26.0F, 1e-4F));
    CHECK_THAT(looping.timeAt(30.0F), WithinAbs(10.0F, 1e-4F));
    CHECK_THAT(looping.timeAt(47.0F), WithinAbs(7.0F, 1e-4F));
}

TEST_CASE("cutscene_triggers_fire_once_as_the_clock_reaches_them", "[cutscenes]")
{
    openhorizon::Cutscene cutscene = singleCut(8.0F, {key(0.0F, glm::vec3(0.0F))});
    openhorizon::CutsceneEvent start;
    start.time = 1.3F;
    start.trigger = "CCutsceneStartCountdownTrigger";
    openhorizon::CutsceneEvent countdown;
    countdown.time = 2.75F;
    countdown.trigger = "CCutsceneCountdownTrigger";
    cutscene.events = {start, countdown};
    openhorizon::CutscenePlayer player;
    player.play(cutscene);
    play(player, 1.0F);
    CHECK((!player.startCountdownReached() && !player.countdownReached()));
    play(player, 0.5F);
    CHECK((player.startCountdownReached() && !player.countdownReached()));
    play(player, 1.5F);
    CHECK(player.countdownReached());
    CHECK(player.takeEvents().size() == std::size_t{2});
    play(player, 6.0F);
    CHECK(player.ended());
    CHECK(player.takeEvents().empty());
}

// Two keys make a straight line (both Hermite tangents are the difference
// of the keys), and a CarSpace position is the car's matrix times the key.
TEST_CASE("cutscene_camera_lerps_two_keys_in_car_space", "[cutscenes]")
{
    const openhorizon::Cutscene cutscene = singleCut(2.0F,
        {key(0.0F, glm::vec3(0.0F, 1.0F, 4.0F), 180.0F, 0.0F, 20.0F),
            key(2.0F, glm::vec3(2.0F, 1.0F, 4.0F), 180.0F, 0.0F, 30.0F)});
    openhorizon::CutscenePlayer player;
    player.play(cutscene);
    openhorizon::CutsceneScene scene;
    scene.car = glm::translate(glm::mat4(1.0F), glm::vec3(100.0F, 10.0F, -50.0F));
    play(player, 1.0F);
    const std::optional<openhorizon::CutsceneView> view = player.view(scene);
    CHECK(view.has_value());
    if (!view) {
        return;
    }
    CHECK_THAT(view->position.x, WithinAbs(101.0F, 0.05F));
    CHECK_THAT(view->position.y, WithinAbs(11.0F, 1e-3F));
    CHECK_THAT(view->position.z, WithinAbs(-46.0F, 1e-3F));
    CHECK_THAT(view->fov, WithinAbs(glm::radians(25.0F), glm::radians(0.3F)));
    // Yaw 180 looks back along the car.
    CHECK_THAT(view->forward.z, WithinAbs(-1.0F, 1e-3F));
}

// Hermite through three keys passes through the middle one with the
// neighbouring slopes' weighted tangent, so an evenly spaced ramp stays a
// straight line.
TEST_CASE("cutscene_camera_curve_passes_its_keys", "[cutscenes]")
{
    const openhorizon::Cutscene cutscene = singleCut(2.0F,
        {key(0.0F, glm::vec3(0.0F)), key(1.0F, glm::vec3(0.0F, 0.0F, 1.0F)), key(2.0F, glm::vec3(0.0F, 0.0F, 2.0F))});
    openhorizon::CutscenePlayer player;
    player.play(cutscene);
    const openhorizon::CutsceneScene scene;
    play(player, 0.5F);
    std::optional<openhorizon::CutsceneView> view = player.view(scene);
    CHECK((view && std::abs(view->position.z - 0.5F) < 0.02F));
    play(player, 0.5F);
    view = player.view(scene);
    CHECK((view && std::abs(view->position.z - 1.0F) < 0.02F));
}

// A PartSpace key is an offset from the car part it names; without the
// part's position the cut leaves the camera to the game.
TEST_CASE("cutscene_part_space_measures_from_the_car_part", "[cutscenes]")
{
    openhorizon::Cutscene cutscene = singleCut(1.0F, {key(0.0F, glm::vec3(0.5F, 0.0F, 0.0F))});
    cutscene.cameras[0].positionSpace = openhorizon::CutsceneSpace::Part;
    cutscene.cameras[0].keys[0].carPart = "headlightL";
    openhorizon::CutscenePlayer player;
    player.play(cutscene);
    player.advance(kStep);
    openhorizon::CutsceneScene scene;
    CHECK(!player.view(scene).has_value());
    scene.parts[0] = glm::vec3(-0.7F, 0.5F, 1.8F);
    const std::optional<openhorizon::CutsceneView> view = player.view(scene);
    CHECK(view.has_value());
    if (view) {
        CHECK_THAT(view->position.x, WithinAbs(-0.2F, 1e-4F));
        CHECK_THAT(view->position.z, WithinAbs(1.8F, 1e-4F));
    }
    CHECK(openhorizon::cutsceneCarPart("wheelLR") == 8);
    CHECK(openhorizon::cutsceneCarPart("roof") == -1);
}

// The intro's first cut fades in from black: opaque for its hold, then
// clear (Duration 0.5, HoldDuration 0.5, Linear: a cut at 0.5 s).
TEST_CASE("cutscene_cut_fades_in_from_black", "[cutscenes]")
{
    openhorizon::Cutscene cutscene = singleCut(2.5F, {key(0.0F, glm::vec3(0.0F))});
    cutscene.cameras[0].fadeIn = openhorizon::FadeOptions{0.5F, 0.5F, false, glm::vec3(0.0F)};
    openhorizon::CutscenePlayer player;
    player.play(cutscene);
    play(player, 0.25F);
    glm::vec3 colour(1.0F);
    CHECK_THAT(player.overlay(colour), WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(colour.x, WithinAbs(0.0F, 1e-6F));
    play(player, 0.5F);
    CHECK_THAT(player.overlay(colour), WithinAbs(0.0F, 1e-6F));
}

TEST_CASE("fade_in_opacity_follows_the_fader", "[cutscenes]")
{
    const openhorizon::FadeOptions sCurve{1.0F, 0.0F, true, glm::vec3(0.0F)};
    CHECK_THAT(openhorizon::fadeInOpacity(sCurve, 0.0F), WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(openhorizon::fadeInOpacity(sCurve, 0.5F), WithinAbs(0.5F, 1e-5F));
    CHECK_THAT(openhorizon::fadeInOpacity(sCurve, 1.0F), WithinAbs(0.0F, 1e-6F));
    const openhorizon::FadeOptions held{0.5F, 0.5F, false, glm::vec3(0.0F)};
    CHECK_THAT(openhorizon::fadeInOpacity(held, 0.49F), WithinAbs(1.0F, 1e-6F));
    CHECK_THAT(openhorizon::fadeInOpacity(held, 0.5F), WithinAbs(0.0F, 1e-6F));
}

// A race session loads the default race cutscenes and its track's file;
// FR02 runs on FESTIVAL_PLNS_006, which has its own intro, countdown and
// finish-line cutscenes, and the generic ones stand in for the rest.
TEST_CASE("disc_race_session_finds_its_own_cutscenes", "[cutscenes]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const openhorizon::CutsceneLibrary race
        = openhorizon::CutsceneLibrary::load(vfs, "colorado", true, "FESTIVAL_PLNS_006");
    const openhorizon::Cutscene* intro = race.raceSpecific("prerace_intro", "FESTIVAL_PLNS_006", "FR02", 26);
    CHECK((intro != nullptr && intro->name == "prerace_intro_FESTIVAL_PLNS_006"));
    if (intro != nullptr) {
        CHECK(intro->looping);
        CHECK_THAT(intro->firstEvent("CCutsceneCountdownTrigger").value_or(-1.0F), WithinAbs(5.0F, 1e-4F));
        CHECK(intro->cameras.front().fadeIn.has_value());
        CHECK(intro->cameras.front().target == std::string("start_location_00"));
    }
    const openhorizon::Cutscene* countdown = race.raceSpecific("prerace_countdown", "FESTIVAL_PLNS_006", "FR02", 26);
    CHECK((countdown != nullptr && countdown->name == "prerace_countdown_FESTIVAL_PLNS_006"));
    if (countdown != nullptr) {
        CHECK_THAT(countdown->firstEvent("CCutsceneStartCountdownTrigger").value_or(-1.0F), WithinAbs(1.3F, 1e-4F));
        CHECK(countdown->cameras.back().type == std::string("Follow"));
    }
    const openhorizon::Cutscene* generic = race.raceSpecific("postrace_transition_02", "FESTIVAL_PLNS_006", "FR02", 26);
    CHECK((generic != nullptr && generic->name == "postrace_transition_02"));
    CHECK(race.find("event_activate_intro") == nullptr);

    const openhorizon::CutsceneLibrary freeRoam = openhorizon::CutsceneLibrary::load(vfs, "colorado", false, {});
    const openhorizon::Cutscene* activate = freeRoam.raceSpecific("event_activate_intro", {}, "FR04", 239);
    CHECK((activate != nullptr && activate->name == "event_activate_intro"));
    if (activate != nullptr) {
        CHECK_THAT(activate->firstEvent("CCutsceneCountdownTrigger").value_or(-1.0F), WithinAbs(0.0F, 1e-6F));
    }
    CHECK(freeRoam.find("prerace_intro") == nullptr);
}
