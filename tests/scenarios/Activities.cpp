// Scenarios of free roam's activities: a speed trap's pass and the
// outposts' discovery and prompts.

#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>

#include "game/Outposts.h"
#include "game/SpeedTraps.h"
#include "ui/Hud.h"

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

namespace fh::test {
namespace {

// Driving through a speed trap (speed_camera_01) above its 30 mph measures
// the car, discovers the trap for the map and hands the HUD's speed trap
// panel the reading.
TEST_CASE_METHOD(GameScenario, "speedTrapMeasuresAPass", "[scenario][activities]")
{
    // The trap's posts stand either side of a road running north.
    const glm::vec2 gate{2274.2F, 319.6F};
    const glm::vec2 north = glm::normalize(glm::vec2(0.1167F, 0.9932F));
    fh::GameOptions options;
    options.spawnAt = gate - north * 120.0F;
    options.spawnHeading = std::atan2(north.x, north.y);
    Session session(options);
    CHECK(session.waitForCar());
    fh::Game& game = session.game();
    const fh::SpeedTraps* traps = game.speedTraps();
    CHECK(traps != nullptr);
    if (traps == nullptr) {
        return;
    }
    const fh::SpeedTrap* trap = traps->find("speed_camera_01");
    CHECK(trap != nullptr);
    CHECK(!hasMarker(game.currentMapState(), "speed_camera_discovered"));
    session.runUntil(20.0F, [&] { return !trap || trap->discovered; }, {.throttle = 1.0F});
    // "Drive past a speed camera" made no sound: the pass plays the
    // camera's shutter and applies the SpeedCam mix.
    const fh::GameplaySounds* sounds = game.gameplaySounds();
    CHECK((sounds && sounds->speedTrapFlashes() == 1));
    CHECK((sounds && sounds->speedTrapFlashPlaying()));
    const fh::audio::SnapshotMixer* mix = game.mixSnapshots();
    CHECK((mix && mix->current(fh::audio::MixGroup::SpeedZones) == fh::audio::MixSnapshotId::SpeedCam));
    session.run(0.3F, {.throttle = 1.0F});
    session.capture("speed_trap_speed");
    session.run(2.2F, {});
    session.capture("speed_trap_best");
    CHECK((trap && trap->discovered));
    CHECK((trap && trap->best.value_or(0.0F) >= 30.0F * 0.44704F));
    const fh::ui::HudState hud = game.currentHudState();
    CHECK(hud.speedCamera.has_value());
    CHECK((hud.speedCamera && !hud.speedCamera->zone));
    const auto markers = hud.mapMarkers;
    CHECK(std::any_of(markers.begin(), markers.end(),
        [](const fh::ui::HudMapMarker& m) { return m.value == "speed_camera_discovered"; }));
    CHECK(hasMarker(game.currentMapState(), "speed_camera_discovered"));
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    session.capture("speed_trap_map");
}

// A Horizon outpost (GasStation_01, the GASSTATION_001 zone) is discovered
// within its discovery zone, then shows on the map, and its zone offers the
// outpost with the game's prompt: HORIZON OUTPOST, its name, ENTER NOW.
TEST_CASE_METHOD(GameScenario, "outpostIsDiscoveredAndOffersItsZone", "[scenario][activities]")
{
    fh::GameOptions options;
    options.spawnObject = "GASSTATION_001";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    fh::Game& game = session.game();
    const fh::Outposts* outposts = game.outposts();
    CHECK(outposts != nullptr);
    if (outposts == nullptr) {
        return;
    }
    CHECK(outposts->discoveredCount() == std::size_t{1});
    session.capture("outpost_prompt");
    const fh::ui::HudState hud = game.currentHudState();
    CHECK(hud.promptTitle == std::string("HORIZON OUTPOST"));
    CHECK(hud.prompt == std::string("SOUTH BEAUMONT"));
    CHECK(hud.promptDetail == std::string("ENTER NOW"));
    CHECK(hasMarker(game.currentMapState(), "gas_station", "SOUTH BEAUMONT"));
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    session.capture("outpost_map");
}

// A player's screenshot near "the media centre": HORIZON OUTPOST over MEDIA
// CENTER with a race icon. The media centre is outpost GasStation_05 (the
// GASSTATION_005 zone, named IDS_GasStationName_05 in gas_stations.xml),
// so its title and name are the game's; the icon was not. An outpost's
// discovery sets its zone's prompt icon to 33 (0x82972dcc), which the
// prompt shows as SET_EVENTICON_PRHUB (0x8289acf8).
TEST_CASE_METHOD(GameScenario, "mediaCenterOutpostPromptShowsThePrHubIcon", "[scenario][activities]")
{
    fh::GameOptions options;
    options.spawnObject = "GASSTATION_005";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    fh::Game& game = session.game();
    const fh::ui::Hud* hud = game.hud();
    CHECK(hud != nullptr);
    if (hud == nullptr) {
        return;
    }
    session.capture("outpost_media_center_prompt");
    const fh::ui::HudState state = game.currentHudState();
    CHECK(state.promptTitle == std::string("HORIZON OUTPOST"));
    CHECK(state.prompt == std::string("MEDIA CENTER"));
    CHECK(state.promptDetail == std::string("ENTER NOW"));
    CHECK(state.promptIcon == std::string("PRHUB"));
    CHECK(hud->promptIconSlide() == std::string("PRHUB"));
}

} // namespace
} // namespace fh::test
