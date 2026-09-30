// Scenarios of the menus and the HUD: the title screen, the pause menu and
// what pausing stops, the help bars, the HUD in free roam and in a race,
// the event prompt and the loading screen.

#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "game/Title.h"
#include "ui/Hud.h"
#include "ui/PauseMenu.h"
#include "ui/WorldMap.h"

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

/// Runs the title screen at the scenarios' fixed step with `button` held
/// for one frame, then released.
bool titleFrames(openhorizon::Title& title, float seconds, std::optional<openhorizon::Button> button = std::nullopt)
{
    std::array<bool, static_cast<std::size_t>(openhorizon::Button::Count)> previous{};
    bool started = false;
    for (float t = 0.0F; t < seconds; t += kStep) {
        openhorizon::InputState input;
        input.previousButtons = previous;
        if (button && t == 0.0F) {
            input.buttons[static_cast<std::size_t>(*button)] = true;
        }
        previous = input.buttons;
        started = title.frame(input, kStep);
    }
    return started;
}

TEST_CASE_METHOD(GameScenario, "titleScreenWaitsForStart", "[scenario][menus]")
{
    openhorizon::Title title(window(), gameDir());
    CHECK(!titleFrames(title, 3.0F));
    CHECK(title.screen().hasMovie());
    CHECK(title.screen().musicPlaying());
    CHECK(title.screen().pressStartText() == "PRESS START");

    // B is polled on the title and does nothing.
    CHECK(!titleFrames(title, 0.5F, openhorizon::Button::B));
    // Past the 53 s idle the screen fades out and comes straight back.
    CHECK(!titleFrames(title, 60.0F));
    CHECK(title.screen().isShown());

    CHECK(titleFrames(title, kStep, openhorizon::Button::Start));
    CHECK(!title.screen().musicPlaying());
    // CSplash's cues on entering and on Start (both silent "Blank" events).
    CHECK(played(title.screen().soundsPlayed(), "PressStartSlideOn"));
    CHECK(played(title.screen().soundsPlayed(), "PressStartSlideOff"));
}

// "The pause menu STILL doesn't pause the game. At most currently it freezes
// the car." Pausing (CPauseGame, pause_game in media/gamemodes/pause.xml)
// stops the world: the car, the cameras and the clocks. The pause screen
// takes the HUD's place, the Paused mixer snapshot silences the cars and
// turns the radio down, and the pause screen plays its sounds.
TEST_CASE_METHOD(GameScenario, "pauseMenuStopsTheGame", "[scenario][menus]")
{
    using openhorizon::audio::MixCategory;
    using openhorizon::audio::MixSnapshotId;
    Session session({});
    // Free roam loads under the Loading mix and drives under FreeRoam.
    CHECK(gameplayMix(session.game()) == MixSnapshotId::Loading);
    CHECK(session.waitForCar());
    session.run(3.0F, {.throttle = 1.0F});
    openhorizon::Game& game = session.game();
    CHECK(gameplayMix(game) == MixSnapshotId::FreeRoam);
    CHECK_THAT(game.audioCategoryVolume(MixCategory::FocusCar), WithinAbs(0.35, 1e-4));
    CHECK(game.playerEngineGain() > 0.0F);
    CHECK(game.hudDrawn());
    const glm::vec3 position = game.playerCar().position();
    const float speed = game.playerCar().speed();
    const float clock = game.timeOfDaySeconds();
    const float animation = game.animationTime();
    const glm::vec3 eye = game.camera().position;
    const glm::vec3 target = game.camera().target;
    const std::size_t sounds = game.uiSoundsPlayed().size();

    session.press(openhorizon::Button::Start);
    CHECK(game.pauseShown());
    session.run(3.0F, {.throttle = 1.0F});
    session.capture("pause");
    CHECK(game.pauseShown());
    CHECK_THAT(glm::length(game.playerCar().position() - position), WithinAbs(0.0, 1e-3));
    CHECK_THAT(game.playerCar().speed(), WithinAbs(speed, 1e-3));
    CHECK_THAT(game.timeOfDaySeconds(), WithinAbs(clock, 1e-3));
    CHECK_THAT(game.animationTime(), WithinAbs(animation, 1e-6));
    CHECK_THAT(glm::length(game.camera().position - eye), WithinAbs(0.0, 1e-5));
    CHECK_THAT(glm::length(game.camera().target - target), WithinAbs(0.0, 1e-5));
    CHECK(!game.hudDrawn());
    CHECK(gameplayMix(game) == MixSnapshotId::Paused);
    CHECK_THAT(game.audioCategoryVolume(MixCategory::FocusCar), WithinAbs(0.0, 1e-6));
    CHECK_THAT(game.audioCategoryVolume(MixCategory::Tire), WithinAbs(0.0, 1e-6));
    CHECK_THAT(game.audioCategoryVolume(MixCategory::GameMusic), WithinAbs(0.25, 1e-6));
    CHECK_THAT(game.audioCategoryVolume(MixCategory::GameUI), WithinAbs(1.0, 1e-6));
    CHECK_THAT(game.playerEngineGain(), WithinAbs(0.0, 1e-6));
    CHECK(playedSince(game, sounds) == std::vector<std::string>{"Pause"});

    // B is the help bar's RESUME: the menu's back sound, then the pause
    // screen's own as it goes, and FreeRoam comes back.
    session.press(openhorizon::Button::B);
    CHECK(!game.pauseShown());
    CHECK((playedSince(game, sounds) == std::vector<std::string>{"Pause", "Cancel", "UnPause"}));
    session.run(1.0F, {});
    CHECK(game.hudDrawn());
    CHECK(game.animationTime() > animation);
    CHECK(gameplayMix(game) == MixSnapshotId::FreeRoam);
    CHECK_THAT(game.audioCategoryVolume(MixCategory::FocusCar), WithinAbs(0.35, 1e-4));
    CHECK(game.playerEngineGain() > 0.0F);
}

// MAP in the pause menu goes to the map without unpausing (map_flow_pause),
// and B on the map comes back to the pause screen (enterBack).
TEST_CASE_METHOD(GameScenario, "pauseMenuMapKeepsTheGamePaused", "[scenario][menus]")
{
    using openhorizon::audio::MixSnapshotId;
    Session session({});
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    openhorizon::Game& game = session.game();
    const std::size_t sounds = game.uiSoundsPlayed().size();
    session.press(openhorizon::Button::Start);
    const openhorizon::ui::PauseMenu* menu = game.pauseMenu();
    CHECK(menu != nullptr);
    if (menu == nullptr) {
        return;
    }
    CHECK(session.focusPauseItem(openhorizon::ui::PauseItem::Map));
    session.press(openhorizon::Button::A);
    CHECK(game.mapShown());
    session.run(0.5F, {});
    CHECK(gameplayMix(game) == MixSnapshotId::Paused);
    CHECK(game.uiSoundPlaying("Map_AmbientBackgroundLoop"));
    session.press(openhorizon::Button::B);
    CHECK(game.pauseShown());
    CHECK(!game.uiSoundPlaying("Map_AmbientBackgroundLoop"));
    CHECK(gameplayMix(game) == MixSnapshotId::Paused);
    CHECK((playedSince(game, sounds)
        == std::vector<std::string>{"Pause", "Accept", "UnPause", "Map_AmbientBackgroundLoop", "Cancel", "Pause"}));
    session.press(openhorizon::Button::B);
    session.run(1.0F, {});
    CHECK(gameplayMix(game) == MixSnapshotId::FreeRoam);
}

/// Every shown pause entry's text ends inside its brush stroke.
void checkPauseEntriesFit(const openhorizon::Game& game)
{
    const openhorizon::ui::PauseMenu* menu = game.pauseMenu();
    CHECK(menu != nullptr);
    if (menu == nullptr) {
        return;
    }
    const std::vector<openhorizon::ui::PauseEntryExtent> extents = menu->entryExtents();
    CHECK(extents.size() == menu->entries().size());
    for (std::size_t i = 0; i < extents.size() && i < menu->entries().size(); ++i) {
        if (extents[i].textRight > extents[i].strokeRight) {
            std::fprintf(stderr, "  %s: text reaches %.1f, its stroke %.1f\n", menu->entries()[i].text.c_str(),
                static_cast<double>(extents[i].textRight), static_cast<double>(extents[i].strokeRight));
        }
        CHECK(extents[i].textRight <= extents[i].strokeRight);
    }
}

// A player's pause menu at a desktop size showed MULTIPLAYER running off
// its bar. The window sizes are the common desktop ones; the menu is laid
// out in the scene's design units, so it must fit at every one.
TEST_CASE_METHOD(GameScenario, "pauseMenuEntriesFitTheirBars", "[scenario][menus]")
{
    Session session({});
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    session.press(openhorizon::Button::Start);
    for (const auto& [width, height] : {std::pair{1600, 900}, std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        window().setSize(width, height);
        session.run(1.0F, {});
        CHECK(session.game().pauseShown());
        checkPauseEntriesFit(session.game());
        session.capture(std::format("pause_{}x{}", width, height));
    }
    // The focused entry plays its own slide (FOCUSBELOW) with the pink bar.
    session.press(openhorizon::Button::DpadDown);
    session.run(1.0F, {});
    CHECK(session.game().pauseMenu()->focus() == 1);
    checkPauseEntriesFit(session.game());
    session.capture("pause_multiplayer_2560x1440");
    window().setSize(640, 360);
}

/// Checks a help bar as the game lays it out: each button's SUPER_STACKER
/// fits its pill to the logo and text, so every pill ends the same way past
/// its text, and the bar (0x8289e100) starts each button 34 units after
/// the text before it.
void checkHelpBarFits(const std::vector<openhorizon::ui::HelpButtonExtent>& extents, std::size_t shown)
{
    CHECK(extents.size() == shown);
    for (std::size_t i = 0; i < extents.size(); ++i) {
        const openhorizon::ui::HelpButtonExtent& button = extents[i];
        const float margin = button.pillRight - button.textRight;
        const float firstMargin = extents.front().pillRight - extents.front().textRight;
        if (std::abs(margin - firstMargin) >= 0.5F || !std::isfinite(margin)) {
            std::fprintf(stderr, "  button %zu: pill ends %.1f past its text, the first's %.1f\n", i,
                static_cast<double>(margin), static_cast<double>(firstMargin));
        }
        CHECK(std::isfinite(margin));
        CHECK(button.textRight <= button.pillRight);
        CHECK(std::abs(margin - firstMargin) < 0.5F);
        if (i + 1 < extents.size()) {
            const float gap = extents[i + 1].left - button.textRight;
            if (std::abs(gap - 34.0F) >= 0.5F) {
                std::fprintf(
                    stderr, "  button %zu starts %.1f after the text before it\n", i + 1, static_cast<double>(gap));
            }
            CHECK(std::abs(gap - 34.0F) < 0.5F);
        }
    }
}

// "The dark brush-stroke backgrounds behind the help bar's texts look wrong
// (square ends, a noisy rectangle)": the pills kept the width authored for
// "BUTTON TEXT" whatever the text, and the buttons their authored slots,
// because nothing ran the stackers of the help buttons nor the bar's own
// layout. Checked on the map and the pause menu at the desktop sizes.
TEST_CASE_METHOD(GameScenario, "helpBarsFitTheirTexts", "[scenario][menus]")
{
    Session session({});
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    session.press(openhorizon::Button::Back);
    for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        window().setSize(width, height);
        session.run(1.0F, {});
        const openhorizon::ui::WorldMap* map = session.game().worldMap();
        CHECK((map != nullptr && session.game().mapShown()));
        if (map != nullptr) {
            checkHelpBarFits(map->helpExtents(), 4);
        }
        session.capture(std::format("help_map_{}x{}", width, height));
    }
    session.press(openhorizon::Button::B);
    session.run(1.0F, {});
    session.press(openhorizon::Button::Start);
    for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        window().setSize(width, height);
        session.run(1.0F, {});
        CHECK(session.game().pauseShown());
        if (const openhorizon::ui::PauseMenu* menu = session.game().pauseMenu()) {
            checkHelpBarFits(menu->helpExtents(), 2);
        }
        session.capture(std::format("help_pause_{}x{}", width, height));
    }
    window().setSize(640, 360);
}

// Free roam draws the game's HUD scene, and nothing of the plain overlay's
// own HUD (speed panel, gear, rev bar) stands in for it.
TEST_CASE_METHOD(GameScenario, "freeRoamDrawsTheGamesHud", "[scenario][menus]")
{
    Session session({});
    CHECK(session.waitForCar());
    session.run(3.0F, {.throttle = 1.0F});
    session.capture("free_roam");
    CHECK(session.game().hudDrawn());
}

// "Mannnnnn this is a recurring issue atp": a race put made-up panels,
// text and lines over the game (place, laps, time, standings, a green and
// red driving line) in a system font. Everything on screen in a race comes
// from the game's HUD scene and the game's own driving line (chevrons of
// raceline.xds); the plain overlay is left to the debug view.
TEST_CASE_METHOD(GameScenario, "raceDrawsOnlyTheGamesHud", "[scenario][menus]")
{
    openhorizon::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    openhorizon::Game& game = session.game();
    session.run(1.5F, {});
    session.capture("race_countdown");
    CHECK(game.hudDrawn());
    CHECK(game.overlayVerticesDrawn() == 0);
    session.runUntil(30.0F, [&] { return !game.race() || game.race()->started(); });
    session.run(3.0F, {.throttle = 1.0F});
    session.capture("race_hud");
    CHECK(game.hudDrawn());
    CHECK(game.overlayVerticesDrawn() == 0);
    const std::vector<openhorizon::DrivingLine::Vertex>& line = game.drivingLineDrawn();
    CHECK(!line.empty());
    CHECK(std::any_of(line.begin(), line.end(), [](const auto& v) { return v.colour[3] > 0; }));

    // The pause flow hides the line (disable_racing_line in pause.xml).
    session.press(openhorizon::Button::Start);
    CHECK(game.pauseShown());
    CHECK(game.drivingLineDrawn().empty());
}

// The prompt control sets the icon from every status it shows (0x828a25f0
// passes the status's icon to 0x8289acf8), and a status without one shows
// SET_EVENTICON_DEFAULT. A prompt without an icon after an outpost's must
// not keep the outpost's.
TEST_CASE_METHOD(GameScenario, "promptWithoutAnIconShowsTheDefaultIcon", "[scenario][menus]")
{
    // The session provides the OpenGL context the HUD draws with.
    Session session({});
    CHECK(session.waitForCar());
    openhorizon::ui::Hud hud(disc(), "colorado");
    openhorizon::ui::HudState state;
    state.dt = kStep;
    state.prompt = "MEDIA CENTER";
    state.promptTitle = "HORIZON OUTPOST";
    state.promptDetail = "ENTER NOW";
    state.promptIcon = "PRHUB";
    hud.draw(state, 1280, 720);
    CHECK(hud.promptIconSlide() == std::string("PRHUB"));
    state.prompt.clear();
    hud.draw(state, 1280, 720);
    state.prompt = "COLORADO SPRINT";
    state.promptTitle.clear();
    state.promptIcon.clear();
    hud.draw(state, 1280, 720);
    CHECK(hud.promptIconSlide() == std::string("DEFAULT"));
}

// "Find in-game loading screens (those with a tip shown) so you can add it
// while you load game stuff": the loading screen went away as soon as the
// ground under the car was in, while world tiles, textures, track effects,
// traffic and the radio were still loading and popped in. The game holds
// its loading screen until streaming around the camera and every car are
// loaded (CWaitForStreaming, CWaitForAllCarsToLoad in prepare_free_roam),
// with the radio and the first traffic started behind it.
TEST_CASE_METHOD(GameScenario, "loadingScreenStaysUntilTheStartIsReady", "[scenario][menus]")
{
    Session session({});
    openhorizon::Game& game = session.game();
    // Up from the first frame, before any has run.
    CHECK(game.loadingStatus().screenShown);
    // While track effects link, the screen says so in place of PLEASE WAIT.
    bool compilingShown = false;
    for (float t = 0.0F; t < 180.0F && game.loadingStatus().screenShown; t += kStep) {
        session.run(kStep, {});
        const openhorizon::Game::LoadingStatus during = game.loadingStatus();
        if (!compilingShown && during.screenShown && during.programsPending > 0
            && during.message.starts_with("Compiling shaders ")) {
            compilingShown = true;
            session.capture("loading_status");
        }
    }
    CHECK(compilingShown);
    const openhorizon::Game::LoadingStatus status = game.loadingStatus();
    CHECK(!status.screenShown);
    CHECK(status.pendingJobs == std::size_t{0});
    CHECK(status.nearbyWorldReady);
    CHECK(status.programsPending == std::size_t{0});
    CHECK(status.playerCarReady);
    CHECK(status.radioReady);
    CHECK(status.trafficPending == std::size_t{0});
    session.capture("loading_done");
    // Then the fade from black, and only after it the player drives.
    CHECK(status.fading);
    CHECK(!game.inControl());
    CHECK(session.waitForCar());
    CHECK(!game.loadingStatus().fading);
}

} // namespace
} // namespace openhorizon::test
