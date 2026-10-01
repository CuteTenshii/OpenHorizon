#pragma once

#include "Decode.h"
#include "Dsp.h"

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

namespace openhorizon::audio {

/// GlobalRolloffs.xml's minDist, maxDist, beginFadeToZeroDist and fadeToZeroDist: full volume to `minDistance`,
/// inverse fall-off to `maxDistance`, flat to `fadeStart`, linear to silence at `fadeEnd`. Inferred from the
/// names and the inverse rolloff AudioEngineConfig selects.
struct Rolloff {
    float minDistance = 1.0F;
    float maxDistance = 10000.0F;
    float fadeStart = std::numeric_limits<float>::infinity();
    float fadeEnd = std::numeric_limits<float>::infinity();

    float gain(float distance) const;
};

/// Audio produced while it plays. Must deliver the mixer's sample rate: streams are not resampled.
class PcmStream {
public:
    virtual ~PcmStream() = default;
    /// 1 or 2; fixed for the stream's life.
    virtual int channels() const = 0;
    /// Returns the frames written; fewer leaves silence. Runs under the mixer's lock on the audio thread:
    /// must not block or call back into the mixer.
    virtual std::size_t read(float* out, std::size_t frames) = 0;
    /// True once the stream will never produce audio again.
    virtual bool finished() const = 0;
};

/// Identifies a voice across the mixer's slot reuse; 0 is never a voice.
using VoiceId = std::uint64_t;
inline constexpr VoiceId kNoVoice = 0;

/// A submix; bus 0 is the master, which has no filters.
using BusId = std::uint32_t;
inline constexpr BusId kMasterBus = 0;

struct VoiceSettings {
    float volume = 1.0F;
    /// -1 left .. 1 right, added to a spatial voice's pan.
    float pan = 0.0F;
    float pitch = 1.0F;
    bool loop = false;
    BusId bus = kMasterBus;
    /// Attenuate and pan by `position` relative to the listener.
    bool spatial = false;
    glm::vec3 position{0.0F};
    Rolloff rolloff{};
    std::size_t startFrame = 0;
};

/// A software mixer to interleaved stereo float. Voices resample with four-point Hermite at buffer rate /
/// output rate * pitch; changes ramp over each block so game-rate updates do not click. Thread-safe; render()
/// holds the lock for one block.
class Mixer {
public:
    static constexpr std::size_t kMaxVoices = 256;
    /// Frames mixed per internal step; ramps span one step.
    static constexpr std::size_t kBlockFrames = 256;

    explicit Mixer(int sampleRate);

    int sampleRate() const { return m_sampleRate; }

    /// kNoVoice (logged) when all voices are busy or the buffer is empty.
    VoiceId play(std::shared_ptr<const PcmBuffer> buffer, const VoiceSettings& settings);
    /// Pitch, looping and startFrame do not apply. kNoVoice (logged) when all voices are busy or the
    /// stream does not have 1 or 2 channels.
    VoiceId playStream(std::shared_ptr<PcmStream> stream, const VoiceSettings& settings);
    /// Fades the voice out over `fadeSeconds` and frees it.
    void stop(VoiceId voice, float fadeSeconds = 0.01F);
    void stopAll();
    void setVolume(VoiceId voice, float volume);
    void setPitch(VoiceId voice, float pitch);
    void setPan(VoiceId voice, float pan);
    void setLooping(VoiceId voice, bool loop);
    void setPosition(VoiceId voice, const glm::vec3& position);
    bool isPlaying(VoiceId voice) const;
    std::size_t activeVoiceCount() const;

    /// Lives until destroyBus(); an `output` that is not a live bus means the master.
    BusId createBus(BusId output = kMasterBus);
    /// Stops its voices; buses that fed it feed the master from then on.
    void destroyBus(BusId bus);
    void setBusVolume(BusId bus, float volume);
    /// 1 for a bus never set, 0 for one that does not exist.
    float busVolume(BusId bus) const;
    /// The product of the volumes on the way to the master, without filters or the master volume.
    float busGain(BusId bus) const;
    /// Filter state carries over while the chain keeps its shape.
    void setBusFilters(BusId bus, std::span<const FilterSpec> filters);

    /// `right` is the right ear's direction, which keeps the pan independent of handedness.
    void setListener(const glm::vec3& position, const glm::vec3& right);
    glm::vec3 listenerPosition() const;
    void setMasterVolume(float volume);

    /// Overwrites `out`; the master passes a peak limiter at 0.98 full scale.
    void render(std::span<float> out);

private:
    struct Voice {
        std::shared_ptr<const PcmBuffer> buffer;
        std::shared_ptr<PcmStream> stream;
        VoiceSettings settings;
        std::uint32_t generation = 0;
        bool active = false;
        bool stopping = false;
        float stopRate = 0.0F;
        double position = 0.0;
        double step = 0.0;
        float gainLeft = 0.0F;
        float gainRight = 0.0F;
        float fade = 1.0F;
        bool started = false;
    };

    struct Bus {
        bool active = false;
        BusId output = kMasterBus;
        float volume = 1.0F;
        float appliedVolume = 1.0F;
        bool volumeSet = false;
        std::vector<FilterSpec> filters;
        /// The block glides from `current` to `target`.
        std::vector<BiquadCoefficients> current;
        std::vector<BiquadCoefficients> target;
        std::vector<Biquad> left;
        std::vector<Biquad> right;
        std::vector<float> buffer;
    };

    VoiceId claimVoice(Voice&& voice);
    int channelsOf(const Voice& voice) const;
    void mixStreamVoice(Voice& voice, float* bus, std::size_t frames);
    Voice* find(VoiceId voice);
    const Voice* find(VoiceId voice) const;
    void targetGains(const Voice& voice, float& left, float& right) const;
    void mixVoice(Voice& voice, float* bus, std::size_t frames);
    void renderBlock(float* out, std::size_t frames);
    /// Rebuilds m_busOrder; the caller holds the lock.
    void orderBuses();

    const int m_sampleRate;
    mutable std::mutex m_mutex;
    std::vector<Voice> m_voices;
    std::vector<float> m_streamScratch;
    std::vector<Bus> m_buses;
    /// Furthest from the master first, so every bus is mixed before its output.
    std::vector<BusId> m_busOrder;
    glm::vec3 m_listenerPosition{0.0F};
    glm::vec3 m_listenerRight{1.0F, 0.0F, 0.0F};
    float m_masterVolume = 1.0F;
    float m_limiterGain = 1.0F;
};

} // namespace openhorizon::audio
