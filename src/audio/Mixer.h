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

namespace fh::audio {

/// Distance attenuation in the shape of the game's GlobalRolloffs.xml
/// entries (minDist, maxDist, beginFadeToZeroDist, fadeToZeroDist):
/// full volume up to `minDistance`, inverse-distance fall-off to
/// `maxDistance`, flat from there to `fadeStart`, then a linear fade to
/// silence at `fadeEnd`. The file only names the four distances; the curve
/// between them follows FMOD's inverse rolloff, which AudioEngineConfig
/// selects, and is otherwise inferred from the names.
struct Rolloff {
    float minDistance = 1.0F;
    float maxDistance = 10000.0F;
    float fadeStart = std::numeric_limits<float>::infinity();
    float fadeEnd = std::numeric_limits<float>::infinity();

    float gain(float distance) const;
};

/// Audio produced while it plays rather than held in a PcmBuffer, such as a
/// decoder feeding from a worker thread. The mixer plays a stream at its own
/// rate: the stream must deliver the mixer's sample rate, since nothing is
/// resampled.
class PcmStream {
public:
    virtual ~PcmStream() = default;
    /// 1 or 2; fixed for the stream's life.
    virtual int channels() const = 0;
    /// Writes up to `frames` interleaved frames to `out` and returns how many
    /// it wrote; fewer means the rest of the block is silence. Called on the
    /// audio thread with the mixer's lock held, so it must not block and
    /// must not call back into the mixer.
    virtual std::size_t read(float* out, std::size_t frames) = 0;
    /// True once the stream will never produce audio again; the mixer then
    /// frees the voice.
    virtual bool finished() const = 0;
};

/// Identifies a voice across the mixer's slot reuse; 0 is never a voice.
using VoiceId = std::uint64_t;
inline constexpr VoiceId kNoVoice = 0;

/// A submix that voices can play into; its filters and volume apply to the
/// sum. Bus 0 is the master, which has no filters.
using BusId = std::uint32_t;
inline constexpr BusId kMasterBus = 0;

struct VoiceSettings {
    float volume = 1.0F;
    /// -1 left .. 1 right, added to the pan a spatial voice gets from its
    /// position.
    float pan = 0.0F;
    /// Playback rate multiplier on top of the buffer's own sample rate.
    float pitch = 1.0F;
    /// Loop over the buffer's loop region instead of stopping at the end.
    bool loop = false;
    BusId bus = kMasterBus;
    /// Attenuate and pan by `position` relative to the listener.
    bool spatial = false;
    glm::vec3 position{0.0F};
    Rolloff rolloff{};
    std::size_t startFrame = 0;
};

/// A software mixer to interleaved stereo float at a fixed rate.
///
/// Voices resample their buffer with four-point Hermite interpolation at
/// `buffer rate / output rate * pitch`. Volume, pan and pitch changes ramp
/// over each rendered block so that game-rate updates do not click. A
/// voice at zero gain costs only its position update.
///
/// Every method may be called from any thread; render() runs on the audio
/// thread and holds the mixer's lock for one block, so control calls wait
/// at most that long.
class Mixer {
public:
    static constexpr std::size_t kMaxVoices = 256;
    /// Frames mixed per internal step; ramps span one step.
    static constexpr std::size_t kBlockFrames = 256;

    explicit Mixer(int sampleRate);

    int sampleRate() const { return m_sampleRate; }

    /// Starts a voice; returns kNoVoice (and logs) when all voices are busy
    /// or the buffer is empty.
    VoiceId play(std::shared_ptr<const PcmBuffer> buffer, const VoiceSettings& settings);
    /// Starts a voice that pulls from `stream`. Pitch, looping and
    /// startFrame do not apply to streams; volume, pan, bus and spatial
    /// settings do. Returns kNoVoice (and logs) when all voices are busy or
    /// the stream's channel count is not 1 or 2.
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

    /// A new bus feeding the master; buses live until destroyBus().
    BusId createBus();
    /// Stops the bus's voices and releases it.
    void destroyBus(BusId bus);
    void setBusVolume(BusId bus, float volume);
    /// Replaces the bus's filter chain; filter state carries over while the
    /// chain keeps its shape, so parameters can change every game frame.
    void setBusFilters(BusId bus, std::span<const FilterSpec> filters);

    /// The listener's position and the direction of its right ear, which
    /// makes the pan independent of the coordinate system's handedness.
    void setListener(const glm::vec3& position, const glm::vec3& right);
    glm::vec3 listenerPosition() const;
    void setMasterVolume(float volume);

    /// Mixes `out.size() / 2` stereo frames into `out`, overwriting it. The
    /// master output passes a peak limiter at 0.98 full scale, so a sum of
    /// loud voices dips instead of clipping.
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
        float volume = 1.0F;
        float appliedVolume = 1.0F;
        bool volumeSet = false;
        std::vector<FilterSpec> filters;
        /// Coefficients in use at the end of the last block and wanted by the
        /// end of the next; the block glides between them.
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

    const int m_sampleRate;
    mutable std::mutex m_mutex;
    std::vector<Voice> m_voices;
    std::vector<float> m_streamScratch;
    std::vector<Bus> m_buses;
    glm::vec3 m_listenerPosition{0.0F};
    glm::vec3 m_listenerRight{1.0F, 0.0F, 0.0F};
    float m_masterVolume = 1.0F;
    float m_limiterGain = 1.0F;
};

} // namespace fh::audio
