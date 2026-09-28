#include "EventPlayer.h"

#include "AudioLibrary.h"

#include "base/Log.h"

#include <algorithm>
#include <cmath>

namespace fh::audio {

EventInstance::EventInstance(Mixer& mixer, AudioLibrary& library, std::string projectPath,
    std::shared_ptr<const FevProject> project, const FevEvent& event, EventSettings settings)
    : m_mixer(mixer)
    , m_library(library)
    , m_projectPath(std::move(projectPath))
    , m_project(std::move(project))
    , m_event(event)
    , m_settings(settings)
    , m_random(m_settings.seed)
{
    for (const FevParameter& parameter : m_event.parameters) {
        m_parameters.push_back(parameter.minimum);
    }
    for (std::size_t l = 0; l < m_event.layers.size(); ++l) {
        for (std::size_t s = 0; s < m_event.layers[l].sounds.size(); ++s) {
            m_slots.push_back(Slot{l, s, kNoVoice, false});
        }
    }
}

EventInstance::~EventInstance()
{
    stop(0.01F);
}

float EventInstance::voiceVolume() const
{
    return m_event.volume * m_settings.volume;
}

float EventInstance::voicePitch() const
{
    return std::exp2(m_event.pitch) * m_settings.pitch;
}

float EventInstance::position(std::size_t index) const
{
    if (index >= m_event.parameters.size()) {
        return 0.0F;
    }
    const FevParameter& parameter = m_event.parameters[index];
    const float range = parameter.maximum - parameter.minimum;
    return range > 0.0F ? (m_parameters[index] - parameter.minimum) / range : 0.0F;
}

bool EventInstance::isActive(const FevLayer& layer, const FevSoundInstance& sound) const
{
    if (m_event.simple || layer.controlParameter >= m_event.parameters.size()) {
        return true;
    }
    // Instances laid end to end share their boundary; a small tolerance keeps
    // the parameter's extremes inside the first and last instance.
    constexpr float kTolerance = 1e-4F;
    const float at = position(layer.controlParameter);
    return at >= sound.start - kTolerance && at <= sound.start + sound.length + kTolerance;
}

float EventInstance::layerGain(const FevLayer& layer) const
{
    float gain = 1.0F;
    for (const FevEnvelope& envelope : layer.envelopes) {
        if (envelope.parent < 0 && envelope.dspName.empty() && envelope.isVolume()) {
            gain *= std::clamp(envelope.valueAt(position(envelope.parameter)), 0.0F, 1.0F);
        }
    }
    return gain;
}

VoiceId EventInstance::startSound(const FevLayer& layer, const FevSoundInstance& sound)
{
    const FevSoundDefinition& definition = m_project->soundDefinition(sound.soundDefinition);
    std::uint32_t totalWeight = 0;
    for (const FevWaveform& waveform : definition.waveforms) {
        totalWeight += waveform.weight;
    }
    if (definition.waveforms.empty() || totalWeight == 0) {
        return kNoVoice;
    }
    std::uniform_int_distribution<std::uint32_t> pick(0, totalWeight - 1);
    std::uint32_t roll = pick(m_random);
    const FevWaveform* chosen = &definition.waveforms.back();
    for (const FevWaveform& waveform : definition.waveforms) {
        if (roll < waveform.weight) {
            chosen = &waveform;
            break;
        }
        roll -= waveform.weight;
    }
    if (chosen->type != FevWaveform::Type::Wavetable) {
        return kNoVoice;
    }
    std::shared_ptr<const PcmBuffer> pcm;
    try {
        pcm = m_library.waveform(m_projectPath, *chosen);
    } catch (const DataError& e) {
        logWarning("audio: event '{}' cannot play '{}': {}", m_event.path, chosen->fileName, e.what());
        return kNoVoice;
    }
    VoiceSettings voice;
    voice.volume = voiceVolume() * layerGain(layer);
    voice.pitch = voicePitch();
    voice.loop = !sound.oneShot;
    voice.bus = m_settings.bus;
    voice.spatial = m_settings.spatial && m_event.is3d();
    voice.position = m_settings.position;
    voice.rolloff = m_settings.rolloff.value_or(Rolloff{m_event.minDistance, m_event.maxDistance});
    return m_mixer.play(std::move(pcm), voice);
}

void EventInstance::refresh()
{
    for (Slot& slot : m_slots) {
        const FevLayer& layer = m_event.layers[slot.layer];
        const FevSoundInstance& sound = layer.sounds[slot.sound];
        const bool active = m_started && isActive(layer, sound);
        if (active && slot.voice == kNoVoice && !slot.fired) {
            slot.voice = startSound(layer, sound);
            slot.fired = sound.oneShot;
        } else if (active) {
            m_mixer.setVolume(slot.voice, voiceVolume() * layerGain(layer));
        } else {
            if (slot.voice != kNoVoice && !sound.oneShot) {
                m_mixer.stop(slot.voice, 0.05F);
            }
            if (!sound.oneShot || !m_mixer.isPlaying(slot.voice)) {
                slot.voice = kNoVoice;
            }
            slot.fired = false;
        }
    }
}

void EventInstance::start()
{
    m_started = true;
    refresh();
}

void EventInstance::stop(float fadeSeconds)
{
    m_started = false;
    for (Slot& slot : m_slots) {
        if (slot.voice != kNoVoice) {
            m_mixer.stop(slot.voice, fadeSeconds);
        }
        slot.voice = kNoVoice;
        slot.fired = false;
    }
}

void EventInstance::setParameter(std::string_view name, float value)
{
    for (std::size_t i = 0; i < m_event.parameters.size(); ++i) {
        const FevParameter& parameter = m_event.parameters[i];
        if (iequals(parameter.name, name)) {
            m_parameters[i] = std::clamp(value, std::min(parameter.minimum, parameter.maximum),
                std::max(parameter.minimum, parameter.maximum));
        }
    }
    refresh();
}

void EventInstance::setVolume(float volume)
{
    m_settings.volume = volume;
    refresh();
}

void EventInstance::setPitch(float pitch)
{
    m_settings.pitch = pitch;
    for (const Slot& slot : m_slots) {
        m_mixer.setPitch(slot.voice, voicePitch());
    }
}

void EventInstance::setPosition(const glm::vec3& position)
{
    m_settings.position = position;
    for (const Slot& slot : m_slots) {
        m_mixer.setPosition(slot.voice, position);
    }
}

bool EventInstance::isPlaying() const
{
    return std::any_of(m_slots.begin(), m_slots.end(), [this](const Slot& slot) {
        return m_mixer.isPlaying(slot.voice);
    });
}

} // namespace fh::audio
