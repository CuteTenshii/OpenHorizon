#pragma once

#include "RadioData.h"
#include "RadioSequencer.h"

#include "audio/Mixer.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::audio {
class AudioDevice;
}

namespace fh::radio {

class RadioAssets;

/// A station as the game's menus and HUD know it.
struct RadioStationInfo {
    /// The name in RadioSystem.xml ("Radio1").
    std::string name;
    /// The localised display name ("HORIZON BASS ARENA"); empty for the
    /// positional stations, which have none.
    std::string displayName;
    /// True for the stations the player tunes through (see
    /// RadioSystemData).
    bool tunable = false;
    bool off = false;
};

/// What the player hears now, for the HUD.
struct RadioNowPlaying {
    std::size_t station = 0;
    bool off = false;
    std::string stationName;
    /// Empty when no track plays (and on the off station).
    std::string title;
    std::string artist;
    bool djSpeaking = false;
};

/// The festival radio, playing through the game's mixer.
///
/// RadioSequencer decides what plays; this class streams the samples from
/// the banks (decoding MPEG progressively, never whole tracks), mixes the
/// music, ident and DJ streams at the sequencer's gains and hands the result
/// to the mixer as one stream voice.
///
/// With `Options::worker` (for a live device) a worker thread runs the
/// sequencer and decoders about kLeadFrames ahead of the mixer, so the
/// audio thread only copies finished audio; commands therefore take effect
/// that much later. Without it (for offline devices, tests and tools) the
/// radio produces its audio inside the mixer's render call, deterministic
/// for a given seed.
///
/// The sequencer's per-frame update runs on the audio timeline every
/// kUpdateFrames (5.3 ms); the game runs it once per game frame. Events are
/// released by update() when the audio they belong to has been mixed.
///
/// Control methods and update() are for one thread (the game's); the audio
/// callbacks are internal.
class Radio {
public:
    static constexpr std::size_t kUpdateFrames = 256;
    static constexpr std::size_t kLeadFrames = 4800;

    struct Options {
        /// Voice bank language code; English when the disc has no bank for
        /// it.
        std::string voiceLanguage = "EN";
        /// String table folder for station names.
        std::string textLanguage = "en";
        /// Seed of the game's random generator (see RadioRandom).
        std::uint32_t seed = 0;
        bool worker = true;
        audio::BusId bus = audio::kMasterBus;
    };

    /// Loads the radio's data and starts its stream voice, silent until
    /// startFreeRoam(). Throws DataError if the radio's files are missing or
    /// malformed.
    Radio(const Vfs& vfs, audio::AudioDevice& device, Options options);
    ~Radio();
    Radio(const Radio&) = delete;
    Radio& operator=(const Radio&) = delete;

    const std::vector<RadioStationInfo>& stations() const { return m_stations; }
    /// The stations the player tunes through are 0..offStation().
    std::size_t offStation() const { return m_offStation; }
    const RadioAssets& assets() const { return *m_assets; }
    /// "RADIO OFF", localised.
    const std::string& radioOffText() const { return m_radioOff; }

    void startFreeRoam();
    /// See RadioSequencer::startForEvent().
    void startForEvent(std::string_view trackName, float secondsToEventStart);
    void stop(float delaySeconds = 0.0F);
    void selectStation(std::size_t index, bool fade = false);
    void setPlayerStation(std::size_t index);
    void nextStation();
    void previousStation();
    /// Turns the radio off: selects the off station.
    void turnOff();
    void setTimeOfDay(TimeOfDay timeOfDay);
    /// The player's radio volume, 0..1; 0 pauses the radio.
    void setVolume(float volume);
    void setPaused(bool paused);
    void setStationIdentsEnabled(bool enabled);
    void setDjDialogueEnabled(bool enabled);
    void setStationSwitchingLocked(bool locked);
    void setDialogueGroupEnabled(int group, bool enabled);

    /// Releases the events whose audio has been mixed, updates nowPlaying()
    /// and counts down the HUD's new-track delay. Call once per game frame.
    void update(float dt);

    const RadioNowPlaying& nowPlaying() const { return m_nowPlaying; }
    /// The track info the HUD should show now: set when the game's HUD delay
    /// after a HudShow event has run out, cleared by the next call.
    std::optional<RadioNowPlaying> takeHudRequest();
    /// The events released by update() since the last call.
    std::vector<RadioEvent> takeEvents();

private:
    struct Impl;
    class Output;

    void post(std::function<void(RadioSequencer&)> command);
    void apply(const RadioEvent& event);

    std::shared_ptr<const RadioAssets> m_assets;
    std::vector<RadioStationInfo> m_stations;
    std::size_t m_offStation = 0;
    std::string m_radioOff;
    std::shared_ptr<Impl> m_impl;
    audio::Mixer& m_mixer;
    audio::VoiceId m_voice = audio::kNoVoice;

    RadioNowPlaying m_nowPlaying;
    std::vector<RadioEvent> m_released;
    std::optional<RadioNowPlaying> m_hudPending;
    float m_hudTimer = 0.0F;
    std::optional<RadioNowPlaying> m_hudRequest;
};

} // namespace fh::radio
