// Scenario tests: the whole game, run offscreen at a fixed step with
// scripted controls, checked through its own state. Each reproduces
// something a player does (pausing, opening the map, turning at low speed)
// and asserts what the game must do. They need the disc and skip without
// FH1_GAME_DIR. With FH1_CAPTURE_DIR set they also save screenshots there,
// for a person to look at (scripts/capture.sh).

#include "TestHarness.h"

#include "base/Log.h"
#include "car/CarPhysics.h"
#include "game/Game.h"
#include "game/Race.h"
#include "game/Title.h"
#include "platform/Input.h"
#include "platform/Window.h"
#include "ui/PauseMenu.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
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

    /// Runs frames until the player's car is on the ground, at most
    /// `seconds` of game time; loading streams on worker threads.
    bool waitForCar(float seconds = 120.0F)
    {
        for (float t = 0.0F; t < seconds && !m_game->playerPlaced(); t += kStep) {
            frame({});
        }
        return m_game->playerPlaced();
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
}

TEST(pauseMenuStopsTheGame)
{
    if (!gameDir()) {
        return;
    }
    Session session({});
    CHECK(session.waitForCar());
    session.run(3.0F, {.throttle = 1.0F});
    const glm::vec3 position = session.game().playerCar().position();
    const float speed = session.game().playerCar().speed();
    const float clock = session.game().timeOfDaySeconds();

    session.press(fh::Button::Start);
    CHECK(session.game().pauseShown());
    session.run(3.0F, {.throttle = 1.0F});
    session.capture("pause");
    CHECK(session.game().pauseShown());
    CHECK_NEAR(glm::length(session.game().playerCar().position() - position), 0.0, 1e-3);
    CHECK_NEAR(session.game().playerCar().speed(), speed, 1e-3);
    CHECK_NEAR(session.game().timeOfDaySeconds(), clock, 1e-3);

    // B is the help bar's RESUME.
    session.press(fh::Button::B);
    CHECK(!session.game().pauseShown());
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

    session.press(fh::Button::Back);
    CHECK(session.game().mapShown());
    session.run(1.0F, {});
    session.capture("map");
    CHECK(!session.game().worldDrawn());

    // B is the map's EXIT MAP.
    session.press(fh::Button::B);
    session.run(0.2F, {});
    CHECK(!session.game().mapShown());
    CHECK(session.game().worldDrawn());
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

} // namespace

int main(int argc, char** argv)
{
    // Never open a window on the desktop or play through the speakers.
    setenv("SDL_VIDEO_DRIVER", "offscreen", 0);
    setenv("SDL_AUDIO_DRIVER", "dummy", 0);
    fh::openLogFile(std::filesystem::temp_directory_path() / "fh1-scenarios.log");
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
