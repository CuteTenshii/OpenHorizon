#pragma once

#include "RadioData.h"
#include "RadioRandom.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::radio {

/// The two sound banks the radio plays from: Radio_Music.fsb and
/// Radio_VO_<language>.fsb.
enum class RadioBank { Music, Voice };

/// What the sequencer needs to know of one sample: its rate, its length and
/// its sync points, all in the sample's own frames.
struct SampleTiming {
    int sampleRate = 0;
    std::uint32_t frames = 0;
    std::vector<std::uint32_t> syncPoints;
};

/// Everything the radio runs on: RadioSystem.xml, the two
/// RadioSoundbankInfo files and the timing of every sample in both banks
/// (indexed by the bank's sample index).
struct RadioCatalog {
    RadioSystemData system;
    SoundBankInfoMap musicInfo;
    SoundBankInfoMap voiceInfo;
    std::vector<SampleTiming> musicSamples;
    std::vector<SampleTiming> voiceSamples;
};

/// The radio's three voices, as the game has them: the music stream, the
/// ident stream and the DJ's dialogue stream (radio system +52, +84 and +68
/// in the executable).
enum class RadioChannel { Music = 0, Ident = 1, Dialogue = 2 };
inline constexpr std::size_t kRadioChannelCount = 3;

/// Something the radio did, stamped with the output frame it happened at.
struct RadioEvent {
    enum class Kind {
        /// A music track started; `name` is its fmodName, `title` and `artist`
        /// its display text, `startMs` the offset it started at.
        TrackStarted,
        TrackEnded,
        /// A station ident started; `name` is its fmodName.
        IdentStarted,
        /// A DJ line started; `name` is its fmodName.
        DialogueStarted,
        DialogueEnded,
        /// The current station changed to `station`.
        StationChanged,
        /// The game would now tell the HUD to show the station and, when one
        /// is playing, the track (`title`, `artist`) after `delaySeconds`.
        HudShow,
        Paused,
        Resumed,
        /// The radio faded out and stopped all its streams.
        Stopped,
    };
    Kind kind = Kind::TrackStarted;
    std::uint64_t frame = 0;
    std::size_t station = 0;
    std::string name;
    std::string title;
    std::string artist;
    float startMs = 0.0F;
    float delaySeconds = 0.0F;
};

/// One voice the sequencer is playing: which sample, from where, and how far
/// it has got.
struct RadioVoice {
    bool active = false;
    RadioBank bank = RadioBank::Music;
    int sampleIndex = -1;
    int sampleRate = 0;
    /// In the sample's own frames.
    std::uint32_t startFrame = 0;
    std::uint32_t length = 0;
    /// Output frames the voice has played. Its position in the sample is
    /// startFrame + elapsed * sampleRate / kOutputRate; keeping the count in
    /// output frames keeps event times exact at any sample rate.
    std::uint64_t elapsed = 0;
    /// Changes whenever the voice starts a new sound, so a player can tell a
    /// restarted voice from a continuing one.
    std::uint64_t instance = 0;
    std::string fmodName;
};

/// The radio's state machine, a reimplementation of the game's radio system
/// (the class whose vtable is at 0x8214ec14; its update is 0x82c9a088, the
/// music, ident and dialogue stream callbacks 0x82c97220, 0x82c99cc0 and
/// 0x82c981d8). It plays no audio: it decides what plays when and at what
/// gain, and a player (Radio) or a tool follows it.
///
/// Time moves two ways, as in the game. advance() moves the voices through
/// their samples and fires what the game's stream callbacks do at sync
/// points and at the ends of sounds; callers must not advance past
/// framesUntilEvent(), so events land on their exact frame. update() does
/// what the game's per-frame update does: the level, duck, scale and
/// station-change fades, the stop timer and the HUD. The output runs at
/// kOutputRate; samples at other rates move proportionally.
///
/// The free-roam flow, all verified in the executable:
/// - A track plays from the music bank. When it reaches its IdentStart sync
///   point, a station ident starts over its end, and, once
///   maxTracksBetweenDJ tracks have started since the last DJ line, a
///   regular DJ line is chosen.
/// - When the ident reaches its StartNextTrack sync point the old track is
///   cut if still playing and the next track starts at its SongStart. With
///   a DJ line pending, the line starts at the same moment and the track
///   starts early enough that the line ends by the track's EventStart
///   (never before the track's start).
/// - Tracks are picked at random, weighted by the time-of-day likelihood,
///   among those not played in the last (track count - 1) picks; idents
///   and DJ lines at random among those not picked in the last noRepeat
///   picks.
/// - Each station remembers its track, the offset it started at and when,
///   and on returning to a station the track carries on where it would be
///   had it kept playing, if that is before 90% of the way from SongStart
///   to IdentStart; otherwise a new track starts at its SongStart. A
///   station never heard before starts a random track at a random point
///   between 10% and 90% of that span.
/// - The off station is a station of silent tracks; choosing it fades the
///   radio out over scaleFadeDownTime and pauses every stream, which also
///   stops the clock the stations' positions are measured by.
/// - Music ducks to duckValue while the DJ's stream is audible (mean square
///   of 256 samples above 0.001).
///
/// The game also plays special, immediate and festival-update DJ lines on
/// gameplay events, starts positional stations in menus, and can force a
/// playlist; those are not implemented. The data for them is parsed.
class RadioSequencer {
public:
    static constexpr int kOutputRate = 48000;
    static constexpr std::size_t kNoEvent = std::numeric_limits<std::size_t>::max();

    /// Links every track, ident and DJ line to its sample through the
    /// catalog's sound bank info; a sound missing there is kept but never
    /// plays (the game would fail to create its stream).
    RadioSequencer(std::shared_ptr<const RadioCatalog> catalog, std::uint32_t seed);

    const RadioCatalog& catalog() const { return *m_catalog; }

    /// StartRadioFlowForFreeRoam: plays the current station. The game's flow
    /// start takes a flag that decides between a fade-in and an immediate
    /// HUD; the value the free-roam start passes was not traced, so this
    /// passes what a station change without a fade does (immediate).
    void startFreeRoam();
    /// StartRadioFlowForEvent: plays `trackName` (a music fmodName, or
    /// "Random" for a pick from the player's chosen station) so that its
    /// EventStart comes `secondsToEventStart` seconds after the start, with
    /// the race's level fade and HUD delay. An unknown name plays
    /// R1_Everyday, as the game does. Nothing plays when the player's
    /// station is the off station.
    void startForEvent(std::string_view trackName, float secondsToEventStart);
    /// StopRadio: after `delaySeconds`, fades the radio out over
    /// levelFadeDownTime and then stops its streams.
    void stop(float delaySeconds = 0.0F);

    /// Selects a station by index (the game's select-by-index, vtable +120).
    /// `fade` asks for the station-change fade; the choice is kept and also
    /// applies to later nextStation()/previousStation() calls, as the game
    /// keeps it. Selecting the current station does nothing. Before
    /// startFreeRoam() it only chooses the station the radio starts on.
    void selectStation(std::size_t index, bool fade);
    /// Records the player's station choice (vtable +56), which a race's
    /// "Random" track is picked from and which decides whether races have
    /// music at all. The game's settings code presumably sets it from the
    /// "radio station" option; that call was not traced.
    void setPlayerStation(std::size_t index);
    /// The D-pad: the next or previous station in name order, wrapping from
    /// the off station round to the first (vtable +116).
    void nextStation();
    void previousStation();

    void setTimeOfDay(TimeOfDay timeOfDay);
    /// The radio volume from the player's settings, 0..1. At 0 the radio
    /// pauses, as the game's update does.
    void setUserVolume(float volume);
    /// Pauses every stream (vtable +208), as the game does when it pauses.
    void setPaused(bool paused);
    /// When off, idents come from the off station, which are silent (radio
    /// system +128). Which game option drives this flag was not traced.
    void setStationIdentsEnabled(bool enabled);
    /// When off, no regular DJ line is queued (radio system +116). Which game
    /// option drives this flag was not traced; the game's own code only
    /// ever clears it.
    void setDjDialogueEnabled(bool enabled);
    /// When locked, the D-pad does not change stations (radio system +124).
    void setStationSwitchingLocked(bool locked);
    /// Switches the regular DJ lines of `group` on or off on every tuneable
    /// station (vtable +132), as the game does for hub hints.
    void setDialogueGroupEnabled(int group, bool enabled);

    /// Frames until the next sync point or end of a sound, kNoEvent when
    /// nothing plays or the radio is paused.
    std::size_t framesUntilEvent() const;
    /// Moves time on by `frames` (at most framesUntilEvent()) and fires the
    /// events that fall at the new time.
    void advance(std::size_t frames);
    /// The game's per-frame update. `dialogueMeanSquare` is the mean square
    /// of the last 256 samples of the DJ's stream, which decides the
    /// ducking; pass 0 when nothing is decoded (tools).
    void update(float dt, float dialogueMeanSquare);

    std::uint64_t frame() const { return m_frame; }
    const RadioVoice& voice(RadioChannel channel) const
    {
        return m_voices[static_cast<std::size_t>(channel)];
    }
    bool paused() const { return m_paused; }
    bool started() const { return m_flowStarted; }

    std::size_t stationCount() const { return m_stations.size(); }
    std::size_t currentStation() const { return m_current; }
    std::size_t playerStation() const { return m_playerStation; }
    std::size_t offStation() const { return m_offStation; }
    /// The stations the player tunes through: 0 to offStation().
    std::size_t tunableStationCount() const { return m_offStation + 1; }
    /// The track playing, if any.
    const MusicTrack* currentTrack() const;

    /// Gains of the three streams, as the game's update sets them.
    float musicGain() const { return m_musicGain; }
    float dialogueGain() const { return m_dialogueGain; }
    float identGain() const { return m_identGain; }

    /// The events since the last call, oldest first.
    std::vector<RadioEvent> takeEvents();

private:
    struct LinkedTrack {
        const MusicTrack* track = nullptr;
        int sampleIndex = -1;
        float eventStart = 0.0F;
        float songStart = 0.0F;
        float identStart = 0.0F;
        int syncIdentStart = -1;
        int counter = 0;
    };

    struct Playlist {
        std::vector<LinkedTrack> tracks;
        /// Indices into `tracks` in the order of the game's map (by
        /// fmodName), one per distinct name.
        std::vector<std::size_t> order;
        std::array<float, 4> totals{};
        int noRepeat = 0;
    };

    struct LinkedLine {
        const VoiceLine* line = nullptr;
        int sampleIndex = -1;
        /// Dialogue: its length in milliseconds. Idents: unused.
        float lengthMs = 0.0F;
        /// Idents: the StartNextTrack sync point.
        int syncStartNextTrack = -1;
        int counter = 0;
    };

    struct LineList {
        std::vector<LinkedLine> lines;
        int noRepeat = 2;
        int blocked = 0;
    };

    struct StationState {
        const Station* station = nullptr;
        Playlist playlist;
        LineList idents;
        LineList regular;
        int lastStarted = -1;
        int selected = -1;
        float resumeOffsetSeconds = 0.0F;
        float resumeClock = 0.0F;
    };

    enum class StartMode { Beginning = 0, SongStart = 1, BeforeEventStart = 2, UnderDialogue = 3, Random = 4,
        Offset = 5 };

    void link();
    int pickTrack(std::size_t station);
    void trackStarted(std::size_t station, int track);
    LinkedLine* pickIdent(LineList& list);
    LinkedLine* pickDialogue(LineList& list);
    void playTrack(int track, StartMode mode, float seconds);
    void playIdent();
    void playDialogue();
    void startVoice(RadioChannel channel, RadioBank bank, int sampleIndex, std::uint32_t startFrame,
        const std::string& fmodName);
    void stopVoice(RadioChannel channel);
    void stopAllVoices();
    void stopNow();
    std::uint64_t outputFramesTo(const RadioVoice& voice, std::uint32_t frame) const;
    std::uint32_t positionOf(const RadioVoice& voice) const;
    void startFlow(std::string_view trackName, float seconds, bool immediate);
    void requestStation(std::size_t index, bool restart);
    void switchStation(std::size_t index, bool restart);
    void setScaleTarget(float target);
    void pause(bool paused);
    void onMusicSync(int index);
    void onMusicEnd();
    void onIdentSync(int index);
    void onIdentEnd();
    void onDialogueEnd();
    void emit(RadioEvent::Kind kind, std::string name = {}, float startMs = 0.0F);
    const SampleTiming* timing(RadioBank bank, int sampleIndex) const;
    bool isNormalStation() const { return m_current != m_offStation && !m_is3d; }
    float fadeRate(float seconds) const;

    std::shared_ptr<const RadioCatalog> m_catalog;
    RadioRandom m_random;
    std::vector<StationState> m_stations;
    std::size_t m_offStation = 0;
    std::size_t m_first3d = 0;

    std::size_t m_current = 0;
    std::size_t m_playerStation = 0;
    std::size_t m_lastTunable = 0;
    bool m_is3d = false;
    int m_timeOfDay = 0;

    std::array<RadioVoice, kRadioChannelCount> m_voices{};
    /// The next sync point of each voice's sample still to be passed.
    std::array<std::size_t, kRadioChannelCount> m_nextSync{};
    std::uint64_t m_nextInstance = 1;
    std::uint64_t m_frame = 0;
    /// The clock stations' positions are kept against (radio system +720):
    /// seconds of unpaused updates.
    float m_clock = 0.0F;

    int m_currentTrack = -1;
    LinkedLine* m_currentIdent = nullptr;
    LinkedLine* m_pendingDialogue = nullptr;
    float m_dialogueLengthMs = 0.0F;
    bool m_dialoguePending = false;
    int m_tracksSinceDialogue = 0;
    bool m_hadMusic = false;

    bool m_flowStarted = false;
    bool m_paused = false;
    bool m_identsEnabled = true;
    bool m_dialogueDisabled = false;
    bool m_switchingLocked = false;
    bool m_hudShown = false;
    float m_hudDelay = 0.0F;

    /// The volume setting as last given, and as the last update() applied it
    /// (radio system +328); the pause on a volume of zero looks at both.
    float m_userVolume = 1.0F;
    float m_appliedVolume = 1.0F;
    float m_level = 0.0F;
    float m_levelTarget = 0.0F;
    /// Radio system +396: 1 until a flow sets the level fade-in.
    float m_levelUpRate = 1.0F;
    float m_duck = 1.0F;
    float m_scale = 1.0F;
    float m_scaleTarget = 1.0F;
    float m_stopTimer = 0.0F;

    /// The station-change fade (radio system +352, +556, +564).
    float m_stationFade = 1.0F;
    int m_fadeState = 0;
    bool m_fadeOnChange = false;
    std::size_t m_pendingStation = 0;
    bool m_pendingRestart = false;

    float m_musicGain = 0.0F;
    float m_dialogueGain = 0.0F;
    float m_identGain = 0.0F;

    std::vector<RadioEvent> m_events;
};

} // namespace openhorizon::radio
