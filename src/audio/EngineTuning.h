#pragma once

#include "Mixer.h"

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::audio {

/// What every tuning curve reads, each normalised to 0..1.
struct EngineInputs {
    /// (rpm - idle) / (redline - idle).
    float rpm = 0.0F;
    float throttle = 0.0F;
    /// Load while the engine drives the car.
    float posTorque = 0.0F;
    /// Load while the car drives the engine (engine braking).
    float negTorque = 0.0F;
};

/// `<PhysicsCoeff>` weights plus a `<ThreePointCurve>`: the clamped weighted sum of the inputs, looked up
/// piecewise linearly and held flat outside the points. Inferred: weights sum to about 1, x spans 0..1;
/// all-zero weights always yield y0.
struct TuningCurve {
    float rpm = 0.0F;
    float throttle = 0.0F;
    float posTorque = 0.0F;
    float negTorque = 0.0F;
    std::array<glm::vec2, 3> points{glm::vec2{0.0F, 1.0F}, glm::vec2{0.5F, 1.0F}, glm::vec2{1.0F, 1.0F}};

    float input(const EngineInputs& in) const;
    float evaluate(const EngineInputs& in) const;
    float at(float x) const;
};

/// Natural pitch at `rpmSample`, audible between `rpmMin` and `rpmMax`.
struct EngineLoop {
    std::uint32_t sampleIndex = 0;
    float rpmMin = 0.0F;
    float rpmSample = 1000.0F;
    float rpmMax = 0.0F;
    float volume = 1.0F;
    /// Always 1 on the disc, as are pitchMin and pitchMax, which are not read.
    float pitch = 1.0F;
};

/// One emission group's DSP chain, FMOD effect parameters as curves (FilterSpec has the units).
struct EmissionDsp {
    struct Peq {
        bool active = false;
        TuningCurve gain;
        TuningCurve center;
        TuningCurve bandwidth;
    };
    TuningCurve volume;
    Peq peq;
    /// Exhaust only: EQ bands that follow positive and negative load.
    Peq posLoad;
    Peq negLoad;
    bool lowpassActive = false;
    TuningCurve lowpassCutoff;
    TuningCurve lowpassResonance;

    /// The chain's filters at the given inputs.
    std::vector<FilterSpec> filters(const EngineInputs& in) const;
};

/// Overrun one-shots: `burbles` samples at the start of the burble bank, then `backfires` more.
struct BurbleSettings {
    std::string bank;
    std::uint32_t burbles = 0;
    std::uint32_t backfires = 0;
    TuningCurve volume;
    /// Milliseconds between attempts, and the rpm window they happen in.
    float minTime = 0.0F;
    float maxTime = 0.0F;
    float minRpm = 0.0F;
    float maxRpm = 0.0F;
    /// Percent of attempts that stay silent.
    int silenceWeighting = 100;
};

/// `media/audio/cars/engines/harmonictuning/*_ht.xml`: one bank's `<Loop>` lists per emitter (ExhaustL and
/// ExhaustR index different samples), its DSP and, for exhausts, burbles. Verified on all 2756 files.
struct HarmonicTuning {
    std::string bank;
    std::map<std::string, std::vector<EngineLoop>, std::less<>> loops;
    EmissionDsp dsp;
    std::optional<BurbleSettings> burble;
    float exhaustNoiseVolume = 0.0F;

    static HarmonicTuning parse(std::string_view xml, const std::string& source);
};

enum class EmissionGroup { Intake = 0, Ambient = 1, Exhaust = 2 };

/// `media/audio/cars/engines/enginetuning/<media>_et.xml`, MediaName in lower case (807 files, 685 with
/// per-car DSP). Upgrades are harmonic tuning names per level 0..3; "NA.xml" (or an "NA_..." tuning of the
/// missing NA.fsb) means no sound of its own. Race cars such as the DBR9 have only level 3.
struct EngineTuning {
    float idleRpm = 800.0F;
    float redlineRpm = 7000.0F;
    /// As written, often not the engine's (4 on the F430, 2 on the Civic Type R).
    int cylinders = 0;
    float engineVolume = 1.0F;
    float transmissionVolume = 1.0F;
    float shiftVolume = 1.0F;
    float backfireVolume = 1.0F;
    float burbleVolume = 1.0F;
    TuningCurve globalVolume;
    std::array<std::array<std::string, 4>, 3> upgrades;
    /// `<HarmonicTunings><EmissionGroupN>` overrides; N = EmissionGroup is inferred from ALF_8C_08.
    std::array<std::optional<EmissionDsp>, 3> groupDsp;
    /// Percent, with its duration in seconds.
    float shiftBoostUp = 0.0F;
    float shiftBoostUpTime = 0.0F;
    float shiftBoostDown = 0.0F;
    float shiftBoostDownTime = 0.0F;

    static EngineTuning parse(std::string_view xml, const std::string& source);
    /// Falls back past "NA" levels to lower ones, then higher; empty if none.
    std::string upgradeFile(EmissionGroup group, int level) const;
};

/// `media/audio/cars/carmodeltuning/<media>_cmt.xml`: emitter positions and per-view volumes.
struct CarModelTuning {
    struct View {
        std::map<std::string, float, std::less<>> emitterVolumes;
        std::map<std::string, float, std::less<>> otherVolumes;
        /// The Left, Right and Center levels of each emitter's surround pan.
        std::map<std::string, glm::vec3, std::less<>> surroundPan;

        /// -1..1, the centre folded into both sides at -3 dB.
        float stereoPan(std::string_view emitter) const;
    };
    /// Car space, metres (the left exhaust has negative x).
    std::map<std::string, glm::vec3, std::less<>> emitterPositions;
    std::map<std::string, View, std::less<>> views;

    static CarModelTuning parse(std::string_view xml, const std::string& source);
    const View* view(std::string_view name) const;
};

/// `media/audio/GlobalRolloffs.xml`: the Gameplay rolloffs of the car emitters (under Chassis), IntakeChuff,
/// Burble and Backfire.
std::map<std::string, Rolloff, std::less<>> parseGlobalRolloffs(std::string_view xml, const std::string& source);

/// `media/audio/BackfireTimerConfig.xml` and `BackfireVolumeConfig.xml`: four entries, one per exhaust
/// upgrade level (inferred; level 0 never fires). Times in milliseconds, rpm normalised.
struct BackfireLevel {
    float minTime = 0.0F;
    float maxTime = 0.0F;
    float minRpm = 0.0F;
    float maxRpm = 0.0F;
    int silenceWeighting = 100;
    TuningCurve volume;
};
std::vector<BackfireLevel> parseBackfireConfig(std::string_view timerXml, std::string_view volumeXml,
    const std::string& source);

/// Everything the engine sound of one car needs, loaded through the VFS.
struct CarAudioTuning {
    std::string media;
    EngineTuning engine;
    CarModelTuning model;
    std::array<std::optional<HarmonicTuning>, 3> harmonics;
    std::array<std::string, 3> harmonicFiles;
    std::map<std::string, Rolloff, std::less<>> rolloffs;
    std::vector<BackfireLevel> backfires;

    /// Throws DataError if the car has no engine tuning.
    static CarAudioTuning load(const Vfs& vfs, std::string_view carMedia, int intakeLevel = 0, int exhaustLevel = 0);
};

/// Wavebank indices count channels: in a bank of n samples, n + i is sample i's second channel (inferred:
/// ExhaustR uses n..2n-1 on 636 of the 651 banks with both). An index past 2n is invalid (5 tunings).
struct LoopSource {
    std::size_t sample = 0;
    /// The channel to play, or -1 for all of them.
    int channel = 0;
};
std::optional<LoopSource> resolveLoopIndex(std::uint32_t wavebankIndex, std::size_t bankSamples,
    int sampleChannels);

/// Looks in `media/audio/cars/engines/soundbanks/lod1` and `.../burbles`; empty if neither has it (two
/// tunings on the disc name missing banks).
std::string engineBankPath(const Vfs& vfs, std::string_view bankName);

} // namespace openhorizon::audio
