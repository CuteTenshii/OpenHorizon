#pragma once

#include "audio/Mixer.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
}
namespace fh::audio {
class AudioLibrary;
class EventInstance;
class FevProject;
} // namespace fh::audio

namespace fh::ui {

/// One sound of media/audio/ui/ui4audio.xml: the name the game plays it by
/// and the FMOD event it plays.
struct UiSound {
    std::string name;
    /// The event's path in the UIInGame project: the entry's group without
    /// the project name, then its cue ("UI/Nav/HUD_Pause").
    std::string event;
    bool looping = false;
};

/// The defaults of media/audio/ui/ui4audio.xml, which map the names the
/// game's screens play (the symbols built from the name strings at
/// 0x82260500 onwards, e.g. "Pause" at 0x8226074C) to events of the
/// UIInGame project. Names are matched without case, as the game hashes
/// them (0x82DBE378 is called with its case-sensitive flag clear and
/// folds letters to lower case): the executable plays
/// "Map_AmbientBackgroundLoop" and the file lists "MAP_AmbientBackgroundLoop".
///
/// Verified: 754 of the 757 defaults name a UIInGame group; two name a
/// "General" group (the speed camera flashes), which is no project under
/// media/audio/ui, and one misspells its group attribute ("ggroup"); those
/// three are left out. No name appears twice. The file's overrides, which
/// the per-scene overrides apply to the post-race, trophy and showroom
/// screens, are for screens this reimplementation does not have and are
/// not read.
class UiSoundTable {
public:
    /// Throws DataError naming `source` if the text is not a ui4audio file.
    static UiSoundTable parse(std::string_view xml, const std::string& source);
    /// Throws DataError if the file is missing or malformed.
    static UiSoundTable load(const Vfs& vfs);

    /// The first entry with the name, or null.
    const UiSound* find(std::string_view name) const;
    std::size_t size() const { return m_sounds.size(); }

private:
    std::vector<UiSound> m_sounds;
};

/// Plays the game's UI sounds by name, as the screens' sound calls do
/// (CForzaUIScene slot 120 plays, slot 124 stops; the sound manager's slot
/// 284 behind 0x824A3770 plays for the game states), through the events of
/// Game:\Media\Audio\UI\UIInGame\UIInGame.fev on one mixer bus.
///
/// An event plays at most its FMOD max playbacks at once; a play beyond
/// that restarts the oldest instance. Which of FMOD's max-playback
/// behaviours an event has is stored in the part of the event's property
/// block that is not decoded (see audio::FevProject); restarting the oldest
/// is FMOD Designer's default, taken as inferred. A name that is not in the
/// table, or whose event is not in the project, is logged once and plays
/// nothing; so do the "Blank" cues, whose event has no sounds.
class UiSounds {
public:
    /// Throws DataError if ui4audio.xml cannot be read.
    UiSounds(const Vfs& vfs, audio::Mixer& mixer, audio::AudioLibrary& library, audio::BusId bus);
    ~UiSounds();
    UiSounds(const UiSounds&) = delete;
    UiSounds& operator=(const UiSounds&) = delete;

    void play(std::string_view name);
    /// Stops every instance of the name's event.
    void stop(std::string_view name);
    /// Sets a parameter on every playing instance of the name's event.
    void setParameter(std::string_view name, std::string_view parameter, float value);
    /// Whether the name's event is sounding: a one-shot until it ends, a
    /// loop while started, even when its parameters leave it silent.
    bool isPlaying(std::string_view name) const;

    /// Every name play() was given, in order, for tests and tools.
    const std::vector<std::string>& played() const { return m_played; }

private:
    struct Playing {
        std::string name;
        bool looping = false;
        std::unique_ptr<audio::EventInstance> instance;
    };

    /// Drops stopped instances and one-shots that have finished sounding; a
    /// loop stays while started, even when its parameters leave it silent.
    void prune();

    audio::Mixer& m_mixer;
    audio::AudioLibrary& m_library;
    audio::BusId m_bus;
    UiSoundTable m_table;
    std::shared_ptr<const audio::FevProject> m_project;
    std::vector<Playing> m_playing;
    std::vector<std::string> m_played;
    std::vector<std::string> m_missing;
};

} // namespace fh::ui
