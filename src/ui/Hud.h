#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::ui {

class MapCoverage;

/// A request to show the radio panel (the HUD_RADIO control), as the game's
/// radio status (CHudStatusRadio) carries it.
struct HudRadioRequest {
    /// The station's index in the radio's tunable order, the off station
    /// last: the panel shows the logo of slide `station + 1` of the scene's
    /// RSLogos component (BASSARENA, PULSE, 2ELEVEN, OFF).
    int station = 0;
    /// Empty hides the line (SHOW_/HIDE_TRACK_NAME, SHOW_/HIDE_ARTIST_NAME).
    std::string title;
    std::string artist;
};

/// Something the minimap marks, matched against the map profile's group
/// filters (media/ui/MapProfileMinimap.xml). The track's fixed activities
/// (festival buildings, street race hubs) are loaded by the HUD itself.
struct HudMapMarker {
    /// The tag and value, such as {"activity_type", "race"} for a race start
    /// (see careerEventActivityType in Minimap.h) or {"vehicle_type", "ai"}
    /// for an opponent in a race.
    std::string key;
    std::string value;
    /// World x and z.
    glm::vec2 position{0.0F};
    /// Radians, 0 facing +z, increasing towards +x.
    float heading = 0.0F;
};

/// A line for the minimap, matched against the map profile's group filters.
struct HudMapLine {
    /// {"type", "route"} for the sat-nav route, {"type", "group_route"} for
    /// a group's route, {"type", "track"} for a race's course.
    std::string key;
    std::string value;
    /// World x and z, in order.
    std::vector<glm::vec2> points;
};

/// A reading of a speed trap or speed zone for the HUD_SPEED_CAMERA
/// control, as the game's CHudStatusSpeedCamera carries it (0x82979af0):
/// the kind (+24: 0 speed trap, 1 speed zone), the speed (+28) and the
/// profile's best before it (+32), both in metres per second.
struct HudSpeedCamera {
    /// Counts readings: a new value starts the display.
    std::size_t serial = 0;
    bool zone = false;
    float speed = 0.0F;
    float previousBest = 0.0F;
};

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

    /// The assist lights (0x8289b278 fires ABS_ON/OFF, STM_ON/OFF and
    /// TCS_ON/OFF when these change; they start off). The car answers them
    /// from its state (vtable +836, +844, +840: 0x82ddf7e0, 0x82ddf830,
    /// 0x82ddf808), so they light while the assist acts, not while it is
    /// switched on: ABS when two of the car's fields (+3020 and +3436) are
    /// set, which reads as enabled and working (inferred), STM when +3016 is
    /// set, TCS while a value at +3004 exceeds 0.1, which reads as how hard
    /// traction control is cutting in (inferred).
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
    /// Race starts, checkpoints ({"checkpoint_type", "checkpoint"}) and
    /// other cars for the minimap.
    std::vector<HudMapMarker> mapMarkers;
    /// Route and course lines for the minimap.
    std::vector<HudMapLine> mapLines;
    /// Metres to the sat-nav destination; empty (or 0) hides the readout
    /// below the minimap (HUD_SAT_NAV, 0x828b0768).
    std::optional<float> destinationDistance;
    /// The roads the player has driven, which colour the minimap's road
    /// lines; null draws every road undriven.
    const MapCoverage* coverage = nullptr;

    /// Set on the frame a radio panel request arrives (for the radio module,
    /// on the frame Radio::takeHudRequest() returns one), empty otherwise.
    std::optional<HudRadioRequest> radio;
    /// Shows the "tuning" note (HUD_RADIO_TUNING) while true. The game
    /// raises it through a status of its own (CHudStatusRadioTuning) whose
    /// trigger was not found; leave it false unless the radio says so.
    bool radioTuning = false;

    /// The latest speed trap or speed zone reading, if any; the HUD shows
    /// one when its serial changes.
    std::optional<HudSpeedCamera> speedCamera;

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
