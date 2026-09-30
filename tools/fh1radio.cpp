// Inspects the in-game radio: lists its stations and what they play, dumps a
// track, ident or DJ line to WAV, prints what a station would play over some
// minutes (offline, from the sequencer alone), and renders a station through
// an offline mixer.

#include "audio/AudioDevice.h"
#include "audio/Wav.h"
#include "base/Log.h"
#include "radio/Radio.h"
#include "radio/RadioAssets.h"
#include "radio/RadioSequencer.h"
#include "radio/RadioText.h"
#include "vfs/Vfs.h"

#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace openhorizon::radio;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1radio DISC stations [--lang LANG]\n"
        "       fh1radio DISC tracks STATION\n"
        "       fh1radio DISC wav NAME OUT [--vo LANG]\n"
        "       fh1radio DISC simulate STATION MINUTES [--seed N] [--vo LANG]\n"
        "       fh1radio DISC render STATION SECONDS OUT [--seed N] [--vo LANG]\n"
        "\n"
        "STATION is a name from RadioSystem.xml (Radio1) or an index. NAME is a\n"
        "music or voice sound name (R1_BlueMonday, R1_Ident_01). LANG is a voice\n"
        "bank language (EN, DE, ES, FR, IT, NL) or a string table folder (en).\n");
    return 2;
}

std::optional<std::map<std::string, std::string>> parseOptions(int argc, char** argv, int first)
{
    std::map<std::string, std::string> options;
    for (int i = first; i < argc; i += 2) {
        const std::string name = argv[i];
        if (!name.starts_with("--") || i + 1 >= argc) {
            return std::nullopt;
        }
        options[name.substr(2)] = argv[i + 1];
    }
    return options;
}

std::string option(const std::map<std::string, std::string>& options, const char* name, const char* fallback)
{
    const auto it = options.find(name);
    return it == options.end() ? fallback : it->second;
}

std::size_t stationIndex(const RadioSystemData& system, const std::string& text)
{
    if (const std::optional<std::size_t> found = system.findStation(text)) {
        return *found;
    }
    char* end = nullptr;
    const unsigned long index = std::strtoul(text.c_str(), &end, 10);
    if (end != text.c_str() && *end == '\0' && index < system.stations.size()) {
        return index;
    }
    throw openhorizon::DataError("no station '{}'", text);
}

std::string clock(std::uint64_t frame)
{
    const std::uint64_t ms = frame * 1000 / RadioSequencer::kOutputRate;
    return std::format("{:02}:{:02}.{:03}", ms / 60000, ms / 1000 % 60, ms % 1000);
}

int stations(const openhorizon::Vfs& vfs, const std::map<std::string, std::string>& options)
{
    const auto assets = RadioAssets::load(vfs);
    const RadioSystemData& system = assets->catalog()->system;
    const std::size_t off = system.offStation();
    const RadioText text(vfs, option(options, "lang", "en"), off + 1, off);
    std::printf("%-22s %-20s %6s %6s %7s %7s  %s\n", "station", "display name", "tracks", "idents", "DJ", "volume",
        "kind");
    for (std::size_t i = 0; i < system.stations.size(); ++i) {
        const Station& s = system.stations[i];
        const char* kind = i == off ? "off (tunable)" : (i < off ? "tunable" : "positional");
        std::printf("%-22s %-20s %6zu %6zu %7zu %7.2f  %s\n", s.name.c_str(), text.stationName(i).c_str(),
            s.playlist.tracks.size(), s.idents.lines.size(), s.regular.lines.size(), static_cast<double>(s.volume),
            kind);
    }
    std::printf("HUD text for the off station: %s\n", text.radioOff().c_str());
    return 0;
}

int tracks(const openhorizon::Vfs& vfs, const std::string& stationText)
{
    const auto assets = RadioAssets::load(vfs);
    const RadioCatalog& c = *assets->catalog();
    const Station& s = c.system.stations[stationIndex(c.system, stationText)];
    std::printf("%s: %zu tracks (file noRepeat %d, the game uses %zu)\n", s.name.c_str(), s.playlist.tracks.size(),
        s.playlist.noRepeatAttribute, s.playlist.tracks.empty() ? 0 : s.playlist.tracks.size() - 1);
    std::printf("  %-32s %-5s %9s %9s %9s %9s  %s\n", "fmodName", "index", "songStart", "eventStart", "identStart",
        "length", "title / artist");
    for (const MusicTrack& t : s.playlist.tracks) {
        const auto info = c.musicInfo.find(t.fmodName);
        if (info == c.musicInfo.end()) {
            std::printf("  %-32s (no sound bank info)\n", t.fmodName.c_str());
            continue;
        }
        const SoundBankInfo& i = info->second;
        std::printf("  %-32s %-5d %9.3f %9.3f %9.3f %9.3f  %s / %s\n", t.fmodName.c_str(), i.soundBankIndex,
            i.offsetSongStart / 1000.0, i.offsetEventStart / 1000.0, i.offsetIdentStart / 1000.0,
            i.fileLength / 1000.0, t.name.c_str(), t.artist.c_str());
    }
    const auto lines = [&](const char* what, const VoiceList& list) {
        std::printf("%s: %zu (noRepeat %d)\n", what, list.lines.size(), list.noRepeat);
        for (const VoiceLine& l : list.lines) {
            const auto info = c.voiceInfo.find(l.fmodName);
            std::printf("  %-60s group %-3d %8.3f s\n", l.fmodName.c_str(), l.group,
                info == c.voiceInfo.end() ? 0.0 : info->second.fileLength / 1000.0);
        }
    };
    lines("idents", s.idents);
    lines("regular DJ lines", s.regular);
    std::printf("special DJ lines: %zu, immediate: %zu, festival update lead-ins: %zu, lead-outs: %zu\n",
        s.special.size(), s.immediate.size(), s.festivalLeadIn.lines.size(), s.festivalLeadOut.lines.size());
    return 0;
}

int wav(const openhorizon::Vfs& vfs, const std::string& name, const std::string& out,
    const std::map<std::string, std::string>& options)
{
    const auto assets = RadioAssets::load(vfs, option(options, "vo", "EN"));
    const RadioCatalog& c = *assets->catalog();
    RadioBank bank = RadioBank::Music;
    auto info = c.musicInfo.find(name);
    if (info == c.musicInfo.end()) {
        bank = RadioBank::Voice;
        info = c.voiceInfo.find(name);
        if (info == c.voiceInfo.end()) {
            throw openhorizon::DataError("no radio sound named '{}'", name);
        }
    }
    openhorizon::audio::MpegSampleStream stream = assets->openStream(bank, info->second.soundBankIndex, 0);
    openhorizon::audio::PcmBuffer pcm;
    pcm.sampleRate = stream.sampleRate();
    pcm.channels = stream.channels();
    while (!stream.finished()) {
        stream.decode(1 << 16, pcm.samples);
    }
    pcm.loopEnd = pcm.frames();
    openhorizon::audio::writeWav(out, pcm, openhorizon::audio::WavFormat::Pcm16);
    std::printf("%s: %zu frames at %d Hz, %d channels -> %s\n", name.c_str(), pcm.frames(), pcm.sampleRate,
        pcm.channels, out.c_str());
    return 0;
}

int simulate(const openhorizon::Vfs& vfs, const std::string& stationText, double minutes,
    const std::map<std::string, std::string>& options)
{
    const auto assets = RadioAssets::load(vfs, option(options, "vo", "EN"));
    const std::uint32_t seed = static_cast<std::uint32_t>(std::strtoul(option(options, "seed", "0").c_str(),
        nullptr, 10));
    RadioSequencer sequencer(assets->catalog(), seed);
    sequencer.selectStation(stationIndex(assets->catalog()->system, stationText), false);
    sequencer.startFreeRoam();
    const auto total = static_cast<std::uint64_t>(minutes * 60.0 * RadioSequencer::kOutputRate);
    constexpr std::size_t kStep = Radio::kUpdateFrames;
    std::size_t sinceUpdate = 0;
    const auto print = [&] {
        for (const RadioEvent& e : sequencer.takeEvents()) {
            const std::string station = sequencer.catalog().system.stations[e.station].name;
            switch (e.kind) {
            case RadioEvent::Kind::TrackStarted:
                std::printf("%s %-8s track    %-30s at %7.3f s  \"%s\" by %s\n", clock(e.frame).c_str(),
                    station.c_str(), e.name.c_str(), e.startMs / 1000.0, e.title.c_str(), e.artist.c_str());
                break;
            case RadioEvent::Kind::TrackEnded:
                std::printf("%s %-8s end      %s\n", clock(e.frame).c_str(), station.c_str(), e.name.c_str());
                break;
            case RadioEvent::Kind::IdentStarted:
                std::printf("%s %-8s ident    %s\n", clock(e.frame).c_str(), station.c_str(), e.name.c_str());
                break;
            case RadioEvent::Kind::DialogueStarted:
                std::printf("%s %-8s DJ       %s\n", clock(e.frame).c_str(), station.c_str(), e.name.c_str());
                break;
            case RadioEvent::Kind::DialogueEnded:
                std::printf("%s %-8s DJ done  %s\n", clock(e.frame).c_str(), station.c_str(), e.name.c_str());
                break;
            case RadioEvent::Kind::StationChanged:
                std::printf("%s %-8s station\n", clock(e.frame).c_str(), station.c_str());
                break;
            case RadioEvent::Kind::HudShow:
                std::printf("%s %-8s HUD      after %.1f s: %s\n", clock(e.frame).c_str(), station.c_str(),
                    static_cast<double>(e.delaySeconds), e.title.empty() ? "(station only)" : e.title.c_str());
                break;
            case RadioEvent::Kind::Paused:
            case RadioEvent::Kind::Resumed:
            case RadioEvent::Kind::Stopped:
                std::printf("%s %-8s %s\n", clock(e.frame).c_str(), station.c_str(),
                    e.kind == RadioEvent::Kind::Paused ? "paused" : (e.kind == RadioEvent::Kind::Resumed ? "resumed"
                                                                                                        : "stopped"));
                break;
            }
        }
    };
    print();
    while (sequencer.frame() < total) {
        const std::size_t until = sequencer.framesUntilEvent();
        const std::size_t step = std::min<std::size_t>({kStep - sinceUpdate, until,
            static_cast<std::size_t>(total - sequencer.frame())});
        sequencer.advance(step);
        sinceUpdate += step;
        if (sinceUpdate == kStep) {
            sinceUpdate = 0;
            sequencer.update(static_cast<float>(kStep) / RadioSequencer::kOutputRate, 0.0F);
        }
        print();
    }
    return 0;
}

int render(const openhorizon::Vfs& vfs, const std::string& stationText, double seconds, const std::string& out,
    const std::map<std::string, std::string>& options)
{
    openhorizon::audio::AudioDevice device{openhorizon::audio::AudioDevice::Offline{}};
    Radio::Options radioOptions;
    radioOptions.voiceLanguage = option(options, "vo", "EN");
    radioOptions.seed = static_cast<std::uint32_t>(std::strtoul(option(options, "seed", "0").c_str(), nullptr, 10));
    radioOptions.worker = false;
    Radio radio(vfs, device, radioOptions);
    radio.selectStation(stationIndex(radio.assets().catalog()->system, stationText));
    radio.startFreeRoam();
    openhorizon::audio::PcmBuffer pcm;
    pcm.sampleRate = openhorizon::audio::AudioDevice::kSampleRate;
    pcm.channels = openhorizon::audio::AudioDevice::kChannels;
    const auto total = static_cast<std::size_t>(seconds * openhorizon::audio::AudioDevice::kSampleRate);
    constexpr std::size_t kFrame = openhorizon::audio::AudioDevice::kSampleRate / 60;
    for (std::size_t done = 0; done < total; done += kFrame) {
        const std::vector<float> block = device.render(std::min(kFrame, total - done));
        pcm.samples.insert(pcm.samples.end(), block.begin(), block.end());
        radio.update(1.0F / 60.0F);
        for (const RadioEvent& e : radio.takeEvents()) {
            if (e.kind == RadioEvent::Kind::TrackStarted || e.kind == RadioEvent::Kind::IdentStarted
                || e.kind == RadioEvent::Kind::DialogueStarted) {
                std::printf("%s %s\n", clock(e.frame).c_str(), e.name.c_str());
            }
        }
        if (auto hud = radio.takeHudRequest()) {
            std::printf("%s HUD: %s  %s / %s\n", clock(done).c_str(), hud->stationName.c_str(), hud->title.c_str(),
                hud->artist.c_str());
        }
    }
    pcm.loopEnd = pcm.frames();
    openhorizon::audio::writeWav(out, pcm, openhorizon::audio::WavFormat::Pcm16);
    std::printf("wrote %s\n", out.c_str());
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
        if (command == "stations") {
            const auto options = parseOptions(argc, argv, 3);
            return options ? stations(vfs, *options) : usage();
        }
        if (command == "tracks" && argc == 4) {
            return tracks(vfs, argv[3]);
        }
        if (command == "wav" && argc >= 5) {
            const auto options = parseOptions(argc, argv, 5);
            return options ? wav(vfs, argv[3], argv[4], *options) : usage();
        }
        if (command == "simulate" && argc >= 5) {
            const auto options = parseOptions(argc, argv, 5);
            return options ? simulate(vfs, argv[3], std::strtod(argv[4], nullptr), *options) : usage();
        }
        if (command == "render" && argc >= 6) {
            const auto options = parseOptions(argc, argv, 6);
            return options ? render(vfs, argv[3], std::strtod(argv[4], nullptr), argv[5], *options) : usage();
        }
        return usage();
    } catch (const std::exception& e) {
        openhorizon::logError("{}", e.what());
        return 1;
    }
}
