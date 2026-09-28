#include "Mixer.h"

#include "base/Log.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace fh::audio {

namespace {

constexpr float kSilence = 1e-7F;
constexpr float kLimiterCeiling = 0.98F;
constexpr float kLimiterReleaseSeconds = 0.08F;

std::uint32_t slotOf(VoiceId voice)
{
    return static_cast<std::uint32_t>(voice & 0xFFFFFFFFU) - 1U;
}

std::uint32_t generationOf(VoiceId voice)
{
    return static_cast<std::uint32_t>(voice >> 32);
}

/// The buffer's sample at frame `index` of channel `channel`, following the
/// loop region when looping and reading silence outside the buffer
/// otherwise.
float sampleAt(const PcmBuffer& pcm, std::ptrdiff_t index, int channel, bool loop)
{
    const auto frames = static_cast<std::ptrdiff_t>(pcm.frames());
    if (loop) {
        const auto loopStart = static_cast<std::ptrdiff_t>(pcm.loopStart);
        const auto loopEnd = static_cast<std::ptrdiff_t>(pcm.loopEnd);
        if (index >= loopEnd) {
            index = loopStart + (index - loopStart) % (loopEnd - loopStart);
        }
        index = std::max<std::ptrdiff_t>(index, 0);
    } else if (index < 0 || index >= frames) {
        return 0.0F;
    }
    return pcm.samples[static_cast<std::size_t>(index) * static_cast<std::size_t>(pcm.channels)
        + static_cast<std::size_t>(channel)];
}

} // namespace

float Rolloff::gain(float distance) const
{
    if (!(distance > minDistance)) {
        return 1.0F;
    }
    if (distance >= fadeEnd) {
        return 0.0F;
    }
    const float floorGain = minDistance / std::max(maxDistance, minDistance);
    float g = distance < maxDistance ? minDistance / distance : floorGain;
    if (distance > fadeStart) {
        const float span = fadeEnd - fadeStart;
        g *= span > 0.0F ? (fadeEnd - distance) / span : 0.0F;
    }
    return g;
}

Mixer::Mixer(int sampleRate)
    : m_sampleRate(sampleRate)
    , m_voices(kMaxVoices)
    , m_buses(1)
{
    m_buses[kMasterBus].active = true;
    m_buses[kMasterBus].buffer.assign(kBlockFrames * 2, 0.0F);
}

Mixer::Voice* Mixer::find(VoiceId voice)
{
    const std::uint32_t slot = slotOf(voice);
    if (voice == kNoVoice || slot >= m_voices.size()) {
        return nullptr;
    }
    Voice& v = m_voices[slot];
    return v.active && v.generation == generationOf(voice) ? &v : nullptr;
}

const Mixer::Voice* Mixer::find(VoiceId voice) const
{
    return const_cast<Mixer*>(this)->find(voice);
}

VoiceId Mixer::play(std::shared_ptr<const PcmBuffer> buffer, const VoiceSettings& settings)
{
    if (!buffer || buffer->channels <= 0 || buffer->sampleRate <= 0 || buffer->frames() == 0) {
        logWarning("audio: not playing an empty buffer");
        return kNoVoice;
    }
    Voice v;
    v.settings = settings;
    if (v.settings.loop && (buffer->loopEnd <= buffer->loopStart || buffer->loopEnd > buffer->frames())) {
        v.settings.loop = false;
    }
    v.position = static_cast<double>(std::min(settings.startFrame, buffer->frames() - 1));
    v.buffer = std::move(buffer);
    std::lock_guard lock(m_mutex);
    return claimVoice(std::move(v));
}

VoiceId Mixer::playStream(std::shared_ptr<PcmStream> stream, const VoiceSettings& settings)
{
    if (!stream || (stream->channels() != 1 && stream->channels() != 2)) {
        logWarning("audio: not playing a stream with {} channels", stream ? stream->channels() : 0);
        return kNoVoice;
    }
    Voice v;
    v.settings = settings;
    v.settings.loop = false;
    v.settings.pitch = 1.0F;
    v.stream = std::move(stream);
    std::lock_guard lock(m_mutex);
    return claimVoice(std::move(v));
}

VoiceId Mixer::claimVoice(Voice&& voice)
{
    for (std::size_t slot = 0; slot < m_voices.size(); ++slot) {
        Voice& v = m_voices[slot];
        if (v.active) {
            continue;
        }
        const std::uint32_t generation = v.generation + 1;
        v = std::move(voice);
        v.generation = generation;
        if (v.settings.bus >= m_buses.size() || !m_buses[v.settings.bus].active) {
            v.settings.bus = kMasterBus;
        }
        v.active = true;
        return (static_cast<VoiceId>(generation) << 32) | (slot + 1);
    }
    logWarning("audio: all {} voices are busy", kMaxVoices);
    return kNoVoice;
}

int Mixer::channelsOf(const Voice& voice) const
{
    return voice.stream ? voice.stream->channels() : voice.buffer->channels;
}

void Mixer::stop(VoiceId voice, float fadeSeconds)
{
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(voice)) {
        v->stopping = true;
        v->stopRate = fadeSeconds > 0.0F ? 1.0F / (fadeSeconds * static_cast<float>(m_sampleRate)) : 1.0F;
    }
}

void Mixer::stopAll()
{
    std::lock_guard lock(m_mutex);
    for (Voice& v : m_voices) {
        v.active = false;
        v.buffer.reset();
        v.stream.reset();
    }
}

void Mixer::setVolume(VoiceId voice, float volume)
{
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(voice)) {
        v->settings.volume = std::max(volume, 0.0F);
    }
}

void Mixer::setPitch(VoiceId voice, float pitch)
{
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(voice)) {
        v->settings.pitch = v->stream ? 1.0F : std::clamp(pitch, 0.0F, 16.0F);
    }
}

void Mixer::setPan(VoiceId voice, float pan)
{
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(voice)) {
        v->settings.pan = std::clamp(pan, -1.0F, 1.0F);
    }
}

void Mixer::setLooping(VoiceId voice, bool loop)
{
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(voice)) {
        v->settings.loop = loop && v->buffer && v->buffer->loopEnd > v->buffer->loopStart;
    }
}

void Mixer::setPosition(VoiceId voice, const glm::vec3& position)
{
    std::lock_guard lock(m_mutex);
    if (Voice* v = find(voice)) {
        v->settings.position = position;
    }
}

bool Mixer::isPlaying(VoiceId voice) const
{
    std::lock_guard lock(m_mutex);
    return find(voice) != nullptr;
}

std::size_t Mixer::activeVoiceCount() const
{
    std::lock_guard lock(m_mutex);
    return static_cast<std::size_t>(std::count_if(m_voices.begin(), m_voices.end(), [](const Voice& v) {
        return v.active;
    }));
}

BusId Mixer::createBus()
{
    std::lock_guard lock(m_mutex);
    for (std::size_t i = 1; i < m_buses.size(); ++i) {
        if (!m_buses[i].active) {
            m_buses[i] = Bus{};
            m_buses[i].active = true;
            m_buses[i].buffer.assign(kBlockFrames * 2, 0.0F);
            return static_cast<BusId>(i);
        }
    }
    Bus bus;
    bus.active = true;
    bus.buffer.assign(kBlockFrames * 2, 0.0F);
    m_buses.push_back(std::move(bus));
    return static_cast<BusId>(m_buses.size() - 1);
}

void Mixer::destroyBus(BusId bus)
{
    std::lock_guard lock(m_mutex);
    if (bus == kMasterBus || bus >= m_buses.size()) {
        return;
    }
    for (Voice& v : m_voices) {
        if (v.active && v.settings.bus == bus) {
            v.active = false;
            v.buffer.reset();
            v.stream.reset();
        }
    }
    m_buses[bus] = Bus{};
}

void Mixer::setBusVolume(BusId bus, float volume)
{
    std::lock_guard lock(m_mutex);
    if (bus < m_buses.size() && m_buses[bus].active) {
        Bus& b = m_buses[bus];
        b.volume = std::max(volume, 0.0F);
        if (!b.volumeSet) {
            // The first volume is where the bus starts, not a ramp from 1.
            b.appliedVolume = b.volume;
            b.volumeSet = true;
        }
    }
}

void Mixer::setBusFilters(BusId bus, std::span<const FilterSpec> filters)
{
    std::lock_guard lock(m_mutex);
    if (bus == kMasterBus || bus >= m_buses.size() || !m_buses[bus].active) {
        return;
    }
    Bus& b = m_buses[bus];
    bool sameShape = b.filters.size() == filters.size();
    for (std::size_t i = 0; sameShape && i < filters.size(); ++i) {
        sameShape = b.filters[i].type == filters[i].type;
    }
    b.filters.assign(filters.begin(), filters.end());
    b.target.resize(filters.size());
    for (std::size_t i = 0; i < filters.size(); ++i) {
        b.target[i] = BiquadCoefficients::design(filters[i], static_cast<float>(m_sampleRate));
    }
    if (!sameShape) {
        b.left.assign(filters.size(), Biquad{});
        b.right.assign(filters.size(), Biquad{});
        b.current = b.target;
    }
}

void Mixer::setListener(const glm::vec3& position, const glm::vec3& right)
{
    std::lock_guard lock(m_mutex);
    m_listenerPosition = position;
    const float length = glm::length(right);
    m_listenerRight = length > 0.0F ? right / length : glm::vec3(1.0F, 0.0F, 0.0F);
}

void Mixer::setMasterVolume(float volume)
{
    std::lock_guard lock(m_mutex);
    m_masterVolume = std::max(volume, 0.0F);
}

void Mixer::targetGains(const Voice& voice, float& left, float& right) const
{
    const VoiceSettings& s = voice.settings;
    float volume = s.volume;
    float pan = s.pan;
    if (s.spatial) {
        const glm::vec3 offset = s.position - m_listenerPosition;
        const float distance = glm::length(offset);
        volume *= s.rolloff.gain(distance);
        if (distance > 1e-3F) {
            pan += glm::dot(offset, m_listenerRight) / distance;
        }
    }
    pan = std::clamp(pan, -1.0F, 1.0F);
    if (channelsOf(voice) == 1) {
        const float angle = (pan + 1.0F) * std::numbers::pi_v<float> / 4.0F;
        left = volume * std::cos(angle);
        right = volume * std::sin(angle);
    } else {
        left = volume * std::min(1.0F, 1.0F - pan);
        right = volume * std::min(1.0F, 1.0F + pan);
    }
}

void Mixer::mixStreamVoice(Voice& voice, float* bus, std::size_t frames)
{
    float targetLeft = 0.0F;
    float targetRight = 0.0F;
    targetGains(voice, targetLeft, targetRight);
    if (voice.stopping) {
        voice.fade = std::max(0.0F, voice.fade - voice.stopRate * static_cast<float>(frames));
    }
    targetLeft *= voice.fade;
    targetRight *= voice.fade;
    if (!voice.started) {
        voice.gainLeft = targetLeft;
        voice.gainRight = targetRight;
        voice.started = true;
    }
    const auto channels = static_cast<std::size_t>(voice.stream->channels());
    m_streamScratch.resize(frames * channels);
    // A stream is read even when silent, so that it keeps time.
    const std::size_t got = std::min(voice.stream->read(m_streamScratch.data(), frames), frames);
    const float inverse = 1.0F / static_cast<float>(frames);
    const float deltaLeft = (targetLeft - voice.gainLeft) * inverse;
    const float deltaRight = (targetRight - voice.gainRight) * inverse;
    float gainLeft = voice.gainLeft;
    float gainRight = voice.gainRight;
    for (std::size_t i = 0; i < got; ++i) {
        const float left = m_streamScratch[i * channels];
        const float right = m_streamScratch[i * channels + channels - 1];
        bus[i * 2] += left * gainLeft;
        bus[i * 2 + 1] += right * gainRight;
        gainLeft += deltaLeft;
        gainRight += deltaRight;
    }
    voice.gainLeft = targetLeft;
    voice.gainRight = targetRight;
    if (voice.stream->finished() || (voice.stopping && voice.fade <= 0.0F)) {
        voice.active = false;
        voice.stream.reset();
    }
}

void Mixer::mixVoice(Voice& voice, float* bus, std::size_t frames)
{
    if (voice.stream) {
        mixStreamVoice(voice, bus, frames);
        return;
    }
    const PcmBuffer& pcm = *voice.buffer;
    const bool loop = voice.settings.loop;
    float targetLeft = 0.0F;
    float targetRight = 0.0F;
    targetGains(voice, targetLeft, targetRight);
    if (voice.stopping) {
        voice.fade = std::max(0.0F, voice.fade - voice.stopRate * static_cast<float>(frames));
    }
    targetLeft *= voice.fade;
    targetRight *= voice.fade;
    const double targetStep = static_cast<double>(pcm.sampleRate) / m_sampleRate * voice.settings.pitch;
    if (!voice.started) {
        voice.gainLeft = targetLeft;
        voice.gainRight = targetRight;
        voice.step = targetStep;
        voice.started = true;
    }

    const auto total = static_cast<double>(pcm.frames());
    const auto loopStart = static_cast<double>(pcm.loopStart);
    const auto loopLength = static_cast<double>(pcm.loopEnd - pcm.loopStart);
    const float inverse = 1.0F / static_cast<float>(frames);
    const float deltaLeft = (targetLeft - voice.gainLeft) * inverse;
    const float deltaRight = (targetRight - voice.gainRight) * inverse;
    const double deltaStep = (targetStep - voice.step) / static_cast<double>(frames);
    const bool silent = std::max({voice.gainLeft, voice.gainRight, targetLeft, targetRight}) < kSilence;

    float gainLeft = voice.gainLeft;
    float gainRight = voice.gainRight;
    double step = voice.step;
    double position = voice.position;
    const int channels = pcm.channels;
    const float channelScale = channels > 2 ? 2.0F / static_cast<float>(channels) : 1.0F;
    bool finished = false;
    for (std::size_t i = 0; i < frames; ++i) {
        if (!silent) {
            const auto index = static_cast<std::ptrdiff_t>(position);
            const auto t = static_cast<float>(position - static_cast<double>(index));
            float left = 0.0F;
            float right = 0.0F;
            for (int c = 0; c < channels; ++c) {
                const float value = hermite(sampleAt(pcm, index - 1, c, loop), sampleAt(pcm, index, c, loop),
                    sampleAt(pcm, index + 1, c, loop), sampleAt(pcm, index + 2, c, loop), t);
                if (channels == 1) {
                    left = value;
                    right = value;
                } else if (c % 2 == 0) {
                    left += value * channelScale;
                } else {
                    right += value * channelScale;
                }
            }
            bus[i * 2] += left * gainLeft;
            bus[i * 2 + 1] += right * gainRight;
        }
        gainLeft += deltaLeft;
        gainRight += deltaRight;
        step += deltaStep;
        position += step;
        if (loop) {
            if (position >= loopStart + loopLength) {
                position = loopStart + std::fmod(position - loopStart, loopLength);
            }
        } else if (position >= total) {
            finished = true;
            break;
        }
    }
    voice.gainLeft = targetLeft;
    voice.gainRight = targetRight;
    voice.step = targetStep;
    voice.position = position;
    if (finished || (voice.stopping && voice.fade <= 0.0F)) {
        voice.active = false;
        voice.buffer.reset();
    }
}

void Mixer::renderBlock(float* out, std::size_t frames)
{
    for (Bus& bus : m_buses) {
        if (bus.active) {
            std::fill_n(bus.buffer.begin(), frames * 2, 0.0F);
        }
    }
    for (Voice& voice : m_voices) {
        if (voice.active) {
            mixVoice(voice, m_buses[voice.settings.bus].buffer.data(), frames);
        }
    }
    float* master = m_buses[kMasterBus].buffer.data();
    const float inverse = 1.0F / static_cast<float>(frames);
    for (std::size_t b = 1; b < m_buses.size(); ++b) {
        Bus& bus = m_buses[b];
        if (!bus.active) {
            continue;
        }
        constexpr std::size_t kGlideFrames = 32;
        for (std::size_t start = 0; start < frames && !bus.left.empty(); start += kGlideFrames) {
            const std::size_t count = std::min(kGlideFrames, frames - start);
            const float t = static_cast<float>(start + count) * inverse;
            for (std::size_t f = 0; f < bus.left.size(); ++f) {
                const BiquadCoefficients c = BiquadCoefficients::interpolate(bus.current[f], bus.target[f], t);
                bus.left[f].setCoefficients(c);
                bus.right[f].setCoefficients(c);
                bus.left[f].process(bus.buffer.data() + start * 2, count, 2);
                bus.right[f].process(bus.buffer.data() + start * 2 + 1, count, 2);
            }
        }
        bus.current = bus.target;
        float gain = bus.appliedVolume;
        const float delta = (bus.volume - bus.appliedVolume) * inverse;
        for (std::size_t i = 0; i < frames; ++i) {
            gain += delta;
            master[i * 2] += bus.buffer[i * 2] * gain;
            master[i * 2 + 1] += bus.buffer[i * 2 + 1] * gain;
        }
        bus.appliedVolume = bus.volume;
    }
    const float release = 1.0F - std::exp(-1.0F / (kLimiterReleaseSeconds * static_cast<float>(m_sampleRate)));
    for (std::size_t i = 0; i < frames; ++i) {
        const float left = master[i * 2] * m_masterVolume;
        const float right = master[i * 2 + 1] * m_masterVolume;
        const float peak = std::max(std::abs(left), std::abs(right));
        const float needed = peak * m_limiterGain > kLimiterCeiling ? kLimiterCeiling / peak : 1.0F;
        if (needed < m_limiterGain) {
            m_limiterGain = needed;
        } else {
            m_limiterGain += (std::min(needed, 1.0F) - m_limiterGain) * release;
            if (peak * m_limiterGain > kLimiterCeiling) {
                m_limiterGain = kLimiterCeiling / peak;
            }
        }
        out[i * 2] = left * m_limiterGain;
        out[i * 2 + 1] = right * m_limiterGain;
    }
}

void Mixer::render(std::span<float> out)
{
    std::lock_guard lock(m_mutex);
    const std::size_t frames = out.size() / 2;
    for (std::size_t done = 0; done < frames;) {
        const std::size_t count = std::min(kBlockFrames, frames - done);
        renderBlock(out.data() + done * 2, count);
        done += count;
    }
}

} // namespace fh::audio
