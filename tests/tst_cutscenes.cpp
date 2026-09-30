// The race cutscenes as the game plays them: the files a session loads,
// the race-specific lookup, the clock and its looping, the triggers the
// flows wait on, the cuts' fades and the keyed cameras. On synthetic
// cutscenes, and on the disc's when FH1_GAME_DIR names it.

#include "TestHarness.h"

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

namespace {

constexpr float kStep = 1.0F / 60.0F;

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

fh::CutsceneKey key(float time, glm::vec3 position, float yaw = 0.0F, float pitch = 0.0F, float fov = 30.0F)
{
    fh::CutsceneKey k;
    k.time = time;
    k.position = position;
    k.yaw = yaw;
    k.pitch = pitch;
    k.fov = fov;
    return k;
}

/// One keyed cut of `duration` seconds around the player's car.
fh::Cutscene singleCut(float duration, std::vector<fh::CutsceneKey> keys)
{
    fh::Cutscene cutscene;
    cutscene.name = "test";
    cutscene.duration = duration;
    fh::CutsceneCamera camera;
    camera.type = "Animateable";
    camera.duration = duration;
    camera.keys = std::move(keys);
    cutscene.cameras.push_back(camera);
    return cutscene;
}

void play(fh::CutscenePlayer& player, float seconds)
{
    for (float t = 0.0F; t < seconds - 1e-4F; t += kStep) {
        player.advance(kStep);
    }
}

} // namespace

TEST(cutscene_clock_holds_or_loops)
{
    fh::Cutscene normal;
    normal.duration = 8.0F;
    CHECK_NEAR(normal.timeAt(3.0F), 3.0F, 1e-6F);
    CHECK_NEAR(normal.timeAt(20.0F), 8.0F, 1e-6F);
    fh::Cutscene looping;
    looping.looping = true;
    looping.duration = 27.0F;
    looping.loopFrom = 7.0F;
    CHECK_NEAR(looping.timeAt(26.0F), 26.0F, 1e-4F);
    CHECK_NEAR(looping.timeAt(30.0F), 10.0F, 1e-4F);
    CHECK_NEAR(looping.timeAt(47.0F), 7.0F, 1e-4F);
}

TEST(cutscene_triggers_fire_once_as_the_clock_reaches_them)
{
    fh::Cutscene cutscene = singleCut(8.0F, {key(0.0F, glm::vec3(0.0F))});
    fh::CutsceneEvent start;
    start.time = 1.3F;
    start.trigger = "CCutsceneStartCountdownTrigger";
    fh::CutsceneEvent countdown;
    countdown.time = 2.75F;
    countdown.trigger = "CCutsceneCountdownTrigger";
    cutscene.events = {start, countdown};
    fh::CutscenePlayer player;
    player.play(cutscene);
    play(player, 1.0F);
    CHECK(!player.startCountdownReached() && !player.countdownReached());
    play(player, 0.5F);
    CHECK(player.startCountdownReached() && !player.countdownReached());
    play(player, 1.5F);
    CHECK(player.countdownReached());
    CHECK_EQ(player.takeEvents().size(), std::size_t{2});
    play(player, 6.0F);
    CHECK(player.ended());
    CHECK(player.takeEvents().empty());
}

// Two keys make a straight line (both Hermite tangents are the difference
// of the keys), and a CarSpace position is the car's matrix times the key.
TEST(cutscene_camera_lerps_two_keys_in_car_space)
{
    const fh::Cutscene cutscene = singleCut(2.0F,
        {key(0.0F, glm::vec3(0.0F, 1.0F, 4.0F), 180.0F, 0.0F, 20.0F),
            key(2.0F, glm::vec3(2.0F, 1.0F, 4.0F), 180.0F, 0.0F, 30.0F)});
    fh::CutscenePlayer player;
    player.play(cutscene);
    fh::CutsceneScene scene;
    scene.car = glm::translate(glm::mat4(1.0F), glm::vec3(100.0F, 10.0F, -50.0F));
    play(player, 1.0F);
    const std::optional<fh::CutsceneView> view = player.view(scene);
    CHECK(view.has_value());
    if (!view) {
        return;
    }
    CHECK_NEAR(view->position.x, 101.0F, 0.05F);
    CHECK_NEAR(view->position.y, 11.0F, 1e-3F);
    CHECK_NEAR(view->position.z, -46.0F, 1e-3F);
    CHECK_NEAR(view->fov, glm::radians(25.0F), glm::radians(0.3F));
    // Yaw 180 looks back along the car.
    CHECK_NEAR(view->forward.z, -1.0F, 1e-3F);
}

// Hermite through three keys passes through the middle one with the
// neighbouring slopes' weighted tangent, so an evenly spaced ramp stays a
// straight line.
TEST(cutscene_camera_curve_passes_its_keys)
{
    const fh::Cutscene cutscene = singleCut(2.0F,
        {key(0.0F, glm::vec3(0.0F)), key(1.0F, glm::vec3(0.0F, 0.0F, 1.0F)), key(2.0F, glm::vec3(0.0F, 0.0F, 2.0F))});
    fh::CutscenePlayer player;
    player.play(cutscene);
    const fh::CutsceneScene scene;
    play(player, 0.5F);
    std::optional<fh::CutsceneView> view = player.view(scene);
    CHECK(view && std::abs(view->position.z - 0.5F) < 0.02F);
    play(player, 0.5F);
    view = player.view(scene);
    CHECK(view && std::abs(view->position.z - 1.0F) < 0.02F);
}

// A PartSpace key is an offset from the car part it names; without the
// part's position the cut leaves the camera to the game.
TEST(cutscene_part_space_measures_from_the_car_part)
{
    fh::Cutscene cutscene = singleCut(1.0F, {key(0.0F, glm::vec3(0.5F, 0.0F, 0.0F))});
    cutscene.cameras[0].positionSpace = fh::CutsceneSpace::Part;
    cutscene.cameras[0].keys[0].carPart = "headlightL";
    fh::CutscenePlayer player;
    player.play(cutscene);
    player.advance(kStep);
    fh::CutsceneScene scene;
    CHECK(!player.view(scene).has_value());
    scene.parts[0] = glm::vec3(-0.7F, 0.5F, 1.8F);
    const std::optional<fh::CutsceneView> view = player.view(scene);
    CHECK(view.has_value());
    if (view) {
        CHECK_NEAR(view->position.x, -0.2F, 1e-4F);
        CHECK_NEAR(view->position.z, 1.8F, 1e-4F);
    }
    CHECK_EQ(fh::cutsceneCarPart("wheelLR"), 8);
    CHECK_EQ(fh::cutsceneCarPart("roof"), -1);
}

// The intro's first cut fades in from black: opaque for its hold, then
// clear (Duration 0.5, HoldDuration 0.5, Linear: a cut at 0.5 s).
TEST(cutscene_cut_fades_in_from_black)
{
    fh::Cutscene cutscene = singleCut(2.5F, {key(0.0F, glm::vec3(0.0F))});
    cutscene.cameras[0].fadeIn = fh::FadeOptions{0.5F, 0.5F, false, glm::vec3(0.0F)};
    fh::CutscenePlayer player;
    player.play(cutscene);
    play(player, 0.25F);
    glm::vec3 colour(1.0F);
    CHECK_NEAR(player.overlay(colour), 1.0F, 1e-6F);
    CHECK_NEAR(colour.x, 0.0F, 1e-6F);
    play(player, 0.5F);
    CHECK_NEAR(player.overlay(colour), 0.0F, 1e-6F);
}

TEST(fade_in_opacity_follows_the_fader)
{
    const fh::FadeOptions sCurve{1.0F, 0.0F, true, glm::vec3(0.0F)};
    CHECK_NEAR(fh::fadeInOpacity(sCurve, 0.0F), 1.0F, 1e-6F);
    CHECK_NEAR(fh::fadeInOpacity(sCurve, 0.5F), 0.5F, 1e-5F);
    CHECK_NEAR(fh::fadeInOpacity(sCurve, 1.0F), 0.0F, 1e-6F);
    const fh::FadeOptions held{0.5F, 0.5F, false, glm::vec3(0.0F)};
    CHECK_NEAR(fh::fadeInOpacity(held, 0.49F), 1.0F, 1e-6F);
    CHECK_NEAR(fh::fadeInOpacity(held, 0.5F), 0.0F, 1e-6F);
}

// A race session loads the default race cutscenes and its track's file;
// FR02 runs on FESTIVAL_PLNS_006, which has its own intro, countdown and
// finish-line cutscenes, and the generic ones stand in for the rest.
TEST(disc_race_session_finds_its_own_cutscenes)
{
    const char* dir = gameDir();
    if (dir == nullptr) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const fh::CutsceneLibrary race = fh::CutsceneLibrary::load(vfs, "colorado", true, "FESTIVAL_PLNS_006");
    const fh::Cutscene* intro = race.raceSpecific("prerace_intro", "FESTIVAL_PLNS_006", "FR02", 26);
    CHECK(intro != nullptr && intro->name == "prerace_intro_FESTIVAL_PLNS_006");
    if (intro != nullptr) {
        CHECK(intro->looping);
        CHECK_NEAR(intro->firstEvent("CCutsceneCountdownTrigger").value_or(-1.0F), 5.0F, 1e-4F);
        CHECK(intro->cameras.front().fadeIn.has_value());
        CHECK_EQ(intro->cameras.front().target, std::string("start_location_00"));
    }
    const fh::Cutscene* countdown = race.raceSpecific("prerace_countdown", "FESTIVAL_PLNS_006", "FR02", 26);
    CHECK(countdown != nullptr && countdown->name == "prerace_countdown_FESTIVAL_PLNS_006");
    if (countdown != nullptr) {
        CHECK_NEAR(countdown->firstEvent("CCutsceneStartCountdownTrigger").value_or(-1.0F), 1.3F, 1e-4F);
        CHECK_EQ(countdown->cameras.back().type, std::string("Follow"));
    }
    const fh::Cutscene* generic = race.raceSpecific("postrace_transition_02", "FESTIVAL_PLNS_006", "FR02", 26);
    CHECK(generic != nullptr && generic->name == "postrace_transition_02");
    CHECK(race.find("event_activate_intro") == nullptr);

    const fh::CutsceneLibrary freeRoam = fh::CutsceneLibrary::load(vfs, "colorado", false, {});
    const fh::Cutscene* activate = freeRoam.raceSpecific("event_activate_intro", {}, "FR04", 239);
    CHECK(activate != nullptr && activate->name == "event_activate_intro");
    if (activate != nullptr) {
        CHECK_NEAR(activate->firstEvent("CCutsceneCountdownTrigger").value_or(-1.0F), 0.0F, 1e-6F);
    }
    CHECK(freeRoam.find("prerace_intro") == nullptr);
}

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
