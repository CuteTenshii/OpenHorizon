#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::audio {

/// The volume categories a mixer snapshot sets, in the executable's order:
/// the name table its static initialiser builds at 0x83284C70, which the
/// snapshot loader (0x82C72980) reads an element of media/audio/
/// audiomixersnapshots.xml for each of, and whose index picks the volume
/// setter the combined volume goes to (jump table at 0x8214B438, setters at
/// 0x82C68F50 onwards on the object at 0x8352EA5C). RallyCoDriver has no
/// element in any snapshot on the disc.
enum class MixCategory {
    FocusCar,
    OppCar,
    Transmission,
    SuperCharger,
    Turbo,
    EngineDamage,
    Backfire,
    Suspension,
    CarWind,
    Tire,
    Collision,
    AmbientStatic,
    AmbientDynamic,
    GameMusic,
    HomespaceAmbientDynamic,
    HomespaceMusic,
    MenuMusic,
    GameUI,
    VO,
    SatNav,
    DJModifier,
    RallyCoDriver,
};
inline constexpr std::size_t kMixCategoryCount = 22;
std::string_view mixCategoryName(MixCategory category);

/// A snapshot's group attribute, numbered as the table at 0x8214B180 that
/// 0x82C71570 matches the attribute against (a name not in it stores 11).
/// Each group holds one volume per category, and holds the last snapshot
/// of the group applied.
enum class MixGroup {
    Location,
    Speed,
    Gameplay,
    Impact,
    SpeedZones,
    VO,
    Reverb,
    Radio,
    SatNav,
    RallyCoDriver,
};
inline constexpr std::size_t kMixGroupCount = 10;

/// The snapshots by the ids the game applies them with: the name table at
/// 0x8214B1D0, which the loader reads in this order and CApplyMixerSnapshot
/// looks its "snapshot" attribute up in (0x82C6F088, 44 when not found).
enum class MixSnapshotId {
    FreeRoam,
    Race,
    UIExterior,
    UIInterior,
    Loading,
    PreEvent,
    Paused,
    PausedMultiplayer,
    Rewind,
    Video,
    FadeOut,
    FadeOutQuick,
    RaceEncounter,
    FadeOutWristbandReward,
    Barnfind,
    Crash,
    TopSpeed,
    LowSpeed,
    DefaultLocation,
    MainTown,
    Mountains,
    RedRock,
    Festival,
    AverageSpeedEnter,
    AverageSpeedExit,
    SpeedCam,
    VOPlaying,
    VONotPlaying,
    VOPlayingOnUIExterior,
    VOPlayingInstant,
    InTunnel,
    NotInTunnel,
    OpeningCutscene,
    OpeningCutsceneIntroCar,
    RadioDJSpeaking,
    RadioFestivalUpdate,
    RadioNormal,
    SatNavPlaying,
    SatNavNotPlaying,
    RallyCoDriverPlaying,
    RallyCoDriverNotPlaying,
    RaceRally,
    LoadingRally,
};
inline constexpr std::size_t kMixSnapshotCount = 43;
std::string_view mixSnapshotName(MixSnapshotId snapshot);
std::optional<MixSnapshotId> findMixSnapshot(std::string_view name);

/// One category's element in a snapshot: its attributes as the loader
/// stores them (24 bytes per category from snapshot +0).
struct MixSetting {
    /// Only active categories start a fade when the snapshot is applied.
    bool active = false;
    /// "destvolume": the group's volume for the category to fade to.
    float volume = 1.0F;
    /// Seconds from where the fade starts to the target, at the rate the
    /// fader works out (see SnapshotMixer).
    float fadeTime = 0.0F;
    /// Non-zero: after this many seconds the category fades back to 1 over
    /// returnFadeTime (Crash is the snapshot that uses it).
    float returnTime = 0.0F;
    /// Also the fade time of the next snapshot applied to the group while
    /// this one is its current snapshot, when that one has no return of its
    /// own (0x82C65948); Paused has 0.1 s, so leaving the pause fades back
    /// faster than FreeRoam's own 0.5 s.
    float returnFadeTime = 0.0F;
};

struct MixSnapshot {
    /// Whether the file has an element for it.
    bool present = false;
    /// Empty when the group attribute names none of the ten groups.
    std::optional<MixGroup> group;
    std::array<MixSetting, kMixCategoryCount> settings{};
};

/// media/audio/audiomixersnapshots.xml as the game's loader (0x82C72B68,
/// which loads Game:\Media\Audio\AudioMixerSnapshots.xml) reads it: under
/// the root MixSnapshots, one element per snapshot name, carrying a group
/// attribute and one element per category name with the attributes active,
/// destvolume, fadetime, returntime and returnfadetime. Verified: the disc's
/// file has 39 of the 43 snapshots (not the four rally ones), each naming
/// one of the groups and having 21 of the 22 categories (not
/// RallyCoDriver). A missing element is left at the
/// defaults above (an inactive category); what the game's XML reader leaves
/// in the fields of a missing element was not traced. The DynamicsData
/// element (the music high-pass rates, the crash and top speed thresholds)
/// is read by the same loader but is not kept here: CrashMix reads the
/// crash thresholds that apply Crash; TopSpeed and LowSpeed, which follow a
/// top speed whose source was not traced, have no triggers in this game.
class MixSnapshots {
public:
    /// Throws DataError naming `source` if the text is not XML with a
    /// MixSnapshots root.
    static MixSnapshots parse(std::string_view xml, const std::string& source);
    /// Throws DataError if the file is missing or malformed.
    static MixSnapshots load(const Vfs& vfs);

    const MixSnapshot& snapshot(MixSnapshotId id) const { return m_snapshots[static_cast<std::size_t>(id)]; }

private:
    std::array<MixSnapshot, kMixSnapshotCount> m_snapshots{};
};

/// The game's snapshot mixer (vtable 0x8214C7FC, the object at 0x8352EA60):
/// per group, a volume for each category; the category volumes that reach
/// the sounds are the products over the ten groups, clamped to 0..1
/// (0x82C73E88, end). Applying a snapshot starts, for each of its active
/// categories, a fade of its group's volume to the category's destvolume.
///
/// Verified in the executable:
/// - apply (0x82C6C840): when the snapshot is of the Gameplay group and the
///   group's current snapshot is none of Paused, PausedMultiplayer, Loading
///   and Rewind, that current snapshot is saved; the fades start
///   (0x82C65948); the snapshot becomes its group's current one. Ids past
///   the table do nothing. restore() (0x82C65930) applies the saved one,
///   which is how leaving the pause goes back to FreeRoam or Race.
/// - a fade (0x82C74850) keeps one fader per category and group. A new
///   fader starts from the group's volume with a step of |volume - target|
///   per fadeTime; a fader still running is restarted from the group's
///   volume with a step of |1 - target| (1 when that is 0). Either way the
///   fader returns to 1 after returnTime when that is not 0.
/// - update (0x82C73E88): each fader's timer starts on the first update
///   after the fade was set up and moves the volume only from the next one,
///   by the elapsed time over fadeTime times the step (or straight to the
///   target for a fadeTime of 0), stopping at the target; a fader with a
///   returnTime counts it from its first update and, once it has passed,
///   fades back to 1 over returnFadeTime. A fader is dropped when neither
///   of its timers runs.
/// The game's timers read the clock (0x82CC0898); here each update() is
/// given the seconds since the last one.
///
/// Not reproduced: apply's special cases for the VO, SatNav and wristband
/// snapshots (ids 13, 26 to 29 and 37, which re-route or ignore each other
/// by the VO state), which nothing in this game applies; the fader of the
/// pseudo-category 23, which is not a volume; the group volumes' initial
/// current snapshots, which the constructor does not set (taken as none).
class SnapshotMixer {
public:
    explicit SnapshotMixer(const MixSnapshots& snapshots);

    void apply(MixSnapshotId id);
    /// Applies the Gameplay snapshot saved by the last apply(), if any.
    void restore();
    /// Advances the fades by `seconds`.
    void update(float seconds);

    /// The category's volume as the sounds get it.
    float volume(MixCategory category) const;
    float groupVolume(MixGroup group, MixCategory category) const;
    std::optional<MixSnapshotId> current(MixGroup group) const;
    std::optional<MixSnapshotId> saved() const { return m_saved; }
    /// Faders running, for tests.
    std::size_t faderCount() const { return m_faders.size(); }

private:
    struct Fader {
        MixGroup group = MixGroup::Gameplay;
        MixCategory category = MixCategory::FocusCar;
        float current = 1.0F;
        /// Where a return fades back to.
        float from = 1.0F;
        float target = 1.0F;
        /// The volume covered per fadeTime.
        float step = 0.0F;
        float fadeTime = 0.0F;
        float returnTime = 0.0F;
        float returnFadeTime = 0.0F;
        bool fadeTimer = false;
        bool returnTimer = false;
        float returnElapsed = 0.0F;
    };

    float& group(MixGroup group, MixCategory category);
    void startFade(MixCategory category, MixGroup group, const MixSetting& setting, float fadeTime);

    MixSnapshots m_snapshots;
    std::array<std::array<float, kMixCategoryCount>, kMixGroupCount> m_groups{};
    std::array<std::optional<MixSnapshotId>, kMixGroupCount> m_current{};
    std::optional<MixSnapshotId> m_saved;
    std::vector<Fader> m_faders;
};

} // namespace openhorizon::audio
