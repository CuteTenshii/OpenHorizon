#pragma once

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::ui {

/// What the HUD shows in one frame. Each field feeds the HUD control the
/// game itself drives with it; the comments name that control.
struct HudState {
    /// HUD gauge (0x828a0a08): the speed readout shows the speed in the
    /// chosen unit, truncated toward zero.
    float speedKmh = 0.0F;
    float speedMph = 0.0F;
    bool metric = false;
    /// -1 reverse, 0 neutral, otherwise the gear.
    int gear = 0;
    float rpm = 0.0F;
    /// The rev scale: the gauge setup (0x82865708) rounds `maxRpm` up to the
    /// next thousand for its top and puts the redline slider at
    /// `redlineRpm`.
    float redlineRpm = 7000.0F;
    float maxRpm = 8000.0F;

    /// Driving assists (0x8289b278 fires ABS_ON/OFF, STM_ON/OFF and
    /// TCS_ON/OFF when they change).
    bool absOn = false;
    bool stmOn = false;
    bool tcsOn = false;

    bool inRace = false;
    int position = 1;
    int racers = 1;
    int lap = 1;
    /// Laps of a circuit race; 1 or less for a point-to-point race.
    int laps = 1;

    /// The race timers (CHudTimersControl): the lines shown are the times
    /// present, in the game's slot order race, lap, last lap, best lap,
    /// target. Seconds.
    std::optional<float> raceTime;
    std::optional<float> lapTime;
    std::optional<float> lastLapTime;
    std::optional<float> bestLapTime;
    std::optional<float> targetTime;

    /// Seconds since the start countdown began; empty outside it. The game's
    /// countdown (0x828a3e00) shows 3 after 0 s, 2 after 1 s, 1 after 2 s and
    /// GO! after 3 s.
    std::optional<float> countdownElapsed;

    /// Racer names in race order and the player's index among them, for the
    /// position indicators.
    std::vector<std::string> standings;
    int playerStanding = -1;

    /// The activation prompt (HUD_ACTIVATION_PROMPT): the event name and the
    /// call to action; an empty name hides it. The title above them shows
    /// when set (0x828a0568 fires SHOW_TITLE or HIDE_TITLE by whether it is
    /// empty). The icon names one of the scene's SET_EVENTICON_* events by
    /// its suffix ("DEFAULT", "FESTIVALEVENT1", "RACECENTRAL", "BARNFIND"
    /// and so on); empty leaves the icon as it is.
    std::string prompt;
    std::string promptDetail;
    std::string promptTitle;
    std::string promptIcon;

    /// For the minimap: the car's position on the ground (world x, z in
    /// metres), the heading of its body's forward axis in radians (0 facing
    /// +z, increasing towards +x) and its speed in metres per second (the
    /// length of its velocity, which the sat-nav camera reads).
    glm::vec2 playerMapPosition{0.0F};
    float playerHeading = 0.0F;
    float speedMetresPerSecond = 0.0F;

    /// Seconds since the previous frame.
    float dt = 0.0F;
};

/// The in-game HUD, drawn from the game's own HUD scene
/// (media/ui/scenes/ui4/947_HUD) with its textures and vector fonts, and
/// driven the way the game's HUD controls drive it: by firing the events the
/// scene's logic handles and by setting the elements its contracts bind.
///
/// Construct it with a current OpenGL 4.5 context. draw() renders over
/// whatever is in the bound framebuffer (the window after tonemapping),
/// enabling blending itself and restoring depth testing afterwards.
class Hud {
public:
    /// `track` names the track folder under media/tracks ("colorado"),
    /// whose road network the minimap draws; without one the minimap stays
    /// empty. Throws fh::DataError if the HUD scene cannot be loaded and
    /// std::runtime_error if its shaders do not compile.
    Hud(const Vfs& vfs, const std::string& track, const std::string& language = "en");
    ~Hud();
    Hud(const Hud&) = delete;
    Hud& operator=(const Hud&) = delete;

    void draw(const HudState& state, int width, int height);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fh::ui
