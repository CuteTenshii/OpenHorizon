// Tests of the radio: the string key hash, the game's random generator, the
// RadioSystem and sound bank info readers, the sequencer's flow on a small
// synthetic station set, progressive MPEG decoding of an encoded tone,
// sync points in FSB headers and the mixer's stream voices; and, when
// FH1_GAME_DIR names an extracted disc, the real radio data, its banks and
// a mixed render.

#include "TestHarness.h"

#include "audio/AudioDevice.h"
#include "audio/Decode.h"
#include "audio/Fsb.h"
#include "audio/Mixer.h"
#include "radio/Radio.h"
#include "radio/RadioAssets.h"
#include "radio/RadioData.h"
#include "radio/RadioRandom.h"
#include "radio/RadioSequencer.h"
#include "radio/RadioText.h"
#include "vfs/Vfs.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <numbers>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace fh::radio;
using fh::Bytes;

constexpr int kRate = RadioSequencer::kOutputRate;

TEST(stringIdKeysMatchTheStringTables)
{
    // Keys of the texts as found in the disc's myprofile.str and ingame.str.
    CHECK_EQ(stringIdKey("IDS_RadioOff"), 19192);
    CHECK_EQ(stringIdKey("IDS_RadioStation_Option_1"), 41630);
    CHECK_EQ(stringIdKey("IDS_RadioStation_Option_2"), 41758);
    CHECK_EQ(stringIdKey("IDS_RadioStation_Option_3"), 41886);
    CHECK_EQ(stringIdKey("IDS_RadioStation_Option_Off"), 4222);
    CHECK_EQ(stringIdKey("ids_radiooff"), stringIdKey("IDS_RADIOOFF"));
}

TEST(randomFollowsTheGamesGenerator)
{
    // Bits 16 to 30 of seed * 0x41C64E6D + 12345, from seed 12345.
    RadioRandom random(12345);
    CHECK_EQ(random.next(), 21468);
    CHECK_EQ(random.next(), 9988);
    CHECK_EQ(random.next(), 22117);
    std::set<int> seen;
    for (int i = 0; i < 2000; ++i) {
        const int v = random.integer(0, 4);
        CHECK(v >= 0 && v <= 4);
        seen.insert(v);
        const float f = random.range(2.0F, 3.0F);
        CHECK(f >= 2.0F && f <= 3.0F);
    }
    CHECK_EQ(seen.size(), std::size_t{5});
}

constexpr const char* kSystemXml = R"(<?xml version="1.0"?>
<RadioSystem>
  <System fsbMusic="Music" fsbVO="VO" maxTracksBetweenDJ="1" duckValue="0.3" musicLevel="0.85"
     identLevel="0.3" HUDNewTrackDisplayDelayFreeRoam="2.0"/>
  <RadioStation name="RadioB">
    <Playlist noRepeat="15">
      <MusicTrack name="Two" artist="B" fmodname="B_Two" likelihoodDaytime="1" likelihoodMorning="1"
         likelihoodEvening="1" likelihoodNight="1"/>
      <MusicTrack name="One" artist="A" fmodName="B_One" likelihoodDaytime="1" likelihoodMorning="1"
         likelihoodEvening="1" likelihoodNight="1"/>
      <MusicTrack name="Three" artist="C" fmodname="B_Three" likelihoodDaytime="1" likelihoodMorning="1"
         likelihoodEvening="1" likelihoodNight="1"/>
    </Playlist>
    <Idents noRepeat="1"><Ident fmodName="B_Ident_1"/><Ident fmodName="B_Ident_2"/></Idents>
    <DJRegularDialogue noRepeat="1">
      <SimpleDialogue fmodName="B_DJ_1" group="0"/>
      <SimpleDialogue fmodName="B_DJ_2" group="0"/>
      <SimpleDialogue fmodName="B_Hint" group="20"/>
    </DJRegularDialogue>
    <DJSpecialDialogue><SpecialDialogue gameplayEvent="Event" fmodName="B_DJ_1"/></DJSpecialDialogue>
  </RadioStation>
  <RadioStation name="RadioA">
    <Playlist noRepeat="2">
      <MusicTrack name="Solo" artist="S" fmodname="A_Solo" likelihoodDaytime="1" likelihoodMorning="1"
         likelihoodEvening="1" likelihoodNight="1"/>
      <MusicTrack name="Duet" artist="D" fmodname="A_Duet" likelihoodDaytime="1" likelihoodMorning="1"
         likelihoodEvening="1" likelihoodNight="1"/>
    </Playlist>
    <Idents noRepeat="1"><Ident fmodName="A_Ident"/></Idents>
    <DJRegularDialogue noRepeat="1"><SimpleDialogue fmodName="A_DJ" group="0"/></DJRegularDialogue>
  </RadioStation>
  <RadioStation name="Radio4_Silent" isOffStation="true">
    <Playlist noRepeat="1">
      <MusicTrack name="None" artist="None" fmodName="Blank" likelihoodDaytime="1" likelihoodMorning="1"
         likelihoodEvening="1" likelihoodNight="1"/>
    </Playlist>
    <Idents noRepeat="1"><Ident fmodName="Ident_Blank"/></Idents>
  </RadioStation>
  <RadioStation name="Radio5_3d" volume="0.35" 3dPanLevel="0.5" musicLoops="false"/>
  <FestivalUpdates><FestivalUpdate gameplayEvent="Update" fmodName="B_DJ_2"/></FestivalUpdates>
</RadioSystem>
)";

TEST(radioSystemParsesAndSortsStations)
{
    const RadioSystemData data = parseRadioSystem(kSystemXml, "test.xml");
    CHECK_EQ(data.stations.size(), std::size_t{4});
    CHECK_EQ(data.stations[0].name, std::string("Radio4_Silent"));
    CHECK_EQ(data.stations[1].name, std::string("Radio5_3d"));
    CHECK_EQ(data.stations[2].name, std::string("RadioA"));
    CHECK_EQ(data.stations[3].name, std::string("RadioB"));
    CHECK_EQ(data.offStation(), std::size_t{0});
    CHECK_EQ(data.settings.maxTracksBetweenDJ, 1);
    CHECK_NEAR(data.settings.duckValue, 0.3, 1e-6);
    // Unset attributes keep the game's defaults.
    CHECK_NEAR(data.settings.levelFadeDownTime, 3.0, 1e-6);
    const Station& b = data.stations[3];
    CHECK_EQ(b.playlist.tracks.size(), std::size_t{3});
    CHECK_EQ(b.playlist.tracks[0].fmodName, std::string("B_Two"));
    CHECK_EQ(b.playlist.tracks[1].fmodName, std::string("B_One"));
    CHECK_EQ(b.playlist.noRepeatAttribute, 15);
    CHECK_EQ(b.regular.lines[2].group, 20);
    CHECK_EQ(b.special.size(), std::size_t{1});
    CHECK_EQ(b.special[0].gameplayEvent, std::string("Event"));
    CHECK_NEAR(data.stations[1].panLevel3d, 0.5, 1e-6);
    CHECK_NEAR(data.stations[1].volume, 0.35, 1e-6);
    CHECK_EQ(data.festivalUpdates.size(), std::size_t{1});
    CHECK_THROWS(parseRadioSystem("<Other/>", "bad.xml"));
}

TEST(soundBankInfoParses)
{
    const SoundBankInfoMap info = parseSoundBankInfo(R"(<SoundBanks>
  <Bank Name="R1_Test"><SoundBankInfo SoundBankIndex="46" OffsetEventStart="83050" OffsetIdentStart="450863"
    OffsetSongStart="20376" OffsetStartNextTrack="0" FileLength="455616" SyncpointIndexIdentStart="2"
    SyncpointStartNextTrack="-1"/></Bank>
</SoundBanks>)",
        "info.xml");
    CHECK_EQ(info.size(), std::size_t{1});
    const SoundBankInfo& i = info.at("R1_Test");
    CHECK_EQ(i.soundBankIndex, 46);
    CHECK_EQ(i.offsetEventStart, 83050U);
    CHECK_EQ(i.offsetIdentStart, 450863U);
    CHECK_EQ(i.offsetSongStart, 20376U);
    CHECK_EQ(i.fileLength, 455616U);
    CHECK_EQ(i.syncpointIndexIdentStart, 2);
    CHECK_EQ(i.syncpointStartNextTrack, -1);
}

/// A catalog for kSystemXml: every track is 60 s with SongStart at 2 s,
/// EventStart at 10 s and IdentStart at 55 s; idents last 6 s with
/// StartNextTrack at 3 s; DJ lines last `djSeconds`.
std::shared_ptr<RadioCatalog> testCatalog(double djSeconds = 4.0)
{
    auto c = std::make_shared<RadioCatalog>();
    c->system = parseRadioSystem(kSystemXml, "test.xml");
    const auto addMusic = [&](const std::string& name) {
        SoundBankInfo i;
        i.soundBankIndex = static_cast<int>(c->musicSamples.size());
        i.offsetSongStart = 2000;
        i.offsetEventStart = 10000;
        i.offsetIdentStart = 55000;
        i.fileLength = 60000;
        i.syncpointIndexIdentStart = 2;
        c->musicInfo[name] = i;
        c->musicSamples.push_back({kRate, 60U * kRate, {2U * kRate, 10U * kRate, 55U * kRate}});
    };
    const auto addVoice = [&](const std::string& name, double seconds, bool ident) {
        SoundBankInfo i;
        i.soundBankIndex = static_cast<int>(c->voiceSamples.size());
        i.fileLength = static_cast<std::uint32_t>(seconds * 1000.0);
        SampleTiming t{kRate, static_cast<std::uint32_t>(seconds * kRate), {}};
        if (ident) {
            i.syncpointStartNextTrack = 0;
            i.offsetStartNextTrack = 3000;
            t.syncPoints.push_back(3U * kRate);
        }
        c->voiceInfo[name] = i;
        c->voiceSamples.push_back(t);
    };
    for (const char* name : {"B_One", "B_Two", "B_Three", "A_Solo", "A_Duet", "Blank"}) {
        addMusic(name);
    }
    for (const char* name : {"B_Ident_1", "B_Ident_2", "A_Ident", "Ident_Blank"}) {
        addVoice(name, 6.0, true);
    }
    for (const char* name : {"B_DJ_1", "B_DJ_2", "B_Hint", "A_DJ"}) {
        addVoice(name, djSeconds, false);
    }
    return c;
}

/// A catalog in the disc's shape: Radio1 (RadioA), Radio2 (RadioB), then the
/// off station and a positional one.
std::shared_ptr<RadioCatalog> tunableCatalog(double djSeconds = 4.0)
{
    auto c = testCatalog(djSeconds);
    for (Station& st : c->system.stations) {
        if (st.name == "RadioA") {
            st.name = "Radio1";
        } else if (st.name == "RadioB") {
            st.name = "Radio2";
        } else if (st.name == "Radio5_3d") {
            st.name = "Radio5_3d_PreRace";
        }
    }
    std::sort(c->system.stations.begin(), c->system.stations.end(),
        [](const Station& a, const Station& b) { return a.name < b.name; });
    return c;
}

/// Runs a sequencer for `seconds`, updating every 256 frames, and returns
/// its events.
std::vector<RadioEvent> run(RadioSequencer& s, double seconds)
{
    std::vector<RadioEvent> events;
    const auto end = s.frame() + static_cast<std::uint64_t>(seconds * kRate);
    std::size_t sinceUpdate = 0;
    while (s.frame() < end) {
        const std::size_t step = std::min<std::size_t>({256 - sinceUpdate, s.framesUntilEvent(),
            static_cast<std::size_t>(end - s.frame())});
        s.advance(step);
        sinceUpdate += step;
        if (sinceUpdate == 256) {
            sinceUpdate = 0;
            s.update(256.0F / kRate, 0.0F);
        }
        for (RadioEvent& e : s.takeEvents()) {
            events.push_back(std::move(e));
        }
    }
    return events;
}

std::vector<const RadioEvent*> ofKind(const std::vector<RadioEvent>& events, RadioEvent::Kind kind)
{
    std::vector<const RadioEvent*> out;
    for (const RadioEvent& e : events) {
        if (e.kind == kind) {
            out.push_back(&e);
        }
    }
    return out;
}

TEST(sequencerRunsTheFreeRoamFlow)
{
    RadioSequencer s(tunableCatalog(), 7);
    s.selectStation(1, false);
    s.startFreeRoam();
    const std::vector<RadioEvent> events = run(s, 300.0);
    const auto tracks = ofKind(events, RadioEvent::Kind::TrackStarted);
    const auto idents = ofKind(events, RadioEvent::Kind::IdentStarted);
    const auto lines = ofKind(events, RadioEvent::Kind::DialogueStarted);
    CHECK(tracks.size() >= 5);
    // The first track starts between 10% and 90% of SongStart..IdentStart.
    CHECK(tracks[0]->startMs >= 2000.0F + 0.1F * 53000.0F - 1.0F);
    CHECK(tracks[0]->startMs <= 55000.0F - 0.1F * 53000.0F + 1.0F);
    // Each later track starts 3 s after an ident, and each ident at a
    // track's IdentStart.
    for (std::size_t i = 1; i < tracks.size() && i - 1 < idents.size(); ++i) {
        CHECK_EQ(tracks[i]->frame, idents[i - 1]->frame + 3U * kRate);
    }
    CHECK(!idents.empty());
    CHECK_EQ(idents.empty() ? 0 : idents[0]->frame,
        static_cast<std::uint64_t>(std::llround((55000.0F - tracks[0]->startMs) * kRate / 1000.0F)));
    // One track per DJ line: every change after the first has a line, and
    // the track starts so the 4 s line ends by EventStart; the intro (8 s) is
    // long enough, so it starts at SongStart.
    CHECK_EQ(lines.size(), tracks.size() - 1);
    for (std::size_t i = 1; i < tracks.size() && i - 1 < lines.size(); ++i) {
        CHECK_NEAR(tracks[i]->startMs, 2000.0, 0.5);
        CHECK_EQ(lines[i - 1]->frame, tracks[i]->frame);
    }
    // No track repeats before the other two have played.
    for (std::size_t i = 0; i + 2 < tracks.size(); ++i) {
        std::set<std::string> window{tracks[i]->name, tracks[i + 1]->name, tracks[i + 2]->name};
        CHECK_EQ(window.size(), std::size_t{3});
    }
    // The group 20 line is on, so all three lines play in turn.
    std::set<std::string> said;
    for (const RadioEvent* e : lines) {
        said.insert(e->name);
    }
    CHECK_EQ(said.size(), std::size_t{3});
}

TEST(sequencerStartsTracksEarlyForLongDjLines)
{
    RadioSequencer s(tunableCatalog(12.0), 3);
    s.selectStation(1, false);
    s.startFreeRoam();
    const auto tracks = ofKind(run(s, 200.0), RadioEvent::Kind::TrackStarted);
    CHECK(tracks.size() >= 3);
    for (std::size_t i = 1; i < tracks.size(); ++i) {
        // EventStart 10 s minus the 12 s line, clamped at the start.
        CHECK_NEAR(tracks[i]->startMs, 0.0, 0.5);
    }
}

TEST(sequencerDialogueGroupsAndDjSwitch)
{
    RadioSequencer s(tunableCatalog(), 11);
    s.selectStation(1, false);
    s.setDialogueGroupEnabled(20, false);
    s.startFreeRoam();
    const auto lines = ofKind(run(s, 400.0), RadioEvent::Kind::DialogueStarted);
    CHECK(!lines.empty());
    for (const RadioEvent* e : lines) {
        CHECK(e->name != "B_Hint");
    }
    s.setDjDialogueEnabled(false);
    const auto later = run(s, 200.0);
    CHECK(ofKind(later, RadioEvent::Kind::DialogueStarted).empty());
    CHECK(!ofKind(later, RadioEvent::Kind::TrackStarted).empty());
}

TEST(sequencerTunesThroughStationsAndOff)
{
    RadioSequencer s(testCatalog(), 5);
    // Stations sort as Radio4_Silent, Radio5_3d, RadioA, RadioB: the off
    // station is first, so nothing is tuneable beyond it.
    CHECK_EQ(s.offStation(), std::size_t{0});
    CHECK_EQ(s.tunableStationCount(), std::size_t{1});
    s.startFreeRoam();
    CHECK(s.paused());
    s.nextStation();
    CHECK_EQ(s.currentStation(), std::size_t{0});
}

TEST(sequencerStationChangesResumeAndPause)
{
    RadioSequencer s(tunableCatalog(), 9);
    CHECK_EQ(s.offStation(), std::size_t{2});
    s.startFreeRoam();
    run(s, 20.0);
    const std::string first = s.voice(RadioChannel::Music).fmodName;
    CHECK(first == "A_Solo" || first == "A_Duet");
    CHECK_EQ(s.currentStation(), std::size_t{0});
    s.nextStation();
    CHECK_EQ(s.currentStation(), std::size_t{1});
    const auto events = run(s, 5.0);
    CHECK(!ofKind(events, RadioEvent::Kind::TrackStarted).empty());
    // Back to the first station: its track carries on where it would be.
    s.previousStation();
    const auto back = ofKind(s.takeEvents(), RadioEvent::Kind::TrackStarted);
    CHECK_EQ(back.size(), std::size_t{1});
    if (!back.empty()) {
        CHECK_EQ(back[0]->name, first);
    }
    // Off pauses everything and stops the clock.
    s.nextStation();
    s.nextStation();
    CHECK_EQ(s.currentStation(), std::size_t{2});
    run(s, 1.0);
    CHECK(s.paused());
    const std::uint64_t elapsed = s.voice(RadioChannel::Music).elapsed;
    run(s, 10.0);
    CHECK_EQ(s.voice(RadioChannel::Music).elapsed, elapsed);
    CHECK_NEAR(s.musicGain(), 0.0, 1e-6);
    // Past the off station the D-pad comes round to the first station, and
    // back from the first one to off.
    s.nextStation();
    CHECK_EQ(s.currentStation(), std::size_t{0});
    run(s, 1.0);
    CHECK(!s.paused());
    s.previousStation();
    CHECK_EQ(s.currentStation(), std::size_t{2});
}

TEST(sequencerDucksAndFadesLevels)
{
    RadioSequencer s(tunableCatalog(), 1);
    s.startFreeRoam();
    s.update(0.5F, 0.0F);
    // 2 s level fade-in by default, musicLevel 0.85.
    CHECK_NEAR(s.musicGain(), 0.85 * 0.25, 1e-4);
    s.update(2.0F, 0.0F);
    CHECK_NEAR(s.musicGain(), 0.85, 1e-4);
    CHECK_NEAR(s.identGain(), 0.3, 1e-4);
    s.setUserVolume(0.5F);
    s.update(0.01F, 0.0F);
    CHECK_NEAR(s.musicGain(), 0.425, 1e-4);
    s.setUserVolume(0.0F);
    s.update(0.01F, 0.0F);
    CHECK(s.paused());
    s.setUserVolume(1.0F);
    s.update(0.01F, 0.0F);
    CHECK(!s.paused());
}

/// Encodes `frames` of a stereo sine at 48 kHz as MPEG-1 layer II frames.
Bytes encodeTone(std::size_t frames)
{
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MP2);
    if (!codec) {
        return {};
    }
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    ctx->sample_rate = kRate;
    ctx->bit_rate = 192000;
    ctx->sample_fmt = AV_SAMPLE_FMT_S16;
    av_channel_layout_default(&ctx->ch_layout, 2);
    Bytes out;
    if (avcodec_open2(ctx, codec, nullptr) < 0) {
        avcodec_free_context(&ctx);
        return {};
    }
    AVFrame* frame = av_frame_alloc();
    AVPacket* packet = av_packet_alloc();
    const auto drain = [&] {
        while (avcodec_receive_packet(ctx, packet) == 0) {
            out.insert(out.end(), packet->data, packet->data + packet->size);
            av_packet_unref(packet);
        }
    };
    for (std::size_t done = 0; done < frames; done += static_cast<std::size_t>(ctx->frame_size)) {
        frame->nb_samples = ctx->frame_size;
        frame->format = ctx->sample_fmt;
        av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
        av_frame_get_buffer(frame, 0);
        auto* data = reinterpret_cast<std::int16_t*>(frame->data[0]);
        for (int i = 0; i < ctx->frame_size; ++i) {
            const double t = static_cast<double>(done + static_cast<std::size_t>(i)) / kRate;
            data[i * 2] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * 440.0 * t));
            data[i * 2 + 1] = static_cast<std::int16_t>(8000.0 * std::sin(2.0 * std::numbers::pi * 660.0 * t));
        }
        avcodec_send_frame(ctx, frame);
        drain();
        av_frame_unref(frame);
    }
    avcodec_send_frame(ctx, nullptr);
    drain();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);
    return out;
}

TEST(mpegStreamDecodesFromAnyFrame)
{
    const std::size_t mpegFrames = 200;
    const Bytes data = encodeTone(mpegFrames * 1152);
    if (data.empty()) {
        std::fprintf(stderr, "skipped: FFmpeg has no MP2 encoder\n");
        return;
    }
    fh::audio::FsbSample sample;
    sample.name = "tone";
    sample.codec = fh::audio::SampleCodec::Mpeg;
    sample.channels = 2;
    sample.frequency = kRate;
    sample.sampleCount = static_cast<std::uint32_t>(mpegFrames * 1152);
    // FSB pads frames; a gap of zeros between two frames must be skipped.
    Bytes padded(data.begin(), data.begin() + 576);
    padded.insert(padded.end(), 4, 0);
    padded.insert(padded.end(), data.begin() + 576, data.end());
    sample.compressedSize = static_cast<std::uint32_t>(padded.size());
    sample.dataOffset = 16;
    Bytes file(16 + padded.size(), 0xEE);
    std::copy(padded.begin(), padded.end(), file.begin() + 16);
    const fh::audio::ByteSource source = [&file](std::uint64_t offset, std::span<std::uint8_t> out) {
        const std::size_t n = offset >= file.size() ? 0 : std::min(out.size(), file.size() - offset);
        std::memcpy(out.data(), file.data() + offset, n);
        return n;
    };
    fh::audio::MpegSampleStream whole(sample, source, "tone");
    std::vector<float> all;
    while (!whole.finished()) {
        whole.decode(3000, all);
    }
    CHECK_EQ(all.size(), std::size_t{sample.sampleCount} * 2);
    for (const std::size_t start : {std::size_t{1}, std::size_t{1152 * 20 + 77}, std::size_t{1152 * 150}}) {
        fh::audio::MpegSampleStream part(sample, source, "tone", start);
        std::vector<float> got;
        part.decode(4000, got);
        CHECK(got.size() >= 4000 * 2);
        float worst = 0.0F;
        for (std::size_t i = 0; i < 4000 * 2; ++i) {
            worst = std::max(worst, std::abs(got[i] - all[start * 2 + i]));
        }
        CHECK(worst < 1e-4F);
    }
    CHECK_THROWS(fh::audio::MpegSampleStream(sample, source, "tone", sample.sampleCount + 1));
}

/// Builds an FSB4 bank with one MPEG sample that carries a SYNC chunk.
Bytes syncBank()
{
    Bytes out;
    const auto u16 = [&](std::uint32_t v) {
        out.push_back(static_cast<std::uint8_t>(v));
        out.push_back(static_cast<std::uint8_t>(v >> 8));
    };
    const auto u32 = [&](std::uint32_t v) {
        u16(v & 0xFFFF);
        u16(v >> 16);
    };
    const auto fixed = [&](std::string_view s, std::size_t size) {
        out.insert(out.end(), s.begin(), s.end());
        out.insert(out.end(), size - s.size(), 0);
    };
    const std::uint32_t headerSize = 0x50 + 8 + 2 * 260;
    fixed("FSB4", 4);
    u32(1);
    u32(headerSize);
    u32(64);
    u32(0x00040000);
    u32(0);
    out.insert(out.end(), 24, 0);
    u16(headerSize);
    fixed("song", 30);
    u32(96000);
    u32(64);
    u32(0);
    u32(95999);
    u32(0x80000200U | 0x40U);
    u32(48000);
    u16(255);
    u16(128);
    u16(128);
    u16(2);
    u32(0x3F800000);
    u32(0x461C4000);
    u32(0);
    u32(0);
    fixed("SYNC", 4);
    u32(2);
    u32(4800);
    fixed("SongStart", 256);
    u32(90000);
    fixed("IdentStart", 256);
    out.insert(out.end(), 64, 0);
    return out;
}

TEST(fsbReadsSyncPointsAndHeadersAlone)
{
    const Bytes file = syncBank();
    const fh::audio::FsbBank full = fh::audio::FsbBank::parse(file, "sync.fsb");
    const fh::audio::FsbSample& s = full.sample(0);
    CHECK_EQ(s.syncPoints.size(), std::size_t{2});
    CHECK_EQ(s.syncPoints[0].frame, 4800U);
    CHECK_EQ(s.syncPoints[0].name, std::string("SongStart"));
    CHECK_EQ(s.syncPoints[1].frame, 90000U);
    CHECK_EQ(s.syncPoints[1].name, std::string("IdentStart"));

    const std::size_t headerBytes = fh::audio::FsbBank::headersSize(file, "sync.fsb");
    CHECK_EQ(headerBytes, file.size() - 64);
    const fh::audio::FsbBank headers = fh::audio::FsbBank::parseHeaders(
        Bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(headerBytes)), file.size(), "sync.fsb");
    CHECK(!headers.hasData());
    CHECK_EQ(headers.sample(0).dataOffset, full.sample(0).dataOffset);
    CHECK_EQ(headers.sample(0).syncPoints.size(), std::size_t{2});
    CHECK_THROWS(headers.sampleData(0));
    CHECK_THROWS(fh::audio::FsbBank::parseHeaders(
        Bytes(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(headerBytes)), file.size() - 1, "short"));
}

class ConstantStream final : public fh::audio::PcmStream {
public:
    int channels() const override { return 2; }
    std::size_t read(float* out, std::size_t frames) override
    {
        const std::size_t n = std::min(frames, m_left);
        for (std::size_t i = 0; i < n; ++i) {
            out[i * 2] = 0.25F;
            out[i * 2 + 1] = -0.5F;
        }
        m_left -= n;
        return n;
    }
    bool finished() const override { return m_left == 0; }

private:
    std::size_t m_left = 1000;
};

TEST(mixerPlaysStreams)
{
    fh::audio::AudioDevice device{fh::audio::AudioDevice::Offline{}};
    const fh::audio::VoiceId voice = device.mixer().playStream(std::make_shared<ConstantStream>(), {});
    CHECK(voice != fh::audio::kNoVoice);
    const std::vector<float> out = device.render(1200);
    CHECK_NEAR(out[200], 0.25, 1e-5);
    CHECK_NEAR(out[201], -0.5, 1e-5);
    CHECK_NEAR(out[1100 * 2], 0.0, 1e-6);
    CHECK(!device.mixer().isPlaying(voice));
}

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (!dir || !*dir) {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

TEST(discRadioDataResolves)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const auto assets = RadioAssets::load(vfs);
    const RadioCatalog& c = *assets->catalog();
    CHECK_EQ(c.system.stations.size(), std::size_t{9});
    CHECK_EQ(c.system.offStation(), std::size_t{3});
    CHECK_EQ(c.system.stations[0].name, std::string("Radio1"));
    CHECK_EQ(c.system.settings.maxTracksBetweenDJ, 1);
    const RadioText text(vfs, "en", 4, 3);
    CHECK_EQ(text.stationName(0), std::string("HORIZON BASS ARENA"));
    CHECK_EQ(text.stationName(1), std::string("HORIZON PULSE"));
    CHECK_EQ(text.stationName(2), std::string("HORIZON ROCKS"));
    CHECK_EQ(text.stationName(3), std::string("OFF"));
    CHECK_EQ(text.radioOff(), std::string("RADIO OFF"));

    // Every name the stations use has a sample, and the info's index points
    // at the sample of that name; offsets are its sync points in whole ms.
    const auto checkInfo = [&](const SoundBankInfoMap& info, RadioBank bankKind, const std::string& name) {
        const auto it = info.find(name);
        if (it == info.end()) {
            fh::test::fail(__FILE__, __LINE__, std::format("no sound bank info for {}", name));
            return;
        }
        const fh::audio::FsbBank& bank = assets->bank(bankKind);
        const auto index = static_cast<std::size_t>(it->second.soundBankIndex);
        CHECK(index < bank.size());
        if (index >= bank.size()) {
            return;
        }
        const fh::audio::FsbSample& s = bank.sample(index);
        CHECK(bank.sampleHasName(index, name));
        const auto ms = [&](std::uint32_t frame) {
            return static_cast<std::uint32_t>(std::uint64_t{frame} * 1000 / static_cast<std::uint64_t>(s.frequency));
        };
        CHECK(std::abs(static_cast<long>(ms(s.sampleCount)) - static_cast<long>(it->second.fileLength)) <= 1);
        if (it->second.syncpointIndexIdentStart >= 0) {
            const auto sync = static_cast<std::size_t>(it->second.syncpointIndexIdentStart);
            CHECK(sync < s.syncPoints.size());
            if (sync < s.syncPoints.size()) {
                CHECK_EQ(s.syncPoints[sync].name, std::string("IdentStart"));
                CHECK(std::abs(static_cast<long>(ms(s.syncPoints[sync].frame))
                          - static_cast<long>(it->second.offsetIdentStart))
                    <= 1);
            }
        }
        if (it->second.syncpointStartNextTrack >= 0) {
            const auto sync = static_cast<std::size_t>(it->second.syncpointStartNextTrack);
            CHECK(sync < s.syncPoints.size());
            if (sync < s.syncPoints.size()) {
                CHECK_EQ(s.syncPoints[sync].name, std::string("StartNextTrack"));
            }
        }
    };
    for (std::size_t i = 0; i <= c.system.offStation(); ++i) {
        const Station& st = c.system.stations[i];
        for (const MusicTrack& t : st.playlist.tracks) {
            checkInfo(c.musicInfo, RadioBank::Music, t.fmodName);
        }
        for (const VoiceLine& l : st.idents.lines) {
            checkInfo(c.voiceInfo, RadioBank::Voice, l.fmodName);
        }
        for (const VoiceLine& l : st.regular.lines) {
            checkInfo(c.voiceInfo, RadioBank::Voice, l.fmodName);
        }
    }
}

TEST(discStreamMatchesWholeDecode)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const auto assets = RadioAssets::load(vfs);
    const fh::audio::FsbBank voice = fh::audio::FsbBank::parse(vfs.readOrThrow("media/audio/radio/Radio_VO_EN.fsb"),
        "Radio_VO_EN.fsb");
    const std::size_t index = voice.findSample("R1_Ident_01").value_or(0);
    const fh::audio::PcmBuffer whole = fh::audio::decodeSample(voice, index);
    const std::size_t channels = static_cast<std::size_t>(whole.channels);
    for (const std::size_t start : {std::size_t{0}, whole.frames() / 3}) {
        fh::audio::MpegSampleStream stream = assets->openStream(RadioBank::Voice, static_cast<int>(index), start);
        std::vector<float> got;
        while (!stream.finished()) {
            stream.decode(8192, got);
        }
        CHECK_EQ(got.size(), (whole.frames() - start) * channels);
        float worst = 0.0F;
        for (std::size_t i = 0; i < got.size() && i + start * channels < whole.samples.size(); ++i) {
            worst = std::max(worst, std::abs(got[i] - whole.samples[start * channels + i]));
        }
        CHECK(worst < 1e-4F);
    }
}

TEST(discStationsNeverRepeatEarly)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const auto assets = RadioAssets::load(vfs);
    for (std::size_t station = 0; station < 3; ++station) {
        RadioSequencer s(assets->catalog(), static_cast<std::uint32_t>(station + 1));
        s.selectStation(station, false);
        s.startFreeRoam();
        const auto events = run(s, 240.0 * 60.0);
        const auto tracks = ofKind(events, RadioEvent::Kind::TrackStarted);
        const std::size_t n = assets->catalog()->system.stations[station].playlist.tracks.size();
        CHECK(tracks.size() > n);
        for (std::size_t i = 0; i + n <= tracks.size(); ++i) {
            std::set<std::string> window;
            for (std::size_t j = i; j < i + n; ++j) {
                window.insert(tracks[j]->name);
            }
            CHECK_EQ(window.size(), n);
        }
        CHECK(ofKind(events, RadioEvent::Kind::DialogueStarted).size() + 1 >= tracks.size());
    }
}

TEST(discRadioRendersOffline)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    fh::audio::AudioDevice device{fh::audio::AudioDevice::Offline{}};
    Radio::Options options;
    options.worker = false;
    options.seed = 4;
    Radio radio(vfs, device, options);
    CHECK_EQ(radio.stations().size(), std::size_t{9});
    CHECK_EQ(radio.stations()[1].displayName, std::string("HORIZON PULSE"));
    radio.selectStation(1);
    radio.startFreeRoam();
    double energy = 0.0;
    for (int frame = 0; frame < 60 * 5; ++frame) {
        for (const float v : device.render(800)) {
            energy += static_cast<double>(v) * v;
        }
        radio.update(1.0F / 60.0F);
    }
    CHECK(energy > 1.0);
    CHECK_EQ(radio.nowPlaying().station, std::size_t{1});
    CHECK(!radio.nowPlaying().title.empty());
    radio.turnOff();
    for (int frame = 0; frame < 60; ++frame) {
        device.render(800);
        radio.update(1.0F / 60.0F);
    }
    CHECK(radio.nowPlaying().off);
    double after = 0.0;
    for (const float v : device.render(4800)) {
        after += static_cast<double>(v) * v;
    }
    CHECK(after < 1e-6);
}

TEST(discRadioWorkerProducesAudio)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    fh::audio::AudioDevice device{fh::audio::AudioDevice::Offline{}};
    Radio::Options options;
    options.seed = 2;
    Radio radio(vfs, device, options);
    radio.startFreeRoam();
    double energy = 0.0;
    for (int frame = 0; frame < 120; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (const float v : device.render(800)) {
            energy += static_cast<double>(v) * v;
        }
        radio.update(1.0F / 60.0F);
    }
    CHECK(energy > 0.1);
}

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
