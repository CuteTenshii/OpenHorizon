#pragma once

#include "game/ScreenFade.h"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class Vfs;

/// One key of a cutscene camera's <Anim>: where the camera is and where it
/// looks at `time` seconds into the camera's cut, in the space its
/// animation names. Angles are in degrees as the files store them.
struct CutsceneKey {
    float time = 0.0F;
    /// The car part a PartSpace position is measured from ("headlightL",
    /// "steering", "wheelRF", ...); empty in the other spaces.
    std::string carPart;
    glm::vec3 position{0.0F};
    float yaw = 0.0F;
    float pitch = 0.0F;
    float roll = 0.0F;
    float fov = 0.0F;
};

/// The space a camera's positions or angles are given in (the <Anim>'s
/// PosAnimType and RotAnimType).
enum class CutsceneSpace { Car, Part, World };

/// One <Cam> of a cutscene: a cut from `start` for `duration` seconds.
struct CutsceneCamera {
    /// "Animateable" (keyed), "Follow" or "Wheel".
    std::string type;
    float start = 0.0F;
    float duration = 0.0F;
    /// The node the positions are relative to ("start_location_00" of the
    /// route file, or a node a CCutsceneCameraNodeTrigger places); empty
    /// for the car.
    std::string target;
    CutsceneSpace positionSpace = CutsceneSpace::Car;
    CutsceneSpace rotationSpace = CutsceneSpace::Car;
    /// The <Anim>'s EaseIn and EaseOut, clamped to [0, 1] as the parser
    /// does (0x8290ff80).
    float easeIn = 0.0F;
    float easeOut = 0.0F;
    std::vector<CutsceneKey> keys;
    /// Fades the cut starts with and ends with; set only when their
    /// Duration is above zero.
    std::optional<FadeOptions> fadeIn;
    std::optional<FadeOptions> fadeOut;
};

/// One <Event> of a cutscene: a trigger fired `time` seconds in.
struct CutsceneEvent {
    float time = 0.0F;
    /// The trigger's class, the EventTrigger's id ("CCutsceneCountdownTrigger",
    /// "CCutsceneAudioTrigger", ...).
    std::string trigger;
    /// Its name attribute ("HideAllCars", "CameraTargetNode_0", ...).
    std::string name;
    /// Every other attribute, as written.
    std::map<std::string, std::string, std::less<>> attributes;
};

/// A <Cutscene> of the game's cutscene files: its cameras, cut by cut, and
/// its events.
///
/// Verified from the files (all 112 of media/gamemodes/colorado/cutscenes):
/// the layout below. What the executable does with the fields is recorded
/// where they are used.
struct Cutscene {
    std::string name;
    /// PlaybackMode "Looping": after `duration` the cutscene carries on from
    /// `loopFrom`.
    bool looping = false;
    float duration = 0.0F;
    float loopFrom = 0.0F;
    std::vector<CutsceneCamera> cameras;
    std::vector<CutsceneEvent> events;

    /// The time of the first event of this trigger class, if it has one.
    std::optional<float> firstEvent(std::string_view trigger) const;
    /// Where the cutscene is after playing for `elapsed` seconds: it holds at
    /// its end, or loops back to loopFrom when it is a looping one.
    float timeAt(float elapsed) const;
    /// The camera cut that `time` lies in, or nullptr past the last one.
    const CutsceneCamera* cameraAt(float time) const;
};

/// The cutscenes a session loads, by name.
///
/// Verified from the loader (0x82991a80): of media/gamemodes/<track>/
/// cutscenes it reads cutscenes_default_both.xml, then
/// cutscenes_default_freeroam.xml in a free-roam session (modes 17 to 19)
/// or cutscenes_default_race.xml in any other, then tracks/cutscenes_
/// <DevName>.xml for the session's track (SELECT DevName FROM Tracks,
/// 0x82704fd8), then events/cutscenes_<event>.xml, which the disc does not
/// have. The other files of the folder (autoshow_cams.xml, the prhubs, the
/// tracks no Tracks row names) belong to other flows and are not read here.
class CutsceneLibrary {
public:
    /// Reads the files of a session on `track`: a race session on the
    /// track `trackDevName` (Tracks.DevName), or free roam when
    /// `raceSession` is false. A file that is missing or cannot be read is
    /// skipped; the result may be empty.
    static CutsceneLibrary load(
        const Vfs& vfs, std::string_view track, bool raceSession, std::string_view trackDevName);

    /// The cutscene of this name (case-insensitive), or nullptr.
    const Cutscene* find(std::string_view name) const;
    /// A race-specific cutscene as CPrepareCutscene with race_specific set
    /// looks it up (0x829e5d38 through 0x8298e3f8 and 0x8298cbd0), the first
    /// that exists of: `base`_X_E, `base`_X, `base`_E, `base`_event<N> and
    /// `base`, where X is `trackDevName`, E is `horizonEventId` and N is
    /// `eventId`. Verified: the order and N (the Events.Id the career race's
    /// load resolves, +904 of the session state). Inferred: that X is the
    /// track's DevName (every race-specific name on the disc ends in one;
    /// the string read is +108 of the object at +124 of the game options)
    /// and that E is the HorizonEventID (event_activate_intro_FESTIVAL_SECOND
    /// names one).
    const Cutscene* raceSpecific(
        std::string_view base, std::string_view trackDevName, std::string_view horizonEventId, int eventId) const;
    std::size_t size() const { return m_cutscenes.size(); }

private:
    std::map<std::string, Cutscene, std::less<>> m_cutscenes;
};

/// The car parts a PartSpace camera is placed from, numbered as the
/// cutscene parser maps the CarPart names (0x8290ff80) and as
/// CarPartPositions numbers its PartId column: headlightL 0, headlightR 1,
/// taillightL 2, taillightR 3, steering 4, wheelLF 5, wheelRF 6, wheelRR 7,
/// wheelLR 8, handsAnchor 9.
inline constexpr std::size_t kCutsceneCarParts = 10;
/// The part id of a CarPart name, or -1.
int cutsceneCarPart(std::string_view name);

/// What cutscene cameras are placed against.
struct CutsceneScene {
    /// The player's car: car space (x right, y up, z forward) to world.
    glm::mat4 car{1.0F};
    /// The player's car's CarPartPositions in car space, by part id.
    /// Verified: the car's physical data loads them with SELECT * FROM
    /// CarPartPositions WHERE Ordinal=%d into a table of ten (0x82531200,
    /// PartId checked below 10). Inferred: that they are car-space points
    /// with this engine's axes (the wheels sit at plus and minus half the
    /// wheelbase along z, the left parts at negative x).
    std::array<std::optional<glm::vec3>, kCutsceneCarParts> parts;
    /// Named transforms of the race's route file (start_location_00, ...),
    /// node space (z along the transform's facing) to world.
    std::map<std::string, glm::mat4, std::less<>> nodes;
    /// The ground's height below a point (x, z) searched down from a height,
    /// for camera nodes placed with snapToGround; nodes stay where the event
    /// puts them without it.
    std::function<std::optional<float>(float x, float z, float fromY)> ground;
};

/// Where a cutscene camera is and looks at one moment.
struct CutsceneView {
    glm::vec3 position{0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    /// Radians; taken as the vertical field of view (the files' 17 to 28
    /// degrees for close shots read as vertical; which one the camera sets
    /// was not traced).
    float fov = 0.5F;
};

/// Plays one cutscene: its clock, its events, its cameras and their fades.
///
/// Verified (CPlayCameraSequence and the cutscene player, 0x8298c520 on):
/// the clock advances by |dt|; a Normal cutscene ends at its Duration, a
/// Looping one jumps back to LoopFrom; the cut in use is the last whose
/// StartCut is not after the clock (inferred from the cuts' contiguous
/// StartCut and duration); CCutsceneCountdownTrigger sets the flag that
/// onCountdown waits for; a cut's fade in is opaque until the shorter of
/// its Duration and HoldDuration and clear from the longer, and its fade
/// out ramps in over the last Duration before the cut ends (0x829035b0),
/// both shaped by the fade's curve; outside those windows a playing
/// cutscene's overlay is clear, which is what lifts a black-out.
///
/// Camera evaluation (0x8290a8b0, 0x82909990, 0x82901738, 0x82903d40):
/// the cut's clock is clamped to its keys and eased by EaseIn and EaseOut;
/// each value is a cubic Hermite between the keys around it, with the
/// tangents weighted by the neighbouring slopes and the plain difference at
/// the first and last key (verified for FOV; the position and angle code is
/// VMX128 the disassembler cannot read, so the same curve per component is
/// inferred); angles are unwrapped key to key to steps within half a turn.
/// CarSpace positions are the target's matrix times the key (the target is
/// the cut's TargetName node, else the player's car); PartSpace keys are
/// offsets from the car part the key names, turned into car space when the
/// cut starts; WorldSpace positions are used as they are. The rotation
/// starts from the target's axes (CarSpace) or the world's (WorldSpace)
/// and turns by yaw about y, then pitch about x, then roll about z, each as
/// glm turns it in these axes. Inferred from the data: a positive yaw
/// turns right (the finish-line camera of FESTIVAL_PLNS_006 faces back at
/// the oncoming cars 12 degrees right of its node) and a positive pitch
/// looks down (that camera, 1.1 m above a road that falls away 3 degrees
/// towards the line, pitches +5); the roll's sense was not checked.
///
/// Not done: the cuts of the other types (Follow is the game camera, the
/// caller's own; Wheel and the rest are not played), the PostEffectsAnim
/// keys (depth of field, exposure flashes, vignettes), mirroring for
/// right-hand drive, and the audio, music, voice and car triggers, which
/// takeEvents() hands to the caller.
class CutscenePlayer {
public:
    /// Starts `cutscene` from its beginning. The cutscene must outlive the
    /// playback.
    void play(const Cutscene& cutscene);
    void stop();
    bool playing() const { return m_cutscene != nullptr; }
    const Cutscene* cutscene() const { return m_cutscene; }
    /// Advances the clock by `dt` seconds and fires the events passed.
    void advance(float dt);
    /// The cutscene's clock, looped back as it plays.
    float time() const { return m_time; }
    /// A CCutsceneCountdownTrigger has fired.
    bool countdownReached() const { return m_countdown; }
    /// A CCutsceneStartCountdownTrigger has fired.
    bool startCountdownReached() const { return m_startCountdown; }
    /// A Normal cutscene has reached its end.
    bool ended() const { return m_ended; }
    /// The events fired since the last call, in order.
    std::vector<CutsceneEvent> takeEvents();

    /// The cut in use, or nullptr.
    const CutsceneCamera* camera() const;
    /// Where the camera of the cut in use is, or nothing when the cut is not
    /// a keyed one, has no keys, or needs a node or car part `scene` lacks.
    std::optional<CutsceneView> view(const CutsceneScene& scene) const;
    /// How opaque the cut's fade is now, and its colour.
    float overlay(glm::vec3& colour) const;

private:
    const Cutscene* m_cutscene = nullptr;
    float m_elapsed = 0.0F;
    float m_time = 0.0F;
    bool m_countdown = false;
    bool m_startCountdown = false;
    bool m_ended = false;
    bool m_started = false;
    std::vector<CutsceneEvent> m_fired;
    /// A node a CCutsceneCameraNodeTrigger placed: its position, its heading
    /// in degrees (the event's rotation, taken as a heading about up as
    /// CPlaceCar's is) and whether it sits on the ground below.
    struct PlacedNode {
        glm::vec3 position{0.0F};
        float headingDegrees = 0.0F;
        bool snapToGround = false;
    };
    std::map<std::string, PlacedNode, std::less<>> m_nodes;
    /// The target's transform: a placed node, a route node or the car.
    std::optional<glm::mat4> targetTransform(const CutsceneCamera& camera, const CutsceneScene& scene) const;
};

} // namespace openhorizon
