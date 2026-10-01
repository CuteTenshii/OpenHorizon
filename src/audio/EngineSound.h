#pragma once

#include "EngineTuning.h"
#include "Mixer.h"
#include "TyreSound.h"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <memory>
#include <random>
#include <span>
#include <string>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::audio {

class AudioDevice;
class AudioLibrary;
class EventInstance;

struct EngineSoundOptions {
    /// 0 (stock) to 3; picks the recordings and the backfire behaviour.
    int intakeUpgrade = 0;
    int exhaustUpgrade = 0;
    /// "Follow" (chase camera), "Front" (bumper), "Cockpit" or "Default".
    std::string view = "Follow";
    /// Placed with setTransform() and attenuated by GlobalRolloffs.xml; otherwise stereo, panned by the
    /// view's surround levels.
    bool spatial = false;
    /// From Tires.fev and Transmissions.fev.
    bool tyres = true;
    bool transmission = true;
    struct Buses {
        BusId engine = kMasterBus;
        BusId backfire = kMasterBus;
        BusId transmission = kMasterBus;
        BusId tyres = kMasterBus;
    };
    Buses buses;
    std::uint32_t seed = 1;
};

/// The engine, exhaust, burble, gear-shift and tyre sounds of one car. Each loop plays at pitch
/// rpm / rpm_sample; there are no on-load and off-load recordings, so load acts only through the
/// tunings' volume curves and EQ (EmissionDsp). The rev limiter cuts in 35 ms pulses.
class EngineSound {
public:
    /// Slow: decodes the car's sounds. Throws DataError if the car has no engine tuning.
    EngineSound(AudioDevice& device, const Vfs& vfs, const std::string& carMedia, EngineSoundOptions options = {});
    /// As above, sharing decoded samples with other sounds.
    EngineSound(AudioDevice& device, std::shared_ptr<AudioLibrary> library, const std::string& carMedia,
        EngineSoundOptions options = {});
    ~EngineSound();
    EngineSound(const EngineSound&) = delete;
    EngineSound& operator=(const EngineSound&) = delete;

    const CarAudioTuning& tuning() const { return m_tuning; }

    /// `throttle` 0..1.
    void update(float rpm, float throttle, float dt);
    /// An upshift also gives the exhaust a chance to backfire.
    void shiftGear(int fromGear, int toGear);
    /// Wheels in CarPhysics order.
    void updateTyres(std::span<const TyreInput, 4> wheels, const TyreCarInput& car, float dt);
    /// Null when the options leave tyres out or Tires.fev failed to load.
    const TyreSound* tyres() const { return m_tyres.get(); }
    /// `basis` columns: right, up, forward.
    void setTransform(const glm::vec3& position, const glm::mat3& basis);
    void setVolume(float volume);
    /// From the loudest emitter bus to the master (Mixer::busGain); 0 without emitters.
    float engineGain() const;

    /// Equal-power crossfade across neighbouring loops' rpm overlaps; `loops` sorted by rpm_sample.
    /// Outside every range the nearest loop plays alone.
    static std::vector<float> loopWeights(std::span<const EngineLoop> loops, float rpm);

    /// Whether the rev limiter is cutting at the moment, for tools.
    bool limiterActive() const { return m_limiter; }

private:
    /// Holds a mixer voice only while its weight is above zero, or a field of cars runs out of voices.
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
    /// `name`: EngineIntake, EngineAmbient, ExhaustL or ExhaustR.
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
    std::unique_ptr<TyreSound> m_tyres;
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
    OneShotTimer m_burbleTimer;
    OneShotTimer m_backfireTimer;
    bool m_backfireQueued = false;
};

} // namespace openhorizon::audio
