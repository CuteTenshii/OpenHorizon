// Inspects and plays the game's audio: lists and exports FMOD sound banks,
// decodes every sample on the disc as a check, lists and plays FMOD Designer
// events, and renders or plays a car's engine sound.

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/Decode.h"
#include "audio/EngineSound.h"
#include "audio/EventPlayer.h"
#include "audio/Fsb.h"
#include "audio/Wav.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <algorithm>
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
#include <vector>

namespace {

using namespace fh::audio;

/// Engine updates per second in renders and live playback, a game frame.
constexpr int kUpdateRate = 60;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1audio DISC list [FOLDER]\n"
        "       fh1audio DISC export BANK OUTDIR\n"
        "       fh1audio DISC verify [FOLDER]\n"
        "       fh1audio DISC events PROJECT\n"
        "       fh1audio DISC event PROJECT EVENT [--wav OUT] [--seconds N] [--param NAME=VALUE]...\n"
        "       fh1audio DISC engine CAR --wav OUT [--seconds N] [--view VIEW] [--upgrade LEVEL]\n"
        "       fh1audio DISC play CAR [--seconds N] [--view VIEW] [--upgrade LEVEL]\n"
        "\n"
        "engine and play sweep the rpm from idle to the redline at full throttle,\n"
        "hold it on the limiter, and let it fall back to idle with the throttle shut.\n");
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

std::vector<std::string> banksUnder(const fh::Vfs& vfs, const std::string& folder)
{
    std::vector<std::string> banks;
    for (const std::string& path : vfs.list(folder)) {
        if (fh::endsWithNoCase(path, ".fsb")) {
            banks.push_back(path);
        }
    }
    return banks;
}

int listBanks(const fh::Vfs& vfs, const std::string& folder)
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

int exportBank(const fh::Vfs& vfs, const std::string& path, const std::filesystem::path& outDir)
{
    const FsbBank bank = FsbBank::parse(vfs.readOrThrow(path), path);
    std::filesystem::create_directories(outDir);
    for (std::size_t i = 0; i < bank.size(); ++i) {
        const FsbSample& sample = bank.sample(i);
        std::string stem = sample.name;
        if (fh::endsWithNoCase(stem, ".wav")) {
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
int verifyBanks(const fh::Vfs& vfs, const std::string& folder)
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
                fh::logInfo("verify: {} of {} banks", done, banks.size());
            }
        }
    };
    std::vector<std::thread> threads;
    const unsigned count = std::max(1U, std::thread::hardware_concurrency());
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
        fh::logError("{} has no event '{}'", projectPath, eventPath);
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

int runEngine(const fh::Vfs& vfs, const std::string& car, const Options& options, bool live)
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
        engine.update(rpm, throttle, 0.0F, dt);
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

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    try {
        fh::Vfs vfs;
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
        if (command == "event" && argc >= 5) {
            const std::optional<Options> options = Options::parse(argc, argv, 5);
            return options ? playEvent(library, argv[3], argv[4], *options) : usage();
        }
        if ((command == "engine" || command == "play") && argc >= 4) {
            const std::optional<Options> options = Options::parse(argc, argv, 4);
            return options ? runEngine(vfs, argv[3], *options, command == "play") : usage();
        }
        return usage();
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
