#pragma once

// What every scenario stands on: the fixture that skips without the disc
// or an OpenGL context, the one window of the process, and the Session that
// runs a game at the fixed step with a scripted player. New scenarios use
// these rather than helpers of their own.

#include "audio/MixSnapshots.h"
#include "game/Game.h"
#include "platform/Input.h"
#include "ui/PauseMenu.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Window;
namespace profile {
class Profile;
} // namespace profile
} // namespace openhorizon

namespace openhorizon::test {

/// The game runs at a fixed 60 Hz so every run of a scenario is the same.
inline constexpr float kStep = 1.0F / 60.0F;

/// The fixture of every scenario: it skips the case when FH1_GAME_DIR is
/// not set or the machine has no OpenGL context for the game, as on a CI
/// runner without a GPU.
class GameScenario {
public:
    GameScenario();

    /// The extracted disc.
    const char* gameDir() const { return m_gameDir; }
    /// The disc's file system, mounted once per process, for reading game
    /// files next to a session.
    const Vfs& disc() const;

private:
    const char* m_gameDir = nullptr;
};

/// One window for the whole process: the game allows one, and a small
/// offscreen one (640x360) keeps frames cheap. Scenarios that resize it put
/// it back to that size.
Window& window();

/// An empty profile folder under the temporary directory.
std::filesystem::path freshProfileDir(std::string_view name);

/// Saves a new profile into `dir`, as `edit` leaves it, so the next game
/// started on that folder loads it.
void saveProfile(const std::filesystem::path& dir, const std::function<void(profile::Profile&)>& edit);

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
    /// Starts a game on `options`, on the disc and in a new profile folder
    /// of its own unless `options` names one.
    explicit Session(GameOptions options);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Game& game() { return *m_game; }

    /// Runs frames until the player drives (loading and its fade over), at
    /// most `seconds` of game time; loading streams on worker threads.
    bool waitForCar(float seconds = 120.0F);

    /// Runs `seconds` of game time holding `hold`.
    void run(float seconds, const Hold& hold);

    /// Runs frames holding `hold` until `done` holds, at most `seconds` of
    /// game time. Returns whether `done` held.
    template <typename Done> bool runUntil(float seconds, Done done, const Hold& hold = {})
    {
        for (float t = 0.0F; t < seconds; t += kStep) {
            if (done()) {
                return true;
            }
            run(kStep, hold);
        }
        return done();
    }

    /// Presses and releases a button over two frames.
    void press(Button button);
    /// Presses a button `times` times, a frame apart.
    void pressTimes(Button button, int times);
    /// As press(), for a button that may quit the game: false when it did.
    bool pressMayQuit(Button button);

    /// Moves the pause menu's focus down to `item`, at most once round its
    /// entries. Returns whether the focus is on it.
    bool focusPauseItem(ui::PauseItem item);

    /// Saves the last frame when FH1_CAPTURE_DIR asks for screenshots.
    void capture(const std::string& name);

private:
    void frame(const Hold& hold, std::optional<Button> button = std::nullopt);
    /// One frame; false when the game quits.
    bool step(const Hold& hold, std::optional<Button> button);

    std::unique_ptr<Game> m_game;
    std::array<bool, static_cast<std::size_t>(Button::Count)> m_previousButtons{};
};

/// The scene's depth as the last frame left it, with the camera that drew
/// it, rows from the bottom as OpenGL stores them.
struct SceneReadback {
    int width = 0;
    int height = 0;
    std::vector<float> depth;
    glm::mat4 inverseViewProjection{1.0F};

    std::size_t at(int x, int y) const
    {
        return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x);
    }
    /// The world point the depth buffer holds at pixel (x, y).
    glm::vec3 worldAt(int x, int y) const;
};

/// Reads the scene's depth back; empty when the game has no scene yet.
SceneReadback readScene(Session& session);

/// Whether `sounds` holds `name`.
bool played(const std::vector<std::string>& sounds, std::string_view name);
/// The UI sounds played since `before` of them had been.
std::vector<std::string> playedSince(const Game& game, std::size_t before);
/// The mixer snapshot of the gameplay group, if the game has a mixer.
std::optional<audio::MixSnapshotId> gameplayMix(const Game& game);
/// Whether the map shows a marker of `value`, named `name` unless empty.
bool hasMarker(const ui::WorldMapState& state, std::string_view value, std::string_view name = {});

/// The heading of a horizontal direction: 0 north, positive east.
float headingOf(const glm::vec3& direction);
/// The angle between two headings, 0 to pi.
float angleBetween(float a, float b);
/// The stick a player holds to keep the car pointed at `target`.
float steerTowards(const CarPhysics& car, glm::vec2 target);

} // namespace openhorizon::test
