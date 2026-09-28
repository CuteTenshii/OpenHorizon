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

namespace fh {
class Vfs;
}

namespace fh::audio {

/// The four physics values every audio curve of the tunings reads, each
/// normalised to 0..1.
struct EngineInputs {
    /// (rpm - idle) / (redline - idle).
    float rpm = 0.0F;
    float throttle = 0.0F;
    /// Load while the engine drives the car.
    float posTorque = 0.0F;
    /// Load while the car drives the engine (engine braking).
    float negTorque = 0.0F;
};

/// A `<PhysicsCoeff RPM= Throttle= PosTorque= NegTorque=/>` weighting plus a
/// `<ThreePointCurve x0= y0= x1= y1= x2= y2=/>`, the pair every tuning value
/// is written as. The weighted sum of the inputs, clamped to 0..1, is looked
/// up piecewise linearly on the three points and held flat outside them
/// (inferred: coefficients sum to about 1 and curve x values span 0..1; a
/// curve whose weights are all 0 therefore always yields y0).
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

/// One recorded engine loop of a harmonic tuning: plays at its natural pitch
/// at `rpmSample` and is audible between `rpmMin` and `rpmMax`, where its
/// neighbours take over.
struct EngineLoop {
    std::uint32_t sampleIndex = 0;
    float rpmMin = 0.0F;
    float rpmSample = 1000.0F;
    float rpmMax = 0.0F;
    float volume = 1.0F;
    /// Always 1 on the disc, as are the pitchMin and pitchMax attributes,
    /// which are therefore not read.
    float pitch = 1.0F;
};

/// The DSP chain the tunings put on one emission group, FMOD effect
/// parameters as curves (see FilterSpec for the units).
struct EmissionDsp {
    struct Peq {
        bool active = false;
        TuningCurve gain;
        TuningCurve center;
        TuningCurve bandwidth;
    };
    TuningCurve volume;
    Peq peq;
    /// Exhaust only: EQ bands that follow positive and negative load; the
    /// recordings have no separate on-load and off-load loops, so these and
    /// the volume curves are what make load audible.
    Peq posLoad;
    Peq negLoad;
    bool lowpassActive = false;
    TuningCurve lowpassCutoff;
    TuningCurve lowpassResonance;

    /// The chain's filters at the given inputs.
    std::vector<FilterSpec> filters(const EngineInputs& in) const;
};

/// Random one-shots on the overrun: `burbles` samples at the start of the
/// burble bank, then `backfires` more.
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

/// A harmonic tuning (`media/audio/cars/engines/harmonictuning/*_ht.xml`):
/// one bank of engine loops, as `<Soundbank name="X.fsb">` with `<Loop>`
/// lists under EngineIntake, EngineAmbient, ExhaustL and ExhaustR (the two
/// exhaust lists index different samples of the same bank), plus the bank's
/// own DSP and, for exhausts, burbles. Verified on all 2756 files.
struct HarmonicTuning {
    std::string bank;
    std::map<std::string, std::vector<EngineLoop>, std::less<>> loops;
    EmissionDsp dsp;
    std::optional<BurbleSettings> burble;
    float exhaustNoiseVolume = 0.0F;

    static HarmonicTuning parse(std::string_view xml, const std::string& source);
};

enum class EmissionGroup { Intake = 0, Ambient = 1, Exhaust = 2 };

/// A car's engine tuning (`media/audio/cars/engines/enginetuning/<media>_et.xml`,
/// named after the car's MediaName in lower case; 807 files, 685 of them
/// with per-car HarmonicTunings DSP; the archive's 808th file is a
/// misplaced car model tuning). Upgrade files are harmonic tuning names per
/// upgrade level 0..3; "NA.xml" (or an "NA_..." tuning of the missing bank
/// NA.fsb) means the level has no sound of its own. Race cars such as the
/// DBR9 have only level 3.
struct EngineTuning {
    float idleRpm = 800.0F;
    float redlineRpm = 7000.0F;
    /// As written, which is often not the engine's (4 on the F430, 2 on the
    /// Civic Type R); the recordings are what carry the firing order.
    int cylinders = 0;
    float engineVolume = 1.0F;
    float transmissionVolume = 1.0F;
    float shiftVolume = 1.0F;
    float backfireVolume = 1.0F;
    float burbleVolume = 1.0F;
    TuningCurve globalVolume;
    std::array<std::array<std::string, 4>, 3> upgrades;
    /// Car-specific replacements for the harmonic tunings' DSP, per
    /// emission group (`<HarmonicTunings><EmissionGroupN>`, N = the
    /// EmissionGroup value, inferred from the ALF_8C_08 groups repeating the
    /// intake, ambient and exhaust tunings' DSP blocks value for value).
    std::array<std::optional<EmissionDsp>, 3> groupDsp;
    /// Upshift and downshift volume boost in percent and its duration in
    /// seconds.
    float shiftBoostUp = 0.0F;
    float shiftBoostUpTime = 0.0F;
    float shiftBoostDown = 0.0F;
    float shiftBoostDownTime = 0.0F;

    static EngineTuning parse(std::string_view xml, const std::string& source);
    /// The harmonic tuning file for `group` at `level`, falling back to lower
    /// levels where a level says "NA", then to higher ones; empty if none.
    std::string upgradeFile(EmissionGroup group, int level) const;
};

/// Per-car listener mix from `media/audio/cars/carmodeltuning/<media>_cmt.xml`:
/// where each emitter sits on the car and, per camera view (Front, Follow,
/// Cockpit, Default, ...), the volume of each emitter and of the car's
/// other sounds.
struct CarModelTuning {
    struct View {
        std::map<std::string, float, std::less<>> emitterVolumes;
        std::map<std::string, float, std::less<>> otherVolumes;
        /// The Left, Right and Center levels of each emitter's surround pan.
        std::map<std::string, glm::vec3, std::less<>> surroundPan;

        /// A stereo pan (-1..1) for `emitter` from its surround levels, with
        /// the centre speaker folded into both sides at -3 dB.
        float stereoPan(std::string_view emitter) const;
    };
    /// Car space, metres: x left to right (the left exhaust has negative
    /// x), y up, z forward.
    std::map<std::string, glm::vec3, std::less<>> emitterPositions;
    std::map<std::string, View, std::less<>> views;

    static CarModelTuning parse(std::string_view xml, const std::string& source);
    const View* view(std::string_view name) const;
};

/// `media/audio/GlobalRolloffs.xml`: the Gameplay rolloff of each car
/// emitter (EngineIntake, EngineAmbient, ExhaustL, ExhaustR under Chassis)
/// and of IntakeChuff, Burble and Backfire.
std::map<std::string, Rolloff, std::less<>> parseGlobalRolloffs(std::string_view xml, const std::string& source);

/// `media/audio/BackfireTimerConfig.xml` and `BackfireVolumeConfig.xml`:
/// four entries each, which by count match the exhaust upgrade levels
/// (inferred; level 0 never fires). Timer times are milliseconds and rpm
/// values normalised.
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

    /// Loads the car's tunings at the given upgrade levels. Throws DataError
    /// if the car has no engine tuning.
    static CarAudioTuning load(const Vfs& vfs, std::string_view carMedia, int intakeLevel = 0, int exhaustLevel = 0);
};

/// What a harmonic tuning's wavebank index plays. Indices count channels,
/// not samples: in a bank of n samples, index i < n is sample i and index
/// n + i is the second channel of sample i. That is how the right exhaust of
/// a stereo exhaust bank is addressed (ExhaustL 0..n-1, ExhaustR n..2n-1 on
/// 636 of the 651 banks with both; inferred from the counts, as the bank has
/// exactly n stereo samples). An index past 2n is invalid (5 tunings).
struct LoopSource {
    std::size_t sample = 0;
    /// The channel to play, or -1 for all of them.
    int channel = 0;
};
std::optional<LoopSource> resolveLoopIndex(std::uint32_t wavebankIndex, std::size_t bankSamples,
    int sampleChannels);

/// Game path of a bank a harmonic tuning names: engine loops live in
/// `media/audio/cars/engines/soundbanks/lod1`, burbles in `.../burbles`.
/// Empty if neither has it (two tunings on the disc name missing banks).
std::string engineBankPath(const Vfs& vfs, std::string_view bankName);

} // namespace fh::audio
