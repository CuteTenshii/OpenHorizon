// Scenario tests: the whole game, run offscreen at a fixed step with
// scripted controls, checked through its own state. Each reproduces
// something a player does (pausing, opening the map, turning at low speed)
// and asserts what the game must do. They need the disc and skip without
// FH1_GAME_DIR. With FH1_CAPTURE_DIR set they also save screenshots there,
// for a person to look at (scripts/capture.sh).

#include "TestHarness.h"

#include "audio/MixSnapshots.h"
#include "base/Log.h"
#include "car/CarPhysics.h"
#include "game/Game.h"
#include "game/Outposts.h"
#include "game/Race.h"
#include "game/SpeedTraps.h"
#include "game/Title.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "ui/Navigation.h"
#include "ui/PauseMenu.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The game runs at a fixed 60 Hz so every run of a scenario is the same.
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

/// One window for the whole run: the game allows one per process, and a
/// small offscreen one keeps frames cheap.
fh::Window& window()
{
    static fh::Window instance([] {
        fh::WindowOptions options;
        options.width = 640;
        options.height = 360;
        options.hidden = true;
        options.vsync = false;
        return options;
    }());
    return instance;
}

/// What the player holds during a stretch of a scenario.
struct Hold {
    float throttle = 0.0F;
    float brake = 0.0F;
    /// -1 full left to 1 full right.
    float steer = 0.0F;
    /// The right stick's y: on the map, positive zooms in.
    float zoom = 0.0F;
};

/// A game and the scripted player driving it.
class Session {
public:
    explicit Session(fh::GameOptions options)
    {
        options.gameDir = gameDir();
        m_game = std::make_unique<fh::Game>(window(), options);
    }

    fh::Game& game() { return *m_game; }

    /// Runs frames until the player drives (loading and its fade over), at
    /// most `seconds` of game time; loading streams on worker threads.
    bool waitForCar(float seconds = 120.0F)
    {
        for (float t = 0.0F; t < seconds && !m_game->inControl(); t += kStep) {
            frame({});
        }
        return m_game->inControl();
    }

    /// Runs `seconds` of game time holding `hold`.
    void run(float seconds, const Hold& hold)
    {
        for (float t = 0.0F; t < seconds; t += kStep) {
            frame(hold);
        }
    }

    /// Presses and releases a button over two frames.
    void press(fh::Button button)
    {
        frame({}, button);
        frame({});
    }

    /// Saves the last frame when FH1_CAPTURE_DIR asks for screenshots.
    void capture(const std::string& name)
    {
        const char* dir = std::getenv("FH1_CAPTURE_DIR");
        if (dir != nullptr && *dir != '\0') {
            m_game->saveScreenshot(std::filesystem::path(dir) / (name + ".png"));
        }
    }

private:
    void frame(const Hold& hold, std::optional<fh::Button> button = std::nullopt)
    {
        fh::InputState input;
        input.previousButtons = m_previousButtons;
        input.axes[static_cast<std::size_t>(fh::Axis::RightTrigger)] = hold.throttle;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftTrigger)] = hold.brake;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftX)] = hold.steer;
        input.axes[static_cast<std::size_t>(fh::Axis::RightY)] = hold.zoom;
        if (button) {
            input.buttons[static_cast<std::size_t>(*button)] = true;
        }
        m_previousButtons = input.buttons;
        CHECK(m_game->frame(input, kStep));
    }

    std::unique_ptr<fh::Game> m_game;
    std::array<bool, static_cast<std::size_t>(fh::Button::Count)> m_previousButtons{};
};

float headingOf(const glm::vec3& direction)
{
    return std::atan2(direction.x, direction.z);
}

float angleBetween(float a, float b)
{
    return std::abs(std::remainder(a - b, 2.0F * 3.14159265F));
}

bool played(const std::vector<std::string>& sounds, std::string_view name)
{
    return std::find(sounds.begin(), sounds.end(), name) != sounds.end();
}

/// The UI sounds played since `before` of them had been.
std::vector<std::string> playedSince(const fh::Game& game, std::size_t before)
{
    const std::vector<std::string> all = game.uiSoundsPlayed();
    return {all.begin() + static_cast<std::ptrdiff_t>(std::min(before, all.size())), all.end()};
}

std::optional<fh::audio::MixSnapshotId> gameplayMix(const fh::Game& game)
{
    const fh::audio::SnapshotMixer* mix = game.mixSnapshots();
    return mix ? mix->current(fh::audio::MixGroup::Gameplay) : std::nullopt;
}

/// Runs the title screen at the scenarios' fixed step with `button` held
/// for one frame, then released.
bool titleFrames(fh::Title& title, float seconds, std::optional<fh::Button> button = std::nullopt)
{
    std::array<bool, static_cast<std::size_t>(fh::Button::Count)> previous{};
    bool started = false;
    for (float t = 0.0F; t < seconds; t += kStep) {
        fh::InputState input;
        input.previousButtons = previous;
        if (button && t == 0.0F) {
            input.buttons[static_cast<std::size_t>(*button)] = true;
        }
        previous = input.buttons;
        started = title.frame(input, kStep);
    }
    return started;
}

TEST(titleScreenWaitsForStart)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Title title(window(), dir);
    CHECK(!titleFrames(title, 3.0F));
    CHECK(title.screen().hasMovie());
    CHECK(title.screen().musicPlaying());
    CHECK(title.screen().pressStartText() == "PRESS START");

    // B is polled on the title and does nothing.
    CHECK(!titleFrames(title, 0.5F, fh::Button::B));
    // Past the 53 s idle the screen fades out and comes straight back.
    CHECK(!titleFrames(title, 60.0F));
    CHECK(title.screen().isShown());

    CHECK(titleFrames(title, kStep, fh::Button::Start));
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
TEST(pauseMenuStopsTheGame)
{
    if (!gameDir()) {
        return;
    }
    using fh::audio::MixCategory;
    using fh::audio::MixSnapshotId;
    Session session({});
    // Free roam loads under the Loading mix and drives under FreeRoam.
    CHECK(gameplayMix(session.game()) == MixSnapshotId::Loading);
    CHECK(session.waitForCar());
    session.run(3.0F, {.throttle = 1.0F});
    fh::Game& game = session.game();
    CHECK(gameplayMix(game) == MixSnapshotId::FreeRoam);
    CHECK_NEAR(game.audioCategoryVolume(MixCategory::FocusCar), 0.35, 1e-4);
    CHECK(game.playerEngineGain() > 0.0F);
    CHECK(game.hudDrawn());
    const glm::vec3 position = game.playerCar().position();
    const float speed = game.playerCar().speed();
    const float clock = game.timeOfDaySeconds();
    const float animation = game.animationTime();
    const glm::vec3 eye = game.camera().position;
    const glm::vec3 target = game.camera().target;
    const std::size_t sounds = game.uiSoundsPlayed().size();

    session.press(fh::Button::Start);
    CHECK(game.pauseShown());
    session.run(3.0F, {.throttle = 1.0F});
    session.capture("pause");
    CHECK(game.pauseShown());
    CHECK_NEAR(glm::length(game.playerCar().position() - position), 0.0, 1e-3);
    CHECK_NEAR(game.playerCar().speed(), speed, 1e-3);
    CHECK_NEAR(game.timeOfDaySeconds(), clock, 1e-3);
    CHECK_NEAR(game.animationTime(), animation, 1e-6);
    CHECK_NEAR(glm::length(game.camera().position - eye), 0.0, 1e-5);
    CHECK_NEAR(glm::length(game.camera().target - target), 0.0, 1e-5);
    CHECK(!game.hudDrawn());
    CHECK(gameplayMix(game) == MixSnapshotId::Paused);
    CHECK_NEAR(game.audioCategoryVolume(MixCategory::FocusCar), 0.0, 1e-6);
    CHECK_NEAR(game.audioCategoryVolume(MixCategory::Tire), 0.0, 1e-6);
    CHECK_NEAR(game.audioCategoryVolume(MixCategory::GameMusic), 0.25, 1e-6);
    CHECK_NEAR(game.audioCategoryVolume(MixCategory::GameUI), 1.0, 1e-6);
    CHECK_NEAR(game.playerEngineGain(), 0.0, 1e-6);
    CHECK(playedSince(game, sounds) == std::vector<std::string>{"Pause"});

    // B is the help bar's RESUME: the menu's back sound, then the pause
    // screen's own as it goes, and FreeRoam comes back.
    session.press(fh::Button::B);
    CHECK(!game.pauseShown());
    CHECK((playedSince(game, sounds) == std::vector<std::string>{"Pause", "Cancel", "UnPause"}));
    session.run(1.0F, {});
    CHECK(game.hudDrawn());
    CHECK(game.animationTime() > animation);
    CHECK(gameplayMix(game) == MixSnapshotId::FreeRoam);
    CHECK_NEAR(game.audioCategoryVolume(MixCategory::FocusCar), 0.35, 1e-4);
    CHECK(game.playerEngineGain() > 0.0F);
}

// MAP in the pause menu goes to the map without unpausing (map_flow_pause),
// and B on the map comes back to the pause screen (enterBack).
TEST(pauseMenuMapKeepsTheGamePaused)
{
    if (!gameDir()) {
        return;
    }
    using fh::audio::MixSnapshotId;
    Session session({});
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    fh::Game& game = session.game();
    const std::size_t sounds = game.uiSoundsPlayed().size();
    session.press(fh::Button::Start);
    const fh::ui::PauseMenu* menu = game.pauseMenu();
    CHECK(menu != nullptr);
    if (menu == nullptr) {
        return;
    }
    const auto& entries = menu->entries();
    const auto map = std::find_if(entries.begin(), entries.end(),
        [](const fh::ui::PauseMenuEntry& e) { return e.item == fh::ui::PauseItem::Map; });
    CHECK(map != entries.end());
    for (auto i = entries.begin(); i != map && map != entries.end(); ++i) {
        session.press(fh::Button::DpadDown);
    }
    session.press(fh::Button::A);
    CHECK(game.mapShown());
    session.run(0.5F, {});
    CHECK(gameplayMix(game) == MixSnapshotId::Paused);
    CHECK(game.uiSoundPlaying("Map_AmbientBackgroundLoop"));
    session.press(fh::Button::B);
    CHECK(game.pauseShown());
    CHECK(!game.uiSoundPlaying("Map_AmbientBackgroundLoop"));
    CHECK(gameplayMix(game) == MixSnapshotId::Paused);
    CHECK((playedSince(game, sounds)
        == std::vector<std::string>{"Pause", "Accept", "UnPause", "Map_AmbientBackgroundLoop", "Cancel", "Pause"}));
    session.press(fh::Button::B);
    session.run(1.0F, {});
    CHECK(gameplayMix(game) == MixSnapshotId::FreeRoam);
}

/// Every shown pause entry's text ends inside its brush stroke.
void checkPauseEntriesFit(const fh::Game& game)
{
    const fh::ui::PauseMenu* menu = game.pauseMenu();
    CHECK(menu != nullptr);
    if (menu == nullptr) {
        return;
    }
    const std::vector<fh::ui::PauseEntryExtent> extents = menu->entryExtents();
    CHECK_EQ(extents.size(), menu->entries().size());
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
TEST(pauseMenuEntriesFitTheirBars)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    session.press(fh::Button::Start);
    for (const auto& [width, height] : {std::pair{1600, 900}, std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        window().setSize(width, height);
        session.run(1.0F, {});
        CHECK(session.game().pauseShown());
        checkPauseEntriesFit(session.game());
        session.capture(std::format("pause_{}x{}", width, height));
    }
    // The focused entry plays its own slide (FOCUSBELOW) with the pink bar.
    session.press(fh::Button::DpadDown);
    session.run(1.0F, {});
    CHECK_EQ(session.game().pauseMenu()->focus(), 1);
    checkPauseEntriesFit(session.game());
    session.capture("pause_multiplayer_2560x1440");
    window().setSize(640, 360);
}

TEST(mapReplacesTheWorld)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    CHECK(session.game().worldDrawn());

    const std::size_t sounds = session.game().uiSoundsPlayed().size();
    session.press(fh::Button::Back);
    CHECK(session.game().mapShown());
    session.run(1.0F, {});
    session.capture("map");
    CHECK(!session.game().worldDrawn());
    // The map pauses the game (pause_for_map) with the pause sound
    // (CWaitForPause) and plays its ambience loop.
    CHECK(gameplayMix(session.game()) == fh::audio::MixSnapshotId::Paused);
    CHECK(session.game().uiSoundPlaying("Map_AmbientBackgroundLoop"));

    // B is the map's EXIT MAP.
    session.press(fh::Button::B);
    session.run(0.2F, {});
    CHECK(!session.game().mapShown());
    CHECK(session.game().worldDrawn());
    CHECK(!session.game().uiSoundPlaying("Map_AmbientBackgroundLoop"));
    CHECK((playedSince(session.game(), sounds)
        == std::vector<std::string>{"Pause", "Map_AmbientBackgroundLoop", "Cancel"}));
    session.run(1.0F, {});
    CHECK(gameplayMix(session.game()) == fh::audio::MixSnapshotId::FreeRoam);
}

// "Map road colours are the same (normally they're different depending on
// the road type)." The map's road lines lerp from their grey primary colour
// to their road type's secondary colour where the player has driven
// (CMapCoverage's visit counts through CMapEntityWay's colour source), so
// driving a stretch of road colours it.
TEST(drivenRoadsColourTheMap)
{
    if (!gameDir()) {
        return;
    }
    // On colorado's way 33, the b road "road_094", at its first node and
    // facing its second, 20 m to the south.
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    CHECK(navigation != nullptr);
    if (navigation == nullptr) {
        return;
    }
    const fh::ui::NavFile& network = navigation->network();
    const auto drivenNodes = [&] {
        std::size_t count = 0;
        for (std::uint32_t node = 0; node < network.nodes.size(); ++node) {
            count += navigation->coverage().visits(node) > 0 ? 1 : 0;
        }
        return count;
    };
    CHECK(navigation->playerNode().has_value());
    const std::size_t atStart = drivenNodes();
    CHECK(atStart > 0);

    session.run(10.0F, {.throttle = 0.7F});
    CHECK(drivenNodes() > atStart);
    const std::optional<std::uint32_t> node = navigation->playerNode();
    CHECK(node.has_value());
    if (node) {
        bool coloured = false;
        for (const fh::ui::NavLink& link : network.nodes[*node].links) {
            const std::vector<float> ratios = navigation->coverage().wayRatios(link.way);
            coloured = coloured || std::find(ratios.begin(), ratios.end(), 1.0F) != ratios.end();
        }
        CHECK(coloured);
    }
    session.press(fh::Button::Back);
    session.run(3.0F, {.zoom = 1.0F});
    session.capture("map_driven");
}

// A race drives under the Race mix (exterior_race before start_race in the
// race flows) and goes back to FreeRoam when it is left.
TEST(raceUsesTheRaceMix)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    CHECK(gameplayMix(session.game()) == fh::audio::MixSnapshotId::Race);
}

TEST(gentleLeftTurnFromStandstillDoesNotSpin)
{
    if (!gameDir()) {
        return;
    }
    // A player on a keyboard pulls away at full throttle and eases left: the
    // car must turn, not spin, with the assists on (the default).
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    // The countdown holds the car; wait for the start.
    for (float t = 0.0F; t < 30.0F && session.game().race() && !session.game().race()->started(); t += kStep) {
        session.run(kStep, {});
    }
    session.run(3.0F, {.throttle = 1.0F, .steer = -0.3F});
    session.capture("left_turn");
    const fh::CarPhysics& car = session.game().playerCar();
    const float yawRate = glm::dot(car.angularVelocity(), car.up());
    const glm::vec3 velocity = car.velocity();
    const float bodySlip = glm::length(velocity) > 1.0F
        ? std::abs(std::atan2(glm::dot(velocity, car.right()), glm::dot(velocity, car.forward())))
        : 0.0F;
    CHECK(std::abs(yawRate) < 1.0F);
    CHECK(bodySlip < 0.5F);
}

TEST(chaseCameraStaysBehindInATurn)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    for (float t = 0.0F; t < 30.0F && session.game().race() && !session.game().race()->started(); t += kStep) {
        session.run(kStep, {});
    }
    session.run(4.0F, {.throttle = 0.6F});
    session.run(1.5F, {.throttle = 0.4F, .steer = -0.3F});
    session.capture("camera_turn");
    const fh::CarPhysics& car = session.game().playerCar();
    const fh::render::Camera& camera = session.game().camera();
    // The chase camera looks at the car from behind: its view heading stays
    // close to the car's heading through a gentle turn.
    const float view = headingOf(camera.target - camera.position);
    CHECK(angleBetween(view, headingOf(car.forward())) < 0.6F);
}

// Free roam draws the game's HUD scene, and nothing of the plain overlay's
// own HUD (speed panel, gear, rev bar) stands in for it.
TEST(freeRoamDrawsTheGamesHud)
{
    if (!gameDir()) {
        return;
    }
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
TEST(raceDrawsOnlyTheGamesHud)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    fh::Game& game = session.game();
    session.run(1.5F, {});
    session.capture("race_countdown");
    CHECK(game.hudDrawn());
    CHECK(game.overlayVerticesDrawn() == 0);
    for (float t = 0.0F; t < 30.0F && game.race() && !game.race()->started(); t += kStep) {
        session.run(kStep, {});
    }
    session.run(3.0F, {.throttle = 1.0F});
    session.capture("race_hud");
    CHECK(game.hudDrawn());
    CHECK(game.overlayVerticesDrawn() == 0);
    const std::vector<fh::DrivingLine::Vertex>& line = game.drivingLineDrawn();
    CHECK(!line.empty());
    CHECK(std::any_of(line.begin(), line.end(), [](const auto& v) { return v.colour[3] > 0; }));

    // The pause flow hides the line (disable_racing_line in pause.xml).
    session.press(fh::Button::Start);
    CHECK(game.pauseShown());
    CHECK(game.drivingLineDrawn().empty());
}

bool hasMarker(const fh::ui::WorldMapState& state, std::string_view value, std::string_view name = {})
{
    return std::any_of(state.markers.begin(), state.markers.end(),
        [&](const fh::ui::WorldMapMarker& m) { return m.marker.value == value && (name.empty() || m.name == name); });
}

// "Mannnnnn this is a recurring issue atp", about the name of an event
// start floating in a dark box the text ran out of. Nothing of the event
// start is made up: no beams, rings or labels in the plain overlay. The
// start shows the set dressing the game puts out for its zone (the world's
// FR04 props), and inside the zone's 25 m the HUD's activation prompt names
// the event with the zone's call to action, ENTER NOW.
TEST(eventStartShowsTheGamesZone)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.spawnObject = "FR04";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(3.0F, {});
    fh::Game& game = session.game();
    session.capture("event_start_fr04");
    CHECK(game.overlayVerticesDrawn() == 0);
    CHECK(game.activityPropChunksDrawn() > 0);
    const fh::ui::HudState hud = game.currentHudState();
    CHECK(!hud.prompt.empty());
    CHECK_EQ(hud.promptDetail, std::string("ENTER NOW"));
    CHECK(hud.promptTitle.empty());
    // X is the zone's activation.
    session.press(fh::Button::X);
    session.run(0.5F, {});
    CHECK(game.race() != nullptr);
    CHECK(game.activityPropChunksDrawn() == 0);
}

// Driving through a speed trap (speed_camera_01) above its 30 mph measures
// the car, discovers the trap for the map and hands the HUD's speed trap
// panel the reading.
TEST(speedTrapMeasuresAPass)
{
    if (!gameDir()) {
        return;
    }
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
    for (float t = 0.0F; t < 20.0F && trap && !trap->discovered; t += kStep) {
        session.run(kStep, {.throttle = 1.0F});
    }
    session.run(0.3F, {.throttle = 1.0F});
    session.capture("speed_trap_speed");
    session.run(2.2F, {});
    session.capture("speed_trap_best");
    CHECK(trap && trap->discovered);
    CHECK(trap && trap->best.value_or(0.0F) >= 30.0F * 0.44704F);
    const fh::ui::HudState hud = game.currentHudState();
    CHECK(hud.speedCamera.has_value());
    CHECK(hud.speedCamera && !hud.speedCamera->zone);
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
TEST(outpostIsDiscoveredAndOffersItsZone)
{
    if (!gameDir()) {
        return;
    }
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
    CHECK_EQ(outposts->discoveredCount(), std::size_t{1});
    session.capture("outpost_prompt");
    const fh::ui::HudState hud = game.currentHudState();
    CHECK_EQ(hud.promptTitle, std::string("HORIZON OUTPOST"));
    CHECK_EQ(hud.prompt, std::string("SOUTH BEAUMONT"));
    CHECK_EQ(hud.promptDetail, std::string("ENTER NOW"));
    CHECK(hasMarker(game.currentMapState(), "gas_station", "SOUTH BEAUMONT"));
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    session.capture("outpost_map");
}

// "Find in-game loading screens (those with a tip shown) so you can add it
// while you load game stuff": the loading screen went away as soon as the
// ground under the car was in, while world tiles, textures, track effects,
// traffic and the radio were still loading and popped in. The game holds
// its loading screen until streaming around the camera and every car are
// loaded (CWaitForStreaming, CWaitForAllCarsToLoad in prepare_free_roam),
// with the radio and the first traffic started behind it.
TEST(loadingScreenStaysUntilTheStartIsReady)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    fh::Game& game = session.game();
    // Up from the first frame, before any has run.
    CHECK(game.loadingStatus().screenShown);
    for (float t = 0.0F; t < 180.0F && game.loadingStatus().screenShown; t += kStep) {
        session.run(kStep, {});
    }
    const fh::Game::LoadingStatus status = game.loadingStatus();
    CHECK(!status.screenShown);
    CHECK_EQ(status.pendingJobs, std::size_t{0});
    CHECK(status.nearbyWorldReady);
    CHECK(status.playerCarReady);
    CHECK(status.radioReady);
    CHECK_EQ(status.trafficPending, std::size_t{0});
    session.capture("loading_done");
    // Then the fade from black, and only after it the player drives.
    CHECK(status.fading);
    CHECK(!game.inControl());
    CHECK(session.waitForCar());
    CHECK(!game.loadingStatus().fading);
}

} // namespace

int main(int argc, char** argv)
{
    // Never open a window on the desktop or play through the speakers.
    setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    setenv("SDL_AUDIO_DRIVER", "dummy", 0);
    // Nor write to the player's own shader cache; this folder persists
    // between runs, so later runs also exercise the cached path.
    setenv("XDG_CACHE_HOME", (std::filesystem::temp_directory_path() / "fh1-scenarios-cache").c_str(), 1);
    fh::openLogFile(std::filesystem::temp_directory_path() / "fh1-scenarios.log");
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
