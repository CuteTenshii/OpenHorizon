#pragma once

#include "EngineTuning.h"
#include "Mixer.h"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::audio {

class AudioDevice;
class AudioLibrary;
class EventInstance;

struct EngineSoundOptions {
    /// Upgrade levels 0 (stock) to 3, which pick the intake and exhaust
    /// recordings and the backfire behaviour.
    int intakeUpgrade = 0;
    int exhaustUpgrade = 0;
    /// The car model tuning view whose mix applies: "Follow" (chase camera),
    /// "Front" (bumper), "Cockpit" or "Default".
    std::string view = "Follow";
    /// Place the emitters in the world with setTransform() and attenuate them
    /// by GlobalRolloffs.xml; otherwise they play in stereo, panned by the
    /// view's surround levels.
    bool spatial = false;
    /// Skid and gear-shift sounds from Tires.fev and Transmissions.fev.
    bool tyres = true;
    bool transmission = true;
    std::uint32_t seed = 1;
};

/// The engine, exhaust, burble, gear-shift and tyre sounds of one car.
///
/// The car's engine tuning names three harmonic tunings (intake, engine
/// ambient, exhaust); each is a bank of loops recorded at steady rpm. Every
/// loop plays continuously at pitch rpm / rpm_sample, and its volume
/// crossfades with equal power across the overlap of its rpm range with its
/// neighbours'. There are no separate on-load and off-load recordings: load
/// shapes the sound through the tunings' volume curves and EQ (see
/// EmissionDsp), evaluated from rpm, throttle and an estimate of positive
/// and negative load, and applied per emitter on a mixer bus. At the
/// redline with the throttle open, a rev limiter cuts the load in 35 ms
/// pulses. On the overrun, burbles (and, with an upgraded exhaust,
/// backfires) fire at random intervals inside the tuning's rpm window.
class EngineSound {
public:
    /// Loads and decodes the car's sounds, which takes a moment. Throws
    /// DataError if the car has no engine tuning.
    EngineSound(AudioDevice& device, const Vfs& vfs, const std::string& carMedia, EngineSoundOptions options = {});
    /// As above, sharing decoded samples with other sounds.
    EngineSound(AudioDevice& device, std::shared_ptr<AudioLibrary> library, const std::string& carMedia,
        EngineSoundOptions options = {});
    ~EngineSound();
    EngineSound(const EngineSound&) = delete;
    EngineSound& operator=(const EngineSound&) = delete;

    const CarAudioTuning& tuning() const { return m_tuning; }

    /// Advances the sound by `dt` seconds for engine speed `rpm`, throttle
    /// 0..1 and road speed in m/s.
    void update(float rpm, float throttle, float speed, float dt);
    /// Plays the gear-change clunk and applies the tuning's shift volume
    /// boost; an upshift also gives the exhaust a chance to backfire.
    void shiftGear(int fromGear, int toGear);
    /// Tyre slip 0 (gripping) to 1 (sliding), driving the skid sound.
    void setTyreSlip(float slip);
    /// The car's world position and orientation (columns: right, up,
    /// forward), for spatial engines.
    void setTransform(const glm::vec3& position, const glm::mat3& basis);
    void setVolume(float volume);

    /// Equal-power crossfade weights of `loops` (sorted by rpm_sample) at
    /// `rpm`: a loop fades in across the overlap of its range with the
    /// previous loop's and out across the overlap with the next one's; the
    /// weights' squares sum to 1. Outside every range the nearest loop plays
    /// alone.
    static std::vector<float> loopWeights(std::span<const EngineLoop> loops, float rpm);

    /// Whether the rev limiter is cutting at the moment, for tools.
    bool limiterActive() const { return m_limiter; }

private:
    /// One recorded loop. It holds a mixer voice only while its crossfade
    /// weight is above zero: a car has dozens of loops but hears two or
    /// three per emitter at any rpm, and a field of cars would otherwise
    /// exhaust the mixer's voices.
    struct LoopVoice {
        EngineLoop loop;
        std::shared_ptr<const PcmBuffer> pcm;
        VoiceSettings settings;
        VoiceId voice = kNoVoice;
    };
    struct Emitter {
        std::string name;
        EmissionGroup group = EmissionGroup::Exhaust;
        BusId bus = kMasterBus;
        std::vector<LoopVoice> loops;
        /// The loops again, contiguous, for loopWeights().
        std::vector<EngineLoop> loopTable;
        const EmissionDsp* dsp = nullptr;
        float viewVolume = 1.0F;
        float pan = 0.0F;
        glm::vec3 localPosition{0.0F};
        Rolloff rolloff;
    };
    struct OneShotTimer {
        float remaining = 0.0F;
        bool armed = false;
    };

    void build();
    /// Starts the loops of the harmonic tuning's `name` list (EngineIntake,
    /// EngineAmbient, ExhaustL or ExhaustR), the emitter of the same name.
    void addEmitter(EmissionGroup group, const std::string& name);
    void startEvents();
    EngineInputs inputs(float rpm, float throttle) const;
    void updateBurbles(const EngineInputs& in, float rpm, float dt);
    void playBurble(std::uint32_t index, float volume);
    float emitterVolume(const std::string& name) const;
    float otherVolume(const std::string& name) const;

    Mixer& m_mixer;
    std::shared_ptr<AudioLibrary> m_library;
    EngineSoundOptions m_options;
    CarAudioTuning m_tuning;
    std::vector<Emitter> m_emitters;
    std::string m_burbleBank;
    const BurbleSettings* m_burble = nullptr;
    std::unique_ptr<EventInstance> m_skid;
    std::unique_ptr<EventInstance> m_shift;
    std::mt19937 m_random;
    glm::vec3 m_position{0.0F};
    glm::mat3 m_basis{1.0F};
    float m_volume = 1.0F;
    float m_throttle = 0.0F;
    float m_limiterClock = 0.0F;
    bool m_limiter = false;
    float m_shiftBoost = 0.0F;
    float m_shiftBoostRate = 0.0F;
    float m_slip = 0.0F;
    OneShotTimer m_burbleTimer;
    OneShotTimer m_backfireTimer;
    bool m_backfireQueued = false;
};

} // namespace fh::audio
