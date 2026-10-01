#pragma once

#include "Fev.h"
#include "Mixer.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::audio {

class AudioLibrary;

struct EventSettings {
    float volume = 1.0F;
    /// Multiplies the event's own pitch.
    float pitch = 1.0F;
    BusId bus = kMasterBus;
    /// Only honoured for 3D events.
    bool spatial = false;
    glm::vec3 position{0.0F};
    /// Overrides the inverse rolloff from the event's min and max distance.
    std::optional<Rolloff> rolloff;
    std::uint32_t seed = 1;
};

/// Plays one FMOD Designer event. Pitch and effect envelopes are ignored (scales unknown) and instances
/// switch without FMOD's crossfades. Spatial 3D events set "(distance)" from the listener; custom-rolloff
/// events (0x4000000, e.g. the LOD2 tyre rolls) get their fall-off only from a volume curve on it.
class EventInstance {
public:
    /// `projectPath` is the game path of the .fev, used to find its banks;
    /// `event` must belong to `project`, which the instance keeps alive.
    EventInstance(Mixer& mixer, AudioLibrary& library, std::string projectPath,
        std::shared_ptr<const FevProject> project, const FevEvent& event, EventSettings settings = {});
    ~EventInstance();
    EventInstance(const EventInstance&) = delete;
    EventInstance& operator=(const EventInstance&) = delete;

    const FevEvent& event() const { return m_event; }

    /// The first start may be slow: waveforms are decoded on first use.
    void start();
    void stop(float fadeSeconds = 0.05F);
    /// Case-insensitive, clamped to the range; unknown names are ignored.
    void setParameter(std::string_view name, float value);
    void setVolume(float volume);
    void setPitch(float pitch);
    void setPosition(const glm::vec3& position);
    /// Whether any of its voices is still sounding.
    bool isPlaying() const;
    /// Between start() and stop(), whether or not any instance is in range.
    bool isStarted() const { return m_started; }

private:
    struct Slot {
        std::size_t layer = 0;
        std::size_t sound = 0;
        VoiceId voice = kNoVoice;
        /// A one-shot plays once per entry into its range.
        bool fired = false;
    };

    /// Where parameter `index` sits in its range, 0..1.
    float position(std::size_t index) const;
    bool isActive(const FevLayer& layer, const FevSoundInstance& sound) const;
    float layerGain(const FevLayer& layer) const;
    void refresh();
    VoiceId startSound(const FevLayer& layer, const FevSoundInstance& sound);
    float voiceVolume() const;
    float voicePitch() const;
    void updateDistance();

    Mixer& m_mixer;
    AudioLibrary& m_library;
    std::string m_projectPath;
    std::shared_ptr<const FevProject> m_project;
    const FevEvent& m_event;
    EventSettings m_settings;
    std::vector<float> m_parameters;
    std::vector<Slot> m_slots;
    /// Index of the "(distance)" parameter, if the event has one.
    std::optional<std::size_t> m_distanceParameter;
    std::mt19937 m_random;
    bool m_started = false;
};

} // namespace openhorizon::audio
