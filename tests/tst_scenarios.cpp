// Scenario tests: the whole game, run offscreen at a fixed step with
// scripted controls, checked through its own state. Each reproduces
// something a player does (pausing, opening the map, turning at low speed)
// and asserts what the game must do. They need the disc and skip without
// FH1_GAME_DIR. With FH1_CAPTURE_DIR set they also save screenshots there,
// for a person to look at (scripts/capture.sh).

#include "RoadFollower.h"
#include "TestHarness.h"

#include "audio/MixSnapshots.h"
#include "base/Host.h"
#include "base/Log.h"
#include "car/CarPhysics.h"
#include "game/Game.h"
#include "game/Outposts.h"
#include "game/Race.h"
#include "game/SpeedTraps.h"
#include "game/Title.h"
#include "particles/ParticleManager.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "render/CrowdRenderer.h"
#include "render/EnvironmentCube.h"
#include "render/FrameStats.h"
#include "render/Gl.h"
#include "render/TrackPrograms.h"
#include "render/WorldRenderer.h"
#include "ui/Navigation.h"
#include "ui/PauseMenu.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <memory>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

/// The game runs at a fixed 60 Hz so every run of a scenario is the same.
constexpr float kStep = 1.0F / 60.0F;

bool haveOpenGl();

/// The disc, or null (and the scenario skips) when FH1_GAME_DIR is not set
/// or the machine has no OpenGL context for the game, as on a CI runner
/// without a GPU.
const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return haveOpenGl() ? dir : nullptr;
}

/// An empty profile folder under the temporary directory.
std::filesystem::path freshProfileDir(std::string_view name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "fh1-scenarios-profiles" / name;
    std::filesystem::remove_all(dir);
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

bool haveOpenGl()
{
    static const bool available = [] {
        try {
            window();
            return true;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "skipped: no OpenGL context (%s)\n", e.what());
            return false;
        }
    }();
    return available;
}

/// What the player holds during a stretch of a scenario.
struct Hold {
    float throttle = 0.0F;
    float brake = 0.0F;
    /// -1 full left to 1 full right.
    float steer = 0.0F;
    /// The right stick's y: on the map, positive zooms in.
    float zoom = 0.0F;
    /// The left stick's y: on the map, positive moves the cursor north.
    float cursorY = 0.0F;
};

/// A game and the scripted player driving it.
class Session {
public:
    explicit Session(fh::GameOptions options)
    {
        options.gameDir = gameDir();
        if (options.profileDir.empty()) {
            // A new profile for every scenario, so none sees what another
            // discovered.
            static int sessions = 0;
            options.profileDir = freshProfileDir(std::format("session{}", ++sessions));
        }
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

    /// As press(), for a button that may quit the game: false when it did.
    bool pressMayQuit(fh::Button button) { return step({}, button) && step({}, std::nullopt); }

    /// Saves the last frame when FH1_CAPTURE_DIR asks for screenshots.
    void capture(const std::string& name)
    {
        const char* dir = std::getenv("FH1_CAPTURE_DIR");
        if (dir != nullptr && *dir != '\0') {
            m_game->saveScreenshot(std::filesystem::path(dir) / (name + ".png"));
        }
    }

private:
    void frame(const Hold& hold, std::optional<fh::Button> button = std::nullopt) { CHECK(step(hold, button)); }

    /// One frame; false when the game quits.
    bool step(const Hold& hold, std::optional<fh::Button> button)
    {
        fh::InputState input;
        input.previousButtons = m_previousButtons;
        input.axes[static_cast<std::size_t>(fh::Axis::RightTrigger)] = hold.throttle;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftTrigger)] = hold.brake;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftX)] = hold.steer;
        input.axes[static_cast<std::size_t>(fh::Axis::RightY)] = hold.zoom;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftY)] = hold.cursorY;
        if (button) {
            input.buttons[static_cast<std::size_t>(*button)] = true;
        }
        m_previousButtons = input.buttons;
        return m_game->frame(input, kStep);
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

/// The width the map overlay (0x828b6998) scales its cursor to: the
/// crosshair's 128 units as authored (a 100-unit rectangle at scale 1.28)
/// times cursorSize * (0.75 + 0.25 * distance / cursorSizeDistance) /
/// distance * viewport width / cursorAnarkSize, the viewport being the
/// scene's 1280-unit wide MODEL_MAP_VIEWPORT.
float expectedCursorWidth(const fh::ui::WorldMapTuning& tuning, float distance)
{
    const float grow = 0.75F + 0.25F * distance / tuning.cursorSizeDistance;
    return 128.0F * tuning.cursorSize * grow / distance * 1280.0F / tuning.cursorAnarkSize;
}

// "The map cursor is too big." The game scales the cursor with the map
// camera's distance every frame; drawn at its authored size it covered
// about a tenth of the screen.
TEST(mapCursorKeepsTheGameSize)
{
    if (!gameDir()) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(gameDir());
    const fh::ui::WorldMapTuning tuning = fh::ui::WorldMapTuning::load(vfs);
    Session session({});
    CHECK(session.waitForCar());
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK(map != nullptr);
    if (map == nullptr) {
        return;
    }
    CHECK_NEAR(map->cursorExtent().x, expectedCursorWidth(tuning, map->camera().distance()), 0.5);
    session.capture("map_cursor");
    // Zoomed in, the camera is nearer and the cursor grows.
    session.run(2.0F, {.zoom = 1.0F});
    CHECK_NEAR(map->cursorExtent().x, expectedCursorWidth(tuning, map->camera().distance()), 0.5);
    session.capture("map_cursor_zoomed");
}

// "Selecting a route on the map doesn't show its itinerary." SET ROUTE on
// a marker gives the sat-nav the waypoint, which searches the roads from
// the car's node to the node nearest the waypoint; the map draws that
// route, and it stays while the car drives.
TEST(setRouteShowsTheItinerary)
{
    if (!gameDir()) {
        return;
    }
    // On colorado's way 33, the b road "road_094", facing along it.
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK(navigation != nullptr && map != nullptr);
    if (navigation == nullptr || map == nullptr) {
        return;
    }
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    // The cursor goes east until it rests on a marker at least 500 m from
    // the car.
    const glm::vec2 car = session.game().currentMapState().playerPosition;
    const auto onFarMarker = [&] {
        const fh::ui::WorldMapMarker* marker = map->hovered();
        return marker != nullptr && glm::length(marker->marker.position - car) > 500.0F;
    };
    for (int frame = 0; frame < 1200 && !onFarMarker(); ++frame) {
        session.run(kStep, {.steer = 0.25F});
    }
    CHECK(onFarMarker());
    session.capture("route_before");
    session.press(fh::Button::A);
    CHECK(map->waypoint().has_value());
    CHECK(navigation->destination().has_value());
    const std::vector<glm::vec2> route = navigation->route();
    CHECK(route.size() > 2);
    if (route.empty() || !map->waypoint() || !navigation->playerNode()) {
        return;
    }
    // From the car's node...
    const glm::vec3 start = navigation->network().nodes[*navigation->playerNode()].position;
    CHECK(glm::length(route.front() - glm::vec2(start.x, start.z)) < 1e-3F);
    CHECK(glm::length(route.front() - car) < 100.0F);
    // ...to the node nearest the waypoint.
    const std::optional<std::uint32_t> goal
        = navigation->locator().nearestOnGround(*map->waypoint(), fh::ui::NavFilter::NoShortcutOrNone);
    CHECK(goal.has_value() && navigation->destination() == goal);
    if (goal) {
        const glm::vec3 end = navigation->network().nodes[*goal].position;
        CHECK(glm::length(route.back() - glm::vec2(end.x, end.z)) < 1e-3F);
    }
    // The map shows it, drawing in, then whole.
    CHECK(session.game().currentMapState().route.size() == route.size());
    CHECK(map->routeReveal() < 1.0F);
    session.run(2.0F, {});
    CHECK_NEAR(map->routeReveal(), 1.0, 1e-6);
    session.capture("route_map");

    // Driving on, the route stays and the car's place moves along it.
    session.press(fh::Button::B);
    session.run(6.0F, {.throttle = 0.6F});
    CHECK(navigation->destination().has_value());
    CHECK(!navigation->routeNodes().empty());
    session.capture("route_minimap");
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

/// The flare of `object`'s activation zone: whether it runs, and the
/// particles alive in the systems it runs in.
std::pair<bool, std::size_t> zoneFlare(const fh::Game& game, std::string_view object)
{
    const fh::particles::ParticleManager* particles = game.particleManager();
    if (particles == nullptr) {
        return {false, 0};
    }
    const fh::particles::SimpleEmitterState* flare = particles->simpleEmitter(std::string(object) + "_flare");
    if (flare == nullptr) {
        return {false, 0};
    }
    std::size_t live = 0;
    for (const fh::particles::ParticleSystem* system : particles->systemsOf(*flare)) {
        live += system->size();
    }
    return {flare->running, live};
}

// An event start burns the flare the game places by its zone
// (ParticleEmitters.xml's FR04_flare, a _Composite_Flare_6) while the car is
// within 500 m; a start farther away does not.
TEST(eventStartBurnsItsFlare)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    // 60 m north of the FR04 zone, outside its 25 m prompt, facing it.
    options.spawnAt = glm::vec2(-1958.7F, 52.7F);
    options.spawnHeading = std::numbers::pi_v<float>;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    fh::Game& game = session.game();
    session.capture("event_start_flare_fr04");
    const auto [running, live] = zoneFlare(game, "FR04");
    CHECK(running);
    CHECK(live > 0);
    // FR17's zone is over a kilometre away.
    CHECK(!zoneFlare(game, "FR17").first);
}

// In a race the free-roam zones are disabled, and their flares are out.
TEST(raceNoZoneFlareBurns)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    const fh::particles::ParticleManager* particles = session.game().particleManager();
    CHECK(particles != nullptr);
    if (particles != nullptr) {
        CHECK(std::none_of(particles->simpleEmitters().begin(), particles->simpleEmitters().end(),
            [](const fh::particles::SimpleEmitterState& e) { return e.running; }));
    }
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

// "Add a game progression system so stuff is saved": what the player found
// in free roam was gone after a restart. A speed trap's first pass is a new
// personal best, which makes the game save its profile ("Activity Speed
// Camera: New PB", 0x8296f76c), roads driven included; the next game loads
// it back.
TEST(progressIsKeptAcrossRestarts)
{
    if (!gameDir()) {
        return;
    }
    const glm::vec2 gate{2274.2F, 319.6F};
    const glm::vec2 north = glm::normalize(glm::vec2(0.1167F, 0.9932F));
    fh::GameOptions options;
    options.spawnAt = gate - north * 120.0F;
    options.spawnHeading = std::atan2(north.x, north.y);
    options.profileDir = freshProfileDir("restart");
    float best = 0.0F;
    std::vector<bool> driven;
    {
        Session session(options);
        CHECK(session.waitForCar());
        const fh::SpeedTraps* traps = session.game().speedTraps();
        const fh::SpeedTrap* trap = traps ? traps->find("speed_camera_01") : nullptr;
        CHECK(trap != nullptr);
        if (trap == nullptr) {
            return;
        }
        for (float t = 0.0F; t < 20.0F && !trap->discovered; t += kStep) {
            session.run(kStep, {.throttle = 1.0F});
        }
        CHECK(trap->discovered);
        CHECK_EQ(session.game().profileSaves(), std::size_t{1});
        best = trap->best.value_or(0.0F);
        CHECK(best >= 30.0F * 0.44704F);
        // The save came at the end of that frame, with the roads driven up to
        // it; those driven after wait for the next save, as in the game.
        const fh::ui::Navigation* navigation = session.game().navigation();
        CHECK(navigation != nullptr);
        if (navigation != nullptr) {
            driven = navigation->coverage().visitedNodes();
        }
        CHECK(std::count(driven.begin(), driven.end(), true) > 0);
        CHECK(std::filesystem::exists(options.profileDir / "ForzaProfile.xml"));
        session.run(0.5F, {});
    }
    Session session(options);
    CHECK(session.waitForCar());
    fh::Game& game = session.game();
    const fh::SpeedTrap* trap = game.speedTraps() ? game.speedTraps()->find("speed_camera_01") : nullptr;
    CHECK(trap != nullptr);
    CHECK(trap && trap->discovered);
    CHECK(trap && trap->best.value_or(0.0F) == best);
    CHECK(hasMarker(game.currentMapState(), "speed_camera_discovered"));
    const fh::ui::Navigation* navigation = game.navigation();
    CHECK(navigation != nullptr);
    if (navigation != nullptr) {
        const std::vector<bool> now = navigation->coverage().visitedNodes();
        std::size_t kept = 0;
        for (std::size_t node = 0; node < driven.size() && node < now.size(); ++node) {
            kept += driven[node] && now[node] ? 1 : 0;
        }
        CHECK_EQ(kept, static_cast<std::size_t>(std::count(driven.begin(), driven.end(), true)));
    }
}

// The player chose that leaving the game keeps the roads driven since the
// last save, which the console game has no moment for: QUIT in the pause
// menu saves, and so does ending the game any other way.
TEST(quittingKeepsTheRoadsDriven)
{
    if (!gameDir()) {
        return;
    }
    // Colorado's road_094, as in drivenRoadsColourTheMap, far from any
    // speed trap, outpost or discount sign.
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    options.profileDir = freshProfileDir("quit");
    const auto drivenCount = [](const std::vector<bool>& nodes) {
        return static_cast<std::size_t>(std::count(nodes.begin(), nodes.end(), true));
    };
    const auto keeps = [](const std::vector<bool>& before, const std::vector<bool>& now) {
        for (std::size_t node = 0; node < before.size(); ++node) {
            if (before[node] && (node >= now.size() || !now[node])) {
                return false;
            }
        }
        return true;
    };
    const auto visited = [](Session& session) {
        const fh::ui::Navigation* navigation = session.game().navigation();
        return navigation != nullptr ? navigation->coverage().visitedNodes() : std::vector<bool>{};
    };
    std::vector<bool> beforeQuit;
    {
        Session session(options);
        CHECK(session.waitForCar());
        session.run(4.0F, {.throttle = 0.7F});
        session.run(3.0F, {.brake = 1.0F});
        CHECK_EQ(session.game().profileSaves(), std::size_t{0});
        beforeQuit = visited(session);
        CHECK(drivenCount(beforeQuit) > 1);
        session.press(fh::Button::Start);
        const fh::ui::PauseMenu* menu = session.game().pauseMenu();
        CHECK(menu != nullptr);
        if (menu == nullptr) {
            return;
        }
        for (std::size_t i = 0; i < menu->entries().size()
            && menu->entries()[static_cast<std::size_t>(menu->focus())].item != fh::ui::PauseItem::Quit;
            ++i) {
            session.press(fh::Button::DpadDown);
        }
        CHECK(menu->entries()[static_cast<std::size_t>(menu->focus())].item == fh::ui::PauseItem::Quit);
        CHECK(session.pressMayQuit(fh::Button::A));
        CHECK(menu->confirming());
        // EXIT SINGLE PLAYER, confirmed: the game quits, saving once.
        CHECK(!session.pressMayQuit(fh::Button::A));
        CHECK_EQ(session.game().profileSaves(), std::size_t{1});
    }
    std::vector<bool> beforeClose;
    {
        Session session(options);
        CHECK(session.waitForCar());
        CHECK(keeps(beforeQuit, visited(session)));
        // Then further down the road, and the game simply ends (the window
        // closed).
        session.run(12.0F, {.throttle = 0.7F});
        CHECK_EQ(session.game().profileSaves(), std::size_t{0});
        beforeClose = visited(session);
        CHECK(drivenCount(beforeClose) > drivenCount(beforeQuit));
    }
    Session session(options);
    CHECK(session.waitForCar());
    CHECK(keeps(beforeClose, visited(session)));
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
    // While track effects link, the screen says so in place of PLEASE WAIT.
    bool compilingShown = false;
    for (float t = 0.0F; t < 180.0F && game.loadingStatus().screenShown; t += kStep) {
        session.run(kStep, {});
        const fh::Game::LoadingStatus during = game.loadingStatus();
        if (!compilingShown && during.screenShown && during.programsPending > 0
            && during.message.starts_with("Compiling shaders ")) {
            compilingShown = true;
            session.capture("loading_status");
        }
    }
    CHECK(compilingShown);
    const fh::Game::LoadingStatus status = game.loadingStatus();
    CHECK(!status.screenShown);
    CHECK_EQ(status.pendingJobs, std::size_t{0});
    CHECK(status.nearbyWorldReady);
    CHECK_EQ(status.programsPending, std::size_t{0});
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

/// GPU milliseconds of the world pass (the frame statistics' DrawWorld
/// part), averaged over one second of `session` standing still.
double worldPassMs(Session& session)
{
    double seconds = 0.0;
    int frames = 0;
    for (float t = 0.0F; t < 1.0F; t += kStep) {
        session.run(kStep, {});

        ++frames;
    }
    return seconds * 1000.0 / frames;
}

// Free roam draws its own forests: the procedural tree sets behind an event
// block for route 0 (models_proc_clrd_trees_mountains_area4a[freeroam]_*
// stand around here), which were never loaded.
TEST(freeRoamDrawsItsForests)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.spawnAt = glm::vec2(4330.0F, -250.0F);
    options.spawnHeading = 0.0F;
    options.startTime = 12.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(4.0F, {});
    const fh::render::WorldRenderer* world = session.game().worldRenderer();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }
    CHECK_EQ(world->eventRoute(), 0);
    CHECK(world->eventChunksIncluded() > 0);
    std::fprintf(stderr, "forest: world pass GPU %.3f ms\n", worldPassMs(session));
    session.capture("forest_area4a");
}

// A race puts out its route's props (barriers, chevrons, gantries) and
// leaving it takes them away again, back to free roam's.
TEST(raceShowsItsRoutePropsUntilItEnds)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    fh::Game& game = session.game();
    const fh::render::WorldRenderer* world = game.worldRenderer();
    CHECK(world != nullptr && game.race() != nullptr);
    if (world == nullptr || game.race() == nullptr) {
        return;
    }
    // FR02 runs on route 96 (FESTIVAL_PLNS_006).
    CHECK_EQ(world->eventRoute(), game.race()->event().routeId);
    CHECK_EQ(world->eventRoute(), 96);
    const std::size_t raceChunks = world->eventChunksIncluded();
    CHECK(raceChunks > 0);
    std::fprintf(stderr, "race grid: %zu event chunks, world pass GPU %.3f ms\n", raceChunks, worldPassMs(session));
    session.capture("race_props_grid");

    session.press(fh::Button::Start);
    const fh::ui::PauseMenu* menu = game.pauseMenu();
    CHECK(menu != nullptr);
    if (menu == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < menu->entries().size()
        && menu->entries()[static_cast<std::size_t>(menu->focus())].item != fh::ui::PauseItem::QuitRace;
        ++i) {
        session.press(fh::Button::DpadDown);
    }
    CHECK(menu->entries()[static_cast<std::size_t>(menu->focus())].item == fh::ui::PauseItem::QuitRace);
    session.press(fh::Button::A);
    CHECK(menu->confirming());
    session.press(fh::Button::A);
    session.run(2.0F, {});
    CHECK(game.race() == nullptr);
    CHECK_EQ(world->eventRoute(), 0);
    CHECK(world->eventChunksIncluded() != raceChunks);
    session.capture("race_props_after");
}

// Flags, water, waterfalls, flipbook screens and swaying trees animate on
// TimeGain, the presentation's clock: it runs with the game and stops while
// the game is paused.
TEST(trackAnimationClockStopsWhilePaused)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    fh::Game& game = session.game();
    const fh::render::WorldRenderer* world = game.worldRenderer();
    CHECK(world != nullptr);
    if (world == nullptr) {
        return;
    }
    const float running = world->timeGain().x;
    CHECK(running > 1.0F);
    CHECK_NEAR(running, fh::render::TrackFrameConstants::timeGain(game.animationTime()).x, 0.05);
    CHECK_NEAR(world->timeGain().w, 1.0, 1e-6);

    session.press(fh::Button::Start);
    CHECK(game.pauseShown());
    session.run(0.1F, {});
    const float paused = world->timeGain().x;
    session.run(2.0F, {});
    CHECK_NEAR(world->timeGain().x, paused, 1e-6);

    session.press(fh::Button::B);
    CHECK(!game.pauseShown());
    session.run(1.0F, {});
    CHECK(world->timeGain().x > paused + 0.9F);
    CHECK_NEAR(fh::render::TrackFrameConstants::timeGain(28800.0F + 5.0F).x, 5.0, 0.01);
}

// "The world still feels horrible": the track effects read scene
// constants that were never set. Night lights the emissive maps (signs,
// windows) and day leaves them off, as SwitchOnLights says, and the
// festival's reflective materials sample the track's own cube maps.
TEST(trackShadingSwitchesLightsAndReflects)
{
    if (!gameDir()) {
        return;
    }
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
        fh::GameOptions options;
        options.startTime = shot.seconds;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(4.0F, {});
        const fh::render::WorldRenderer* world = session.game().worldRenderer();
        CHECK(world != nullptr);
        if (world == nullptr) {
            return;
        }
        const glm::vec4 emissive = world->emissiveSwitchOn();
        CHECK_EQ(emissive.z, 1.0F);
        if (shot.lit) {
            CHECK(emissive.x >= fh::render::TrackFrameConstants::kSwitchOnLightsFloor);
        } else {
            CHECK_EQ(emissive.x, -1.0F);
        }
        CHECK(world->cubeMapsLoaded() > 0);
        std::fprintf(stderr, "%s: world pass GPU %.3f ms\n", shot.name, worldPassMs(session));
        session.capture(shot.name);
    }
}

// "On the map you can't select a destination somewhere on the road. Only
// events." With the cursor on no marker, A hands the sat-nav the point of
// the roads nearest the cursor (the map overlay's update, 0x828b6998), and
// the route goes to that point's node; A again on it clears the route.
TEST(setRouteOnARoadAwayFromMarkers)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK(navigation != nullptr && map != nullptr);
    if (navigation == nullptr || map == nullptr) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(gameDir());
    const std::vector<fh::ui::MapMarker> activities = fh::ui::loadActivityMarkers(vfs, "colorado");
    session.press(fh::Button::Back);
    session.run(1.0F, {});

    // The cursor goes west until it is 800 m from the car with no marker
    // within 400 m of it.
    const glm::vec2 car = session.game().currentMapState().playerPosition;
    const auto clearOfMarkers = [&] {
        const glm::vec2 at = map->camera().cursor();
        const auto near = [&](glm::vec2 marker) { return glm::length(marker - at) < 400.0F; };
        for (const fh::ui::MapMarker& marker : activities) {
            if (near(marker.position)) {
                return false;
            }
        }
        for (const fh::ui::WorldMapMarker& marker : session.game().currentMapState().markers) {
            if (near(marker.marker.position)) {
                return false;
            }
        }
        return glm::length(at - car) > 800.0F;
    };
    for (int frame = 0; frame < 1800 && !clearOfMarkers(); ++frame) {
        session.run(kStep, {.steer = -0.25F});
    }
    session.run(0.5F, {});
    CHECK(clearOfMarkers());
    CHECK(map->hovered() == nullptr);
    const std::optional<glm::vec2> target = map->target();
    CHECK(target.has_value());
    if (!target) {
        return;
    }
    // The target is the pick of the profile's road lines (a, b, dirt and
    // freeway ways, SnapSize 60 each) nearest the cursor.
    const fh::ui::NavFile& nav = navigation->network();
    std::optional<fh::ui::MapLinePick> best;
    for (const fh::ui::NavWay& way : nav.ways) {
        const std::string_view type = way.tag("road_type");
        if (type != "a" && type != "b" && type != "dirt" && type != "freeway") {
            continue;
        }
        const std::optional<fh::ui::MapLinePick> pick = fh::ui::pickMapLine(way.points, map->camera().cursor(), 60.0F);
        if (pick && (!best || pick->distance < best->distance)) {
            best = pick;
        }
    }
    CHECK(best.has_value());
    if (best) {
        CHECK(glm::length(best->point - *target) < 1e-3F);
    }
    session.capture("route_road_before");

    session.press(fh::Button::A);
    CHECK(map->waypoint().has_value());
    CHECK(navigation->destination().has_value());
    const std::optional<std::uint32_t> goal
        = navigation->locator().nearestOnGround(*target, fh::ui::NavFilter::NoShortcutOrNone);
    CHECK(goal.has_value() && navigation->destination() == goal);
    CHECK(map->targetNode() == goal);
    const std::vector<glm::vec2> route = navigation->route();
    CHECK(route.size() > 2);
    if (goal && !route.empty()) {
        const glm::vec3 end = nav.nodes[*goal].position;
        CHECK(glm::length(route.back() - glm::vec2(end.x, end.z)) < 1e-3F);
    }
    CHECK_EQ(session.game().currentMapState().route.size(), route.size());
    session.run(2.0F, {});
    session.capture("route_road_map");

    // A on the destination's node clears it (CLEAR ROUTE).
    session.press(fh::Button::A);
    CHECK(!map->waypoint().has_value());
    CHECK(!navigation->destination().has_value());
    CHECK(navigation->routeNodes().empty());
}

// A text the scene draws now, with some opacity.
bool drawsText(const fh::ui::Presentation& scene, std::string_view text)
{
    const std::vector<fh::ui::DrawItem> items = scene.evaluate();
    return std::any_of(items.begin(), items.end(), [&](const fh::ui::DrawItem& item) {
        return item.isText && item.text.text == text && item.text.color.a > 0.0F;
    });
}

// "The map doesn't have the profile elements such as wristband, credits."
// h_map's PLAYER_BAR shows the profile's Main/Credits, the points from
// Main/XP to the band after Main/WristbandLevel, and the car.
TEST(mapShowsTheProfilesPlayerBar)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.profileDir = freshProfileDir("playerbar");
    std::filesystem::create_directories(options.profileDir);
    CHECK(fh::writeFileToDisk(options.profileDir / "ForzaProfile.xml", R"(<ForzaProfile version="1"><Profile>
      <Property name="Main" type="PropertyBag">
        <Property name="Credits" type="UInt32" value="1234567"/>
        <Property name="XP" type="Int32" value="500"/>
        <Property name="WristbandLevel" type="Int32" value="1"/>
      </Property>
    </Profile></ForzaProfile>)"));
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK(map != nullptr && session.game().mapShown());
    if (map == nullptr) {
        return;
    }
    const fh::ui::Presentation& scene = map->scene();
    const fh::ui::PlayerBar& bar = map->playerBar();
    const auto textOf = [&](int element) { return element >= 0 ? scene.text(element) : std::string(); };
    CHECK(bar.bound());
    CHECK_EQ(textOf(bar.creditsText()), std::string("1,234,567 Cr"));
    // The band after the green one (Id 1) needs 750 XP.
    CHECK_EQ(textOf(bar.wristbandText()), std::string("250 PTS"));
    CHECK_EQ(textOf(bar.nextLevelText()), std::string("Nxt Lvl"));
    CHECK_EQ(textOf(bar.carText()), std::string("Alfa Romeo 8C Competizione"));
    CHECK(drawsText(scene, "1,234,567 Cr"));
    CHECK(drawsText(scene, "250 PTS"));
    session.capture("map_player_bar");
}

// "When a direction is selected, the number of km is supposed to be
// shown." The HUD's sat-nav readout shows the planner's distance left
// (0x8266cf20 reads its +624) in kilometres with one decimal, and it goes
// down as the car drives along the route.
TEST(satNavShowsTheDistanceLeft)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    options.metric = true;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    const fh::ui::WorldMap* map = session.game().worldMap();
    const fh::ui::Hud* hud = session.game().hud();
    CHECK(navigation != nullptr && map != nullptr && hud != nullptr);
    if (navigation == nullptr || map == nullptr || hud == nullptr) {
        return;
    }
    CHECK(!session.game().currentHudState().destinationDistance.has_value());
    // The car faces south along its road: the destination is that road
    // 800 m to the south, so the route runs ahead of the car.
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    const glm::vec2 car = session.game().currentMapState().playerPosition;
    for (int frame = 0; frame < 1200 && car.y - map->camera().cursor().y < 800.0F; ++frame) {
        session.run(kStep, {.cursorY = -0.25F});
    }
    session.run(0.5F, {});
    CHECK(map->hovered() == nullptr);
    session.press(fh::Button::A);
    session.press(fh::Button::B);
    session.run(0.5F, {});
    CHECK(navigation->destination().has_value());

    // What is left: from the car to the route's node after its place, then
    // along the route.
    const auto left = [&] {
        const std::vector<std::uint32_t>& route = navigation->routeNodes();
        const std::size_t next = navigation->routeProgress() + 1;
        if (next >= route.size()) {
            return 0.0F;
        }
        const fh::ui::NavFile& nav = navigation->network();
        float distance = glm::length(session.game().playerCar().position() - nav.nodes[route[next]].position);
        for (std::size_t i = next + 1; i < route.size(); ++i) {
            distance += glm::length(nav.nodes[route[i]].position - nav.nodes[route[i - 1]].position);
        }
        return distance;
    };
    const auto shown = [&] {
        return std::format("{:.1f} KM", session.game().currentHudState().destinationDistance.value_or(0.0F) / 1000.0F);
    };
    const std::optional<float> first = session.game().currentHudState().destinationDistance;
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    CHECK_NEAR(*first, left(), 1.0);
    CHECK_EQ(hud->satNavDistanceText(), shown());
    session.capture("satnav_distance");

    // Driving on along the route until the car's place moves along it,
    // then stopping there: the readout counts down with the car.
    for (int frame = 0; frame < 1200 && navigation->routeProgress() < 2; ++frame) {
        session.run(kStep, {.throttle = 0.5F});
    }
    CHECK(navigation->routeProgress() >= 2);
    session.run(3.0F, {.brake = 1.0F});
    CHECK(navigation->destination().has_value());
    const std::optional<float> later = session.game().currentHudState().destinationDistance;
    CHECK(later.has_value());
    if (later) {
        CHECK(*later < *first - 20.0F);
        CHECK_NEAR(*later, left(), 1.0);
    }
    CHECK_EQ(hud->satNavDistanceText(), shown());
    session.capture("satnav_distance_driven");
}

/// The share of the sun shadow mask's pixels that are shadowed (below
/// white), and the GPU milliseconds of the shadow passes over one second
/// of `session` standing still.
struct ShadowReading {
    double shadowed = 0.0;
    double gpuMs = 0.0;
    /// Median scene luminance (linear HDR) of shadowed and of sunlit pixels
    /// in the lower half of the picture, below the sky.
    double shadowedLuminance = 0.0;
    double litLuminance = 0.0;
};

ShadowReading readShadows(Session& session)
{
    ShadowReading reading;
    int frames = 0;
    for (float t = 0.0F; t < 1.0F; t += kStep) {
        session.run(kStep, {});
        reading.gpuMs += session.game()
                             .frameStats()
                             .gpuFrameSeconds()[static_cast<std::size_t>(fh::render::FrameStats::Part::DrawShadows)];
        ++frames;
    }
    reading.gpuMs *= 1000.0 / frames;
    const fh::render::ShadowPass* pass = session.game().shadowPass();
    if (pass == nullptr || pass->width() <= 0) {
        return reading;
    }
    std::vector<std::uint8_t> red(static_cast<std::size_t>(pass->width()) * static_cast<std::size_t>(pass->height()));
    glGetTextureImage(pass->mask(), 0, GL_RED, GL_UNSIGNED_BYTE, static_cast<GLsizei>(red.size()), red.data());
    const auto shadowed = std::count_if(red.begin(), red.end(), [](std::uint8_t v) { return v < 250; });
    reading.shadowed = static_cast<double>(shadowed) / static_cast<double>(red.size());
    const int w = pass->width();
    const int h = pass->height();
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
            const double luminance = 0.2126 * hdr[i] + 0.7152 * hdr[i + 1] + 0.0722 * hdr[i + 2];
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
TEST(sunShadowsFallByDayOnly)
{
    if (!gameDir()) {
        return;
    }
    struct Shot {
        const char* name;
        float hours;
        bool shadows;
    };
    for (const Shot shot :
        {Shot{"shadows_noon", 12.0F, true}, Shot{"shadows_low_sun", 18.5F, true}, Shot{"shadows_night", 2.0F, false}}) {
        fh::GameOptions options;
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
TEST(postProcessingAtTimesAndInAZone)
{
    if (!gameDir()) {
        return;
    }
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
        fh::GameOptions options;
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
            tonemapSeconds += session.game()
                                  .frameStats()
                                  .gpuFrameSeconds()[static_cast<std::size_t>(fh::render::FrameStats::Part::Tonemap)];
            ++frames;
        }
        std::fprintf(stderr, "%s: post-processing GPU %.3f ms\n", shot.name, tonemapSeconds * 1000.0 / frames);
        session.capture(shot.name);
        CHECK_EQ(std::string(session.game().postProcessingZone()), std::string(shot.zone));
        const fh::PostSettings* settings = session.game().postSettings();
        CHECK(settings != nullptr);
        if (settings != nullptr) {
            CHECK(settings->filmic.white >= shot.whiteMin - 1e-4F && settings->filmic.white <= shot.whiteMax + 1e-4F);
        }
    }
}

// The sky had no clouds: CRealtimeSky draws its far and close cloud layers
// over the dome.
TEST(skyDrawsItsClouds)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.spawnAt = glm::vec2(4330.0F, -250.0F);
    options.spawnHeading = 0.0F;
    options.startTime = 13.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(3.0F, {});
    CHECK(session.game().clouds() != nullptr);
    session.capture("clouds_sky");
}

/// Checks a help bar as the game lays it out: each button's SUPER_STACKER
/// fits its pill to the logo and text, so every pill ends the same way past
/// its text, and the bar (0x8289e100) starts each button 34 units after
/// the text before it.
void checkHelpBarFits(const std::vector<fh::ui::HelpButtonExtent>& extents, std::size_t shown)
{
    CHECK_EQ(extents.size(), shown);
    for (std::size_t i = 0; i < extents.size(); ++i) {
        const fh::ui::HelpButtonExtent& button = extents[i];
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
TEST(helpBarsFitTheirTexts)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    session.press(fh::Button::Back);
    for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        window().setSize(width, height);
        session.run(1.0F, {});
        const fh::ui::WorldMap* map = session.game().worldMap();
        CHECK(map != nullptr && session.game().mapShown());
        if (map != nullptr) {
            checkHelpBarFits(map->helpExtents(), 4);
        }
        session.capture(std::format("help_map_{}x{}", width, height));
    }
    session.press(fh::Button::B);
    session.run(1.0F, {});
    session.press(fh::Button::Start);
    for (const auto& [width, height] : {std::pair{1920, 1080}, std::pair{2560, 1440}}) {
        window().setSize(width, height);
        session.run(1.0F, {});
        CHECK(session.game().pauseShown());
        if (const fh::ui::PauseMenu* menu = session.game().pauseMenu()) {
            checkHelpBarFits(menu->helpExtents(), 2);
        }
        session.capture(std::format("help_pause_{}x{}", width, height));
    }
    window().setSize(640, 360);
}

// A player's screenshot near "the media centre": HORIZON OUTPOST over MEDIA
// CENTER with a race icon. The media centre is outpost GasStation_05 (the
// GASSTATION_005 zone, named IDS_GasStationName_05 in gas_stations.xml),
// so its title and name are the game's; the icon was not. An outpost's
// discovery sets its zone's prompt icon to 33 (0x82972dcc), which the
// prompt shows as SET_EVENTICON_PRHUB (0x8289acf8).
TEST(mediaCenterOutpostPromptShowsThePrHubIcon)
{
    if (!gameDir()) {
        return;
    }
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
    CHECK_EQ(state.promptTitle, std::string("HORIZON OUTPOST"));
    CHECK_EQ(state.prompt, std::string("MEDIA CENTER"));
    CHECK_EQ(state.promptDetail, std::string("ENTER NOW"));
    CHECK_EQ(state.promptIcon, std::string("PRHUB"));
    CHECK_EQ(hud->promptIconSlide(), std::string("PRHUB"));
}

// The prompt control sets the icon from every status it shows (0x828a25f0
// passes the status's icon to 0x8289acf8), and a status without one shows
// SET_EVENTICON_DEFAULT. A prompt without an icon after an outpost's must
// not keep the outpost's.
TEST(promptWithoutAnIconShowsTheDefaultIcon)
{
    const char* dir = gameDir();
    if (dir == nullptr) {
        return;
    }
    // The session provides the OpenGL context the HUD draws with.
    Session session({});
    CHECK(session.waitForCar());
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    fh::ui::Hud hud(vfs, "colorado");
    fh::ui::HudState state;
    state.dt = kStep;
    state.prompt = "MEDIA CENTER";
    state.promptTitle = "HORIZON OUTPOST";
    state.promptDetail = "ENTER NOW";
    state.promptIcon = "PRHUB";
    hud.draw(state, 1280, 720);
    CHECK_EQ(hud.promptIconSlide(), std::string("PRHUB"));
    state.prompt.clear();
    hud.draw(state, 1280, 720);
    state.prompt = "COLORADO SPRINT";
    state.promptTitle.clear();
    state.promptIcon.clear();
    hud.draw(state, 1280, 720);
    CHECK_EQ(hud.promptIconSlide(), std::string("DEFAULT"));
}

// Chrome, glass and paint reflected nothing: the car effect's
// reflectionScaler stayed at its default of 0 and envSampler was left
// empty. The game renders the environment cube around the focus car, all six
// faces a frame under the in-game render scenario, and sets
// reflectionScaler to 1 for every car draw. Captured where
// sunShadowsFallByDayOnly captures, at noon and in the low evening sun, so
// the two sets of pictures compare.
TEST(carsReflectTheEnvironmentCube)
{
    if (!gameDir()) {
        return;
    }
    struct Shot {
        const char* name;
        float hours;
    };
    for (const Shot shot : {Shot{"cube_noon", 12.0F}, Shot{"cube_low_sun", 18.5F}}) {
        fh::GameOptions options;
        options.spawnAt = glm::vec2(4330.0F, -250.0F);
        options.spawnHeading = 0.0F;
        options.startTime = shot.hours * 3600.0F;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(3.0F, {});
        const fh::Game& game = session.game();
        const fh::render::EnvironmentCube* cube = game.environmentCube();
        CHECK(cube != nullptr);
        const fh::render::CarRenderer* car = game.playerCarModel();
        CHECK(car != nullptr);
        if (cube == nullptr || car == nullptr) {
            continue;
        }
        CHECK_EQ(cube->facesDrawn(), 6);
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
        constexpr int kSize = fh::render::envcube::kSize >> kLevel;
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
        const fh::render::FrameStats& stats = game.frameStats();
        const auto part = static_cast<std::size_t>(fh::render::FrameStats::Part::DrawEnvironmentCube);
        std::fprintf(stderr, "%s: environment cube CPU %.3f ms, GPU %.3f ms\n", shot.name,
            stats.frameSeconds()[part] * 1000.0, stats.gpuFrameSeconds()[part] * 1000.0);
        session.capture(shot.name);
    }
}

/// The composite matrices the last frame posed the animated object placed
/// at `position` with, and its index; nothing when it was not drawn.
std::optional<std::pair<std::size_t, std::vector<glm::mat4>>> animatedPose(
    const fh::Game& game, const glm::vec3& position)
{
    const fh::animated::AnimatedObjects* objects = game.animatedObjects();
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
    fh::GameOptions options;
    const glm::vec2 away = glm::normalize(from);
    options.spawnAt = glm::vec2(object.x, object.z) + away * distance;
    options.spawnHeading = std::atan2(-away.x, -away.y);
    options.startTime = 12.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    fh::Game& game = session.game();
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
    CHECK(second && second->second != first->second);
    const fh::animated::AnimatedObjects& objects = *game.animatedObjects();
    const float played = objects.clipTime(first->first);

    session.press(fh::Button::Start);
    CHECK(game.pauseShown());
    const auto paused = animatedPose(game, object);
    const float pausedAt = objects.clipTime(first->first);
    session.run(1.0F, {});
    CHECK_NEAR(objects.clipTime(first->first), pausedAt, 1e-6);
    const auto still = animatedPose(game, object);
    CHECK(paused && still && paused->second == still->second);

    session.press(fh::Button::B);
    CHECK(!game.pauseShown());
    session.run(1.0F, {});
    CHECK(objects.clipTime(first->first) > played + 0.9F);
    const auto after = animatedPose(game, object);
    CHECK(after && still && after->second != still->second);
}

// The wind turbines and fairground rides of Colorado play their Granny
// animations on the presentation clock (see animated::AnimatedObjects): a
// wind turbine's rotor turns (a rigid part), and the ferris wheel turns (a
// skinned model), both stopping while paused. The windmills are not drawn:
// the textures their sets name (11101, 11170) are not on the disc.
TEST(windTurbineTurnsAndStopsWhilePaused)
{
    if (!gameDir()) {
        return;
    }
    // The first wind turbine of the row placed by set __R00G00211.
    checkAnimatedObject({-2240.8F, 17.0F, -1876.3F}, {0.0F, 1.0F}, 400.0F, "animated_turbine");
}

TEST(ferrisWheelTurnsAndStopsWhilePaused)
{
    if (!gameDir()) {
        return;
    }
    // The festival's ferris wheel (set __R00G00076), seen across its Z axis
    // (0.990, 0, -0.139), about which it turns.
    checkAnimatedObject({-1084.47F, -1.51F, -614.58F}, {-0.990F, 0.139F}, 150.0F, "animated_ferris");
}

// Free roam starts at the festival, among its crowds: the people near the
// camera are drawn as models, the rest as sprites, and their clips move on
// with the game's clock and stop while it is paused.
TEST(festivalCrowdIsDrawnAndAnimated)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    fh::Game& game = session.game();
    const fh::render::CrowdRenderer* crowd = game.crowdRenderer();
    CHECK(crowd != nullptr);
    if (crowd == nullptr) {
        return;
    }
    const fh::render::CrowdRenderer::Stats& stats = crowd->stats();
    std::fprintf(stderr,
        "festival crowd: models %zu %zu %zu %zu, walkers %zu, %zu sets in view, %zu sprites in %zu sets\n",
        stats.models[0], stats.models[1], stats.models[2], stats.models[3], stats.walkers, stats.visibleSets,
        stats.spriteInstances, stats.spriteSets);
    CHECK(stats.modelTotal() > 0);
    // The level bands cap the finer levels (5 and 10 people) and the queue
    // holds 500.
    CHECK(stats.models[0] <= 5 && stats.models[1] <= 10);
    CHECK(stats.modelTotal() <= 500);
    CHECK(stats.spriteInstances > 1000);
    session.capture("crowd_festival_near");

    // A second of play moves the clips on by the game's 30 frames, give or
    // take one where the second's ends fall.
    std::uint64_t before = crowd->ticksApplied();
    session.run(1.0F, {});
    CHECK(crowd->ticksApplied() - before >= 29 && crowd->ticksApplied() - before <= 31);
    session.press(fh::Button::Start);
    CHECK(game.pauseShown());
    session.run(0.1F, {});
    before = crowd->ticksApplied();
    session.run(1.0F, {});
    CHECK_EQ(crowd->ticksApplied(), before);
    session.press(fh::Button::B);
    session.run(1.0F, {});
    CHECK(crowd->ticksApplied() > before + 25);
}

// From 300 m behind free roam's start, looking the same way, the
// festival's crowd shows as sprites.
TEST(festivalCrowdFromAfarIsSprites)
{
    if (!gameDir()) {
        return;
    }
    // Free roam starts at (-836.6, -250.6) heading -63 degrees.
    const float heading = -63.0F * std::numbers::pi_v<float> / 180.0F;
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-836.6F, -250.6F) - 300.0F * glm::vec2(std::sin(heading), std::cos(heading));
    options.spawnHeading = heading;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    const fh::render::CrowdRenderer* crowd = session.game().crowdRenderer();
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
TEST(raceCrowdShowsAtTheStart)
{
    if (!gameDir()) {
        return;
    }
    fh::GameOptions options;
    options.race = "FR04";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    fh::Game& game = session.game();
    const fh::render::CrowdRenderer* crowd = game.crowdRenderer();
    CHECK(crowd != nullptr && game.race() != nullptr);
    if (crowd == nullptr || game.race() == nullptr) {
        return;
    }
    const fh::render::CrowdRenderer::Stats& stats = crowd->stats();
    std::fprintf(stderr, "FR04 start: %zu event sets in view of %zu, %zu models, %zu sprites\n", stats.visibleEventSets,
        stats.visibleSets, stats.modelTotal(), stats.spriteInstances);
    CHECK_EQ(game.race()->event().routeId, 108);
    CHECK(stats.visibleEventSets > 0);
    CHECK(stats.modelTotal() > 0);
    session.capture("crowd_race_start_near");
    // Far along the grid, once the race is on and the car has left the
    // start behind.
    for (float t = 0.0F; t < 30.0F && !game.race()->started(); t += kStep) {
        session.run(kStep, {});
    }
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
    explicit StreamingProbe(const fh::render::WorldRenderer& world)
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

    void frame(const fh::render::Camera& camera)
    {
        const fh::world::WorldTileGrid& grid = m_world.grid();
        const auto& chunks = grid.index().chunks();
        const glm::mat4 viewProjection = camera.projection(16.0F / 9.0F) * camera.view();
        const fh::world::ZoneGrid* zones = grid.index().zoneGrid();
        const int zone = zones != nullptr ? zones->zoneAt(camera.position.x, camera.position.z) : -1;
        m_drawnBefore.resize(chunks.size(), 0);
        m_residentFrame.resize(chunks.size(), -1);
        for (std::size_t t = 0; t < grid.tiles().size(); ++t) {
            const fh::world::WorldTileGrid::Tile& tile = grid.tiles()[t];
            if (grid.distanceTo(tile, camera.position.x, camera.position.z) > kStreamingWatchDistance) {
                continue;
            }
            for (const std::uint32_t c : m_world.drawnChunks(t)) {
                m_residentFrame[c] = m_frame;
            }
            for (const std::uint32_t c : tile.chunks) {
                const fh::world::WorldChunk& chunk = chunks[c];
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

    const fh::render::WorldRenderer& m_world;
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
TEST(drivingStreamsTheWorldInTime)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    fh::Game& game = session.game();
    const fh::render::WorldRenderer* world = game.worldRenderer();
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
    fh::test::RoadFollower follower(*game.navigation());
    fh::InputState previous;
    float driven = 0.0F;
    for (float t = 0.0F; t < kStreamingDriveSeconds; t += kStep) {
        fh::InputState input = follower.controls(game.playerCar());
        input.previousKeys = previous.keys;
        input.previousButtons = previous.buttons;
        previous = input;
        const glm::vec3 before = game.playerCar().position();
        CHECK(game.frame(input, kStep));
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
    CHECK_EQ(probe.missingAtStart, std::size_t{0});
    CHECK_EQ(probe.neverShown, 0);
    CHECK_EQ(probe.waiting(), std::size_t{0});
    CHECK(percentileOf(probe.delays, 1.0) <= kStreamingMaxDelayFrames);
    CHECK_EQ(appearedBeyond, std::size_t{0});
    CHECK_EQ(probe.drawnUnwanted, std::size_t{0});
    session.capture("streaming_end");
}

} // namespace

int main(int argc, char** argv)
{
    // Never open a window on the desktop or play through the speakers.
    fh::setEnvironmentVariable("SDL_VIDEO_DRIVER", "offscreen", false);
    fh::setEnvironmentVariable("SDL_AUDIO_DRIVER", "dummy", false);
    // Nor write to the player's own shader cache; this folder persists
    // between runs, so later runs also exercise the cached path.
    fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Cache),
        fh::pathToUtf8(std::filesystem::temp_directory_path() / "fh1-scenarios-cache"));
    // Nor the player's own profile: every Session names its own folder, and
    // this catches any game made without one.
    fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Data),
        fh::pathToUtf8(std::filesystem::temp_directory_path() / "fh1-scenarios-data"));
    fh::openLogFile(std::filesystem::temp_directory_path() / "fh1-scenarios.log");
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
