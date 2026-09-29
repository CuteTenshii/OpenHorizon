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

namespace fh::audio {

class AudioLibrary;

struct EventSettings {
    float volume = 1.0F;
    /// Multiplies the event's own pitch.
    float pitch = 1.0F;
    BusId bus = kMasterBus;
    /// Position the event in the world; only honoured for 3D events.
    bool spatial = false;
    glm::vec3 position{0.0F};
    /// Overrides the inverse rolloff built from the event's min and max
    /// distance.
    std::optional<Rolloff> rolloff;
    std::uint32_t seed = 1;
};

/// Plays one FMOD Designer event as far as the FEV layout is understood:
/// each layer plays the sound instances whose [start, start + length] range
/// contains the current value of the layer's controlling parameter; a sound
/// instance plays a waveform of its sound definition picked at random by
/// weight, looping unless it is a one-shot. Event volume and pitch apply, as
/// do the layers' volume envelopes; pitch and effect envelopes do not (their
/// scales are unknown), and instances switch at their range boundaries
/// without FMOD's crossfades.
///
/// A spatial 3D event keeps Designer's built-in "(distance)" parameter at
/// its distance from the mixer's listener. An event whose mode has FMOD's
/// custom rolloff bit (0x4000000) gets no distance attenuation of its own:
/// such events draw their fall-off as a volume curve on "(distance)" (the
/// LOD2 tyre rolls, whose minimum distance is 0, are the case on the disc).
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

    /// Starts the sound instances active at the current parameter values.
    /// Waveforms are decoded on first use, so the first start of an event
    /// may take a moment.
    void start();
    void stop(float fadeSeconds = 0.05F);
    /// Sets a parameter by name (case-insensitive), clamped to its range,
    /// and starts or stops sound instances accordingly. Unknown names are
    /// ignored.
    void setParameter(std::string_view name, float value);
    void setVolume(float volume);
    void setPitch(float pitch);
    void setPosition(const glm::vec3& position);
    /// Whether any of its voices is still sounding.
    bool isPlaying() const;
    /// Between start() and stop(), whether or not any sound instance is in
    /// range.
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
    /// The product of the layer's volume envelopes at the current parameters.
    float layerGain(const FevLayer& layer) const;
    void refresh();
    VoiceId startSound(const FevLayer& layer, const FevSoundInstance& sound);
    float voiceVolume() const;
    float voicePitch() const;
    /// Sets "(distance)" from the listener, for spatial 3D events.
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

} // namespace fh::audio
