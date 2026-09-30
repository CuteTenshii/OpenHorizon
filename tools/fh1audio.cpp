// Inspects and plays the game's audio: lists and exports FMOD sound banks,
// decodes every sample on the disc as a check, lists and plays FMOD Designer
// events, and renders or plays a car's engine and tyre sounds.

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/Decode.h"
#include "audio/EngineSound.h"
#include "audio/EventPlayer.h"
#include "audio/Fsb.h"
#include "audio/TyreSound.h"
#include "audio/Wav.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace openhorizon::audio;

/// Engine updates per second in renders and live playback, a game frame.
constexpr int kUpdateRate = 60;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1audio DISC list [FOLDER]\n"
        "       fh1audio DISC export BANK OUTDIR\n"
        "       fh1audio DISC verify [FOLDER]\n"
        "       fh1audio DISC events PROJECT\n"
        "       fh1audio DISC describe PROJECT EVENT\n"
        "       fh1audio DISC event PROJECT EVENT [--wav OUT] [--seconds N] [--param NAME=VALUE]...\n"
        "       fh1audio DISC engine CAR --wav OUT [--seconds N] [--view VIEW] [--upgrade LEVEL]\n"
        "       fh1audio DISC play CAR [--seconds N] [--view VIEW] [--upgrade LEVEL]\n"
        "       fh1audio DISC tyres --wav OUT [--surface NAME] [--seconds N] [--lod2 1]\n"
        "\n"
        "engine and play sweep the rpm from idle to the redline at full throttle,\n"
        "hold it on the limiter, and let it fall back to idle with the throttle shut.\n"
        "tyres plays four wheels on a surfaceTypes.xml surface (default Asphalt)\n"
        "through a scripted drive and prints each stage's level.\n");
    return 2;
}

/// The options after the positional arguments: "--name value" pairs, with
/// "--param" allowed more than once.
struct Options {
    std::map<std::string, std::string> values;
    std::vector<std::pair<std::string, float>> parameters;

    static std::optional<Options> parse(int argc, char** argv, int first)
    {
        Options options;
        for (int i = first; i < argc; i += 2) {
            const std::string name = argv[i];
            if (!name.starts_with("--") || i + 1 >= argc) {
                return std::nullopt;
            }
            const std::string value = argv[i + 1];
            if (name == "--param") {
                const std::size_t equals = value.find('=');
                if (equals == std::string::npos) {
                    return std::nullopt;
                }
                options.parameters.emplace_back(value.substr(0, equals), std::strtof(value.c_str() + equals + 1,
                    nullptr));
            } else {
                options.values[name.substr(2)] = value;
            }
        }
        return options;
    }

    std::optional<std::string> get(const std::string& name) const
    {
        const auto it = values.find(name);
        return it == values.end() ? std::nullopt : std::optional<std::string>(it->second);
    }

    float number(const std::string& name, float fallback) const
    {
        const std::optional<std::string> text = get(name);
        return text ? std::strtof(text->c_str(), nullptr) : fallback;
    }
};

std::vector<std::string> banksUnder(const openhorizon::Vfs& vfs, const std::string& folder)
{
    std::vector<std::string> banks;
    for (const std::string& path : vfs.list(folder)) {
        if (openhorizon::endsWithNoCase(path, ".fsb")) {
            banks.push_back(path);
        }
    }
    return banks;
}

int listBanks(const openhorizon::Vfs& vfs, const std::string& folder)
{
    std::map<std::string, std::size_t> totals;
    std::size_t samples = 0;
    const std::vector<std::string> banks = banksUnder(vfs, folder);
    for (const std::string& path : banks) {
        const FsbBank bank = FsbBank::parse(vfs.readOrThrow(path), path);
        std::map<std::string, std::size_t> codecs;
        for (const FsbSample& sample : bank.samples()) {
            ++codecs[codecName(sample.codec)];
            ++totals[codecName(sample.codec)];
        }
        std::string codecText;
        for (const auto& [name, count] : codecs) {
            codecText += std::format("{}{} {}", codecText.empty() ? "" : ", ", count, name);
        }
        std::printf("%-72s %5zu samples  %s\n", path.c_str(), bank.size(), codecText.c_str());
        samples += bank.size();
    }
    std::printf("%zu banks, %zu samples:", banks.size(), samples);
    for (const auto& [name, count] : totals) {
        std::printf(" %zu %s", count, name.c_str());
    }
    std::printf("\n");
    return 0;
}

int exportBank(const openhorizon::Vfs& vfs, const std::string& path, const std::filesystem::path& outDir)
{
    const FsbBank bank = FsbBank::parse(vfs.readOrThrow(path), path);
    std::filesystem::create_directories(outDir);
    for (std::size_t i = 0; i < bank.size(); ++i) {
        const FsbSample& sample = bank.sample(i);
        std::string stem = sample.name;
        if (openhorizon::endsWithNoCase(stem, ".wav")) {
            stem.resize(stem.size() - 4);
        }
        std::replace_if(stem.begin(), stem.end(), [](char c) { return c == '/' || c == '\\' || c == ':'; }, '_');
        const std::filesystem::path file = outDir / std::format("{:04}_{}.wav", i, stem);
        writeWav(file, decodeSample(bank, i), WavFormat::Pcm16);
        std::printf("%s\n", std::format("{}  {} Hz  {} ch  {} frames  {}", file.string(), sample.frequency,
            sample.channels, sample.sampleCount, codecName(sample.codec)).c_str());
    }
    return 0;
}

struct CodecTally {
    std::size_t samples = 0;
    std::size_t decoded = 0;
    std::size_t trimmed = 0;
    std::uint64_t frames = 0;
    double seconds = 0.0;
};

/// Decodes every sample of every bank under `folder` on all cores and fails
/// if any sample does not decode to its header's length.
int verifyBanks(const openhorizon::Vfs& vfs, const std::string& folder)
{
    const std::vector<std::string> banks = banksUnder(vfs, folder);
    std::mutex mutex;
    std::map<std::string, CodecTally> tallies;
    std::vector<std::string> errors;
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> finished{0};
    const auto work = [&] {
        while (true) {
            const std::size_t b = next++;
            if (b >= banks.size()) {
                return;
            }
            std::map<std::string, CodecTally> local;
            std::vector<std::string> localErrors;
            try {
                const FsbBank bank = FsbBank::parse(vfs.readOrThrow(banks[b]), banks[b]);
                for (std::size_t i = 0; i < bank.size(); ++i) {
                    const FsbSample& sample = bank.sample(i);
                    CodecTally& tally = local[codecName(sample.codec)];
                    ++tally.samples;
                    try {
                        DecodeReport report;
                        const PcmBuffer pcm = decodeSample(bank, i, report);
                        ++tally.decoded;
                        tally.trimmed += report.codecFrames > report.headerFrames ? 1 : 0;
                        tally.frames += pcm.frames();
                        tally.seconds += static_cast<double>(pcm.frames()) / pcm.sampleRate;
                    } catch (const std::exception& e) {
                        localErrors.emplace_back(e.what());
                    }
                }
            } catch (const std::exception& e) {
                localErrors.emplace_back(e.what());
            }
            std::lock_guard lock(mutex);
            for (const auto& [name, tally] : local) {
                CodecTally& total = tallies[name];
                total.samples += tally.samples;
                total.decoded += tally.decoded;
                total.trimmed += tally.trimmed;
                total.frames += tally.frames;
                total.seconds += tally.seconds;
            }
            errors.insert(errors.end(), localErrors.begin(), localErrors.end());
            const std::size_t done = ++finished;
            if (done % 100 == 0 || done == banks.size()) {
                openhorizon::logInfo("verify: {} of {} banks", done, banks.size());
            }
        }
    };
    std::vector<std::thread> threads;
    const unsigned count = std::max(1U, std::thread::hardware_concurrency());
    threads.reserve(count);
    for (unsigned t = 0; t < count; ++t) {
        threads.emplace_back(work);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }

    std::size_t samples = 0;
    std::size_t decoded = 0;
    std::printf("%zu banks\n", banks.size());
    for (const auto& [name, tally] : tallies) {
        std::printf("%-10s %6zu samples, %6zu decoded (%zu longer than their header, trimmed), %llu frames, "
                    "%.1f hours\n",
            name.c_str(), tally.samples, tally.decoded, tally.trimmed, static_cast<unsigned long long>(tally.frames),
            tally.seconds / 3600.0);
        samples += tally.samples;
        decoded += tally.decoded;
    }
    std::printf("total      %6zu samples, %6zu decoded, %zu errors\n", samples, decoded, errors.size());
    for (const std::string& error : errors) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
    }
    return errors.empty() && decoded == samples ? 0 : 1;
}

int listEvents(AudioLibrary& library, const std::string& projectPath)
{
    const std::shared_ptr<const FevProject> project = library.project(projectPath);
    std::printf("%s\n", std::format("project {} (FEV version 0x{:08x}), banks:", project->name(),
        project->version()).c_str());
    for (const FevBank& bank : project->banks()) {
        std::printf("  %s\n", AudioLibrary::bankPathFor(projectPath, bank.name).c_str());
    }
    for (const FevGroup& group : project->groups()) {
        for (const FevUserProperty& property : group.userProperties) {
            std::visit([&](const auto& value) {
                std::printf("%s\n", std::format("group {}: {} = {}", group.path, property.name, value).c_str());
            }, property.value);
        }
    }
    for (const FevEvent& event : project->events()) {
        std::string parameters;
        for (const FevParameter& parameter : event.parameters) {
            parameters += std::format("{}{} {}..{}", parameters.empty() ? "" : ", ", parameter.name,
                parameter.minimum, parameter.maximum);
        }
        std::size_t sounds = 0;
        std::string first;
        for (const FevLayer& layer : event.layers) {
            for (const FevSoundInstance& sound : layer.sounds) {
                if (sounds++ == 0) {
                    first = project->soundDefinition(sound.soundDefinition).name;
                }
            }
        }
        std::printf("%s\n", std::format("{:<56} {:<7} {:>2} layers {:>2} sounds {:<28} {}", event.path,
            event.simple ? "simple" : "complex", event.layers.size(), sounds, first, parameters).c_str());
    }
    std::size_t waveforms = 0;
    std::size_t wavetables = 0;
    for (const FevSoundDefinition& definition : project->soundDefinitions()) {
        waveforms += definition.waveforms.size();
        for (const FevWaveform& waveform : definition.waveforms) {
            wavetables += waveform.type == FevWaveform::Type::Wavetable ? 1 : 0;
        }
    }
    std::printf("%zu events, %zu sound definitions, %zu waveforms (%zu wavetables)\n", project->events().size(),
        project->soundDefinitions().size(), waveforms, wavetables);
    return 0;
}

int describeEvent(AudioLibrary& library, const std::string& projectPath, const std::string& eventPath)
{
    const std::shared_ptr<const FevProject> project = library.project(projectPath);
    const FevEvent* event = project->findEvent(eventPath);
    if (!event) {
        openhorizon::logError("{} has no event '{}'", projectPath, eventPath);
        return 1;
    }
    std::printf("%s\n", std::format("{}: volume {} pitch {} priority {} max playbacks {} mode 0x{:x} distance {}..{}",
        event->path, event->volume, event->pitch, event->priority, event->maxPlaybacks, event->mode,
        event->minDistance, event->maxDistance).c_str());
    for (std::size_t i = 0; i < event->parameters.size(); ++i) {
        const FevParameter& parameter = event->parameters[i];
        std::printf("%s\n", std::format("  parameter {} {}: {}..{} velocity {} flags 0x{:x} seek speed {}", i,
            parameter.name, parameter.minimum, parameter.maximum, parameter.velocity, parameter.flags,
            parameter.seekSpeed).c_str());
    }
    for (const FevUserProperty& property : event->userProperties) {
        std::visit([&](const auto& value) {
            std::printf("%s\n", std::format("  user property {} = {}", property.name, value).c_str());
        }, property.value);
    }
    for (const std::string& category : event->categories) {
        std::printf("  category %s\n", category.c_str());
    }
    for (std::size_t l = 0; l < event->layers.size(); ++l) {
        const FevLayer& layer = event->layers[l];
        const std::string control = layer.controlParameter < event->parameters.size()
            ? event->parameters[layer.controlParameter].name
            : std::string("none");
        std::printf("%s\n", std::format("  layer {}: flags 0x{:x} priority {} follows {}", l, layer.flags,
            layer.priority, control).c_str());
        for (const FevSoundInstance& sound : layer.sounds) {
            const FevSoundDefinition& definition = project->soundDefinition(sound.soundDefinition);
            std::printf("%s\n", std::format("    sound {} at {:.4f} length {:.4f}{} ({} waveforms)", definition.name,
                sound.start, sound.length, sound.oneShot ? " one-shot" : "", definition.waveforms.size()).c_str());
        }
        for (const FevEnvelope& envelope : layer.envelopes) {
            const std::string follows = envelope.parameter < event->parameters.size()
                ? event->parameters[envelope.parameter].name
                : std::string("?");
            std::string points;
            for (const FevEnvelopePoint& point : envelope.points) {
                points += std::format(" ({:.4f}, {:.4f}{})", point.position, point.value,
                    point.curveShape != 0 ? std::format(" shape {}", point.curveShape) : std::string());
            }
            std::printf("%s\n", std::format("    envelope property 0x{:x} dsp '{}' #{} parent {} follows {}:{}",
                envelope.property, envelope.dspName, envelope.dspParameter, envelope.parent, follows, points)
                .c_str());
        }
    }
    return 0;
}

/// Collects rendered output until it is written as a WAV.
struct Recording {
    std::vector<float> samples;

    void append(const std::vector<float>& block) { samples.insert(samples.end(), block.begin(), block.end()); }

    void write(const std::filesystem::path& path) const
    {
        PcmBuffer pcm;
        pcm.sampleRate = AudioDevice::kSampleRate;
        pcm.channels = AudioDevice::kChannels;
        pcm.samples = samples;
        writeWav(path, pcm, WavFormat::Float32);
        float peak = 0.0F;
        double energy = 0.0;
        for (const float s : samples) {
            peak = std::max(peak, std::abs(s));
            energy += static_cast<double>(s) * s;
        }
        const double rms = samples.empty() ? 0.0 : std::sqrt(energy / static_cast<double>(samples.size()));
        std::printf("%s\n", std::format("wrote {}: {:.2f} s, peak {:.3f}, rms {:.3f}", path.string(),
            static_cast<double>(samples.size()) / (2.0 * AudioDevice::kSampleRate), peak, rms).c_str());
    }
};

int playEvent(AudioLibrary& library, const std::string& projectPath, const std::string& eventPath,
    const Options& options)
{
    const std::shared_ptr<const FevProject> project = library.project(projectPath);
    const FevEvent* event = project->findEvent(eventPath);
    if (!event) {
        openhorizon::logError("{} has no event '{}'", projectPath, eventPath);
        return 1;
    }
    const std::optional<std::string> wav = options.get("wav");
    const float seconds = options.number("seconds", 5.0F);
    std::optional<AudioDevice> device;
    if (wav) {
        device.emplace(AudioDevice::Offline{});
    } else {
        device.emplace();
    }
    EventInstance instance(device->mixer(), library, projectPath, project, *event);
    for (const auto& [name, value] : options.parameters) {
        instance.setParameter(name, value);
    }
    instance.start();
    Recording recording;
    const int updates = static_cast<int>(seconds * kUpdateRate);
    for (int i = 0; i < updates; ++i) {
        if (wav) {
            recording.append(device->render(AudioDevice::kSampleRate / kUpdateRate));
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(1000000 / kUpdateRate));
        }
    }
    instance.stop();
    if (wav) {
        recording.write(*wav);
    }
    return 0;
}

/// Engine speed and throttle at time `t` of a `length`-second sweep: up to
/// the redline at full throttle, on the limiter, then back to idle closed.
std::pair<float, float> sweepAt(float t, float length, float idle, float redline)
{
    const float rise = 0.45F * length;
    const float hold = 0.10F * length;
    if (t < rise) {
        return {idle + (redline - idle) * t / rise, 1.0F};
    }
    if (t < rise + hold) {
        return {redline, 1.0F};
    }
    const float fall = std::min(1.0F, (t - rise - hold) / (length - rise - hold));
    return {redline - (redline - idle) * fall, 0.0F};
}

int runEngine(const openhorizon::Vfs& vfs, const std::string& car, const Options& options, bool live)
{
    const std::optional<std::string> wav = options.get("wav");
    if (!live && !wav) {
        return usage();
    }
    const float seconds = options.number("seconds", 10.0F);
    EngineSoundOptions settings;
    settings.view = options.get("view").value_or("Follow");
    settings.intakeUpgrade = static_cast<int>(options.number("upgrade", 0.0F));
    settings.exhaustUpgrade = settings.intakeUpgrade;

    std::optional<AudioDevice> device;
    if (live) {
        device.emplace();
    } else {
        device.emplace(AudioDevice::Offline{});
    }
    EngineSound engine(*device, vfs, car, settings);
    const float idle = engine.tuning().engine.idleRpm;
    const float redline = engine.tuning().engine.redlineRpm;
    std::printf("%s\n", std::format("{}: idle {} rpm, redline {} rpm, {} cylinders", car, idle, redline,
        engine.tuning().engine.cylinders).c_str());

    Recording recording;
    const int updates = static_cast<int>(seconds * kUpdateRate);
    const float dt = 1.0F / kUpdateRate;
    auto next = std::chrono::steady_clock::now();
    for (int i = 0; i <= updates; ++i) {
        const float t = static_cast<float>(i) * dt;
        const auto [rpm, throttle] = sweepAt(t, seconds, idle, redline);
        engine.update(rpm, throttle, dt);
        if (live) {
            next += std::chrono::microseconds(1000000 / kUpdateRate);
            std::this_thread::sleep_until(next);
        } else {
            recording.append(device->render(AudioDevice::kSampleRate / kUpdateRate));
        }
    }
    if (wav) {
        recording.write(*wav);
    }
    return 0;
}

/// One stage of the tyre script: every wheel in the same state. The values
/// are test inputs in TyreInput's units, chosen to visit each part of the
/// events' curves, not measurements of the game.
struct TyreStage {
    const char* name;
    bool onGround;
    float spin;
    float slipAngle;
    float slipRatio;
    float combinedSlip;
};

int runTyres(const openhorizon::Vfs& vfs, const Options& options)
{
    const std::optional<std::string> wav = options.get("wav");
    if (!wav) {
        return usage();
    }
    const float seconds = std::max(options.number("seconds", 3.0F), 0.1F);
    const std::string surface = options.get("surface").value_or("Asphalt");
    AudioDevice device{AudioDevice::Offline{}};
    TyreSoundOptions settings;
    settings.perWheel = options.number("lod2", 0.0F) == 0.0F;
    TyreSound tyres(device.mixer(), std::make_shared<AudioLibrary>(vfs), settings);
    const TyreSmoothing& smoothing = tyres.smoothing();
    std::printf("%s\n", std::format("smoothing per second: combined slip {}, slip angle {}, slip ratio {}, load {}, "
        "rpm {}; leaves the ground after {} s", smoothing.combinedSlipRate, smoothing.slipAngleRate,
        smoothing.slipRatioRate, smoothing.normLoadRate, smoothing.tireRpmRate, smoothing.timeUntilLeavesGround)
        .c_str());
    if (!tyres.surfaces().find(surface)) {
        std::printf("%s is not in Tires.xml, so the game plays no tyre sound on it\n", surface.c_str());
    }

    constexpr std::array<TyreStage, 7> kStages{{
        {"standing", true, 0.0F, 0.0F, 0.0F, 0.0F},
        {"rolling", true, 60.0F, 0.05F, 0.05F, 0.07F},
        {"cornering", true, 60.0F, 0.8F, 0.05F, 0.8F},
        {"sliding", true, 50.0F, 3.0F, -0.5F, 3.0F},
        {"locked", true, 0.0F, 0.2F, -5.0F, 5.0F},
        {"spinning", true, 90.0F, 0.1F, 3.0F, 3.0F},
        {"airborne", false, 60.0F, 0.0F, 0.0F, 0.0F},
    }};
    constexpr float kMass = 1500.0F;
    Recording recording;
    const int updates = static_cast<int>(seconds * kUpdateRate);
    for (const TyreStage& stage : kStages) {
        std::array<TyreInput, TyreSound::kWheels> wheels{};
        for (TyreInput& wheel : wheels) {
            wheel.onGround = stage.onGround;
            wheel.surface = surface;
            wheel.spin = stage.spin;
            wheel.slipAngle = stage.slipAngle;
            wheel.slipRatio = stage.slipRatio;
            wheel.combinedSlip = stage.combinedSlip;
            wheel.load = kMass * 9.80665F / 4.0F;
        }
        TyreCarInput car;
        car.mass = kMass;
        double energy = 0.0;
        std::size_t count = 0;
        for (int i = 0; i < updates; ++i) {
            tyres.update(wheels, car, 1.0F / kUpdateRate);
            const std::vector<float> block = device.render(AudioDevice::kSampleRate / kUpdateRate);
            for (const float sample : block) {
                energy += static_cast<double>(sample) * sample;
            }
            count += block.size();
            recording.append(block);
        }
        std::printf("%s\n", std::format("{:<10} rms {:.4f}{}", stage.name,
            std::sqrt(energy / static_cast<double>(std::max<std::size_t>(count, 1))),
            tyres.isPlaying() ? "" : " (no event sounding)").c_str());
    }
    recording.write(*wav);
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    try {
        openhorizon::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string command = argv[2];
        if (command == "list") {
            return listBanks(vfs, argc > 3 ? argv[3] : "media");
        }
        if (command == "export" && argc == 5) {
            return exportBank(vfs, argv[3], argv[4]);
        }
        if (command == "verify") {
            return verifyBanks(vfs, argc > 3 ? argv[3] : "media");
        }
        AudioLibrary library(vfs);
        if (command == "events" && argc == 4) {
            return listEvents(library, argv[3]);
        }
        if (command == "describe" && argc == 5) {
            return describeEvent(library, argv[3], argv[4]);
        }
        if (command == "event" && argc >= 5) {
            const std::optional<Options> options = Options::parse(argc, argv, 5);
            return options ? playEvent(library, argv[3], argv[4], *options) : usage();
        }
        if (command == "tyres") {
            const std::optional<Options> options = Options::parse(argc, argv, 3);
            return options ? runTyres(vfs, *options) : usage();
        }
        if ((command == "engine" || command == "play") && argc >= 4) {
            const std::optional<Options> options = Options::parse(argc, argv, 4);
            return options ? runEngine(vfs, argv[3], *options, command == "play") : usage();
        }
        return usage();
    } catch (const std::exception& e) {
        openhorizon::logError("{}", e.what());
        return 1;
    }
}
