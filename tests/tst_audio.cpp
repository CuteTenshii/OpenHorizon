// Tests of the audio layer: the FSB and FEV readers on files built here in
// the layouts the headers document, the DSP, mixer and engine-sound logic on
// synthetic signals, and, when FH1_GAME_DIR names an extracted disc, a few
// banks of every kind (every bank as a slow test), every project and every
// engine tuning on it.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/CollisionSounds.h"
#include "audio/CrashMix.h"
#include "audio/Decode.h"
#include "audio/Dsp.h"
#include "audio/EngineSound.h"
#include "audio/EngineTuning.h"
#include "audio/EventPlayer.h"
#include "audio/Fev.h"
#include "audio/Fsb.h"
#include "audio/MixSnapshots.h"
#include "audio/Mixer.h"
#include "audio/TyreSound.h"
#include "audio/Wav.h"
#include "base/Bytes.h"
#include "vfs/Vfs.h"

#include <SDL3/SDL.h>
#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using Catch::Matchers::WithinAbs;

using openhorizon::test::gameDir;

namespace {

using openhorizon::Bytes;
using namespace openhorizon::audio;

/// Builds little-endian test data, the byte order of FMOD's files.
struct Writer {
    Bytes data;

    void u8(std::uint8_t v) { data.push_back(v); }
    void u16(std::uint32_t v)
    {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v)
    {
        u16(v & 0xFFFFU);
        u16(v >> 16);
    }
    void u64(std::uint64_t v)
    {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
    void text(std::string_view s) { data.insert(data.end(), s.begin(), s.end()); }
    void fixed(std::string_view s, std::size_t size)
    {
        text(s.substr(0, size));
        fill(size - std::min(size, s.size()), 0);
    }
    void fill(std::size_t count, std::uint8_t value) { data.insert(data.end(), count, value); }
    /// FMOD's counted string: length including the NUL, then the bytes.
    void string(std::string_view s)
    {
        if (s.empty()) {
            u32(0);
            return;
        }
        u32(static_cast<std::uint32_t>(s.size() + 1));
        text(s);
        u8(0);
    }
    void append(const Bytes& other) { data.insert(data.end(), other.begin(), other.end()); }
};

struct TestSample {
    std::string name;
    std::uint32_t frames = 0;
    std::uint32_t mode = 0;
    std::int32_t frequency = 48000;
    std::uint16_t channels = 1;
    std::uint32_t loopStart = 0;
    std::uint32_t loopEnd = 0;
    Bytes data;
    Bytes extra;
};

constexpr std::uint64_t kTestHash = 0x0807060504030201ULL;

/// An FSB4 bank as the Fsb.h comment lays it out.
Bytes buildFsb(const std::vector<TestSample>& samples, std::uint32_t flags)
{
    const bool basic = (flags & 0x2U) != 0;
    Writer headers;
    Writer data;
    for (std::size_t i = 0; i < samples.size(); ++i) {
        const TestSample& s = samples[i];
        data.fill((32 - data.data.size() % 32) % 32, 0xEE);
        if (basic && i > 0) {
            headers.u32(s.frames);
            headers.u32(static_cast<std::uint32_t>(s.data.size()));
        } else {
            headers.u16(static_cast<std::uint32_t>(0x50 + s.extra.size()));
            // FMOD keeps at most 29 characters so that a NUL always follows.
            headers.fixed(std::string_view(s.name).substr(0, 29), 30);
            headers.u32(s.frames);
            headers.u32(static_cast<std::uint32_t>(s.data.size()));
            headers.u32(s.loopStart);
            headers.u32(s.loopEnd);
            headers.u32(s.mode);
            headers.u32(static_cast<std::uint32_t>(s.frequency));
            headers.u16(255);
            headers.u16(128);
            headers.u16(128);
            headers.u16(s.channels);
            headers.f32(1.0F);
            headers.f32(10000.0F);
            headers.u32(0);
            headers.u32(0);
            headers.append(s.extra);
        }
        data.append(s.data);
    }
    Writer out;
    out.text("FSB4");
    out.u32(static_cast<std::uint32_t>(samples.size()));
    out.u32(static_cast<std::uint32_t>(headers.data.size()));
    out.u32(static_cast<std::uint32_t>(data.data.size()));
    out.u32(0x00040000);
    out.u32(flags);
    out.u64(kTestHash);
    out.fill(16, 0);
    out.append(headers.data);
    out.append(data.data);
    return out.data;
}

Bytes pcm16(std::initializer_list<std::int16_t> values)
{
    Writer w;
    for (const std::int16_t v : values) {
        w.u16(static_cast<std::uint16_t>(v));
    }
    return w.data;
}

std::vector<TestSample> testSamples()
{
    TestSample tone;
    tone.name = "Tone.wav";
    tone.frames = 4;
    tone.mode = 0x10 | 0x20 | 0x2;
    tone.frequency = 22050;
    tone.loopStart = 1;
    tone.loopEnd = 3;
    tone.data = pcm16({0, 16384, -16384, 32767});

    TestSample stereo;
    stereo.name = "Stereo8";
    stereo.frames = 3;
    stereo.mode = 0x8 | 0x40 | 0x80;
    stereo.channels = 2;
    stereo.data = {128, 255, 0, 128, 192, 64};
    stereo.loopEnd = 2;

    TestSample xma;
    xma.name = "EngineLoopWithAVeryLongNameThatIsCut";
    xma.frames = 40000;
    xma.mode = 0x01000000;
    xma.channels = 2;
    xma.data.assign(2048, 0);
    Writer extra;
    for (const std::uint32_t v : {0U, 0U, 0U, 16U, 1U, 3U, 0U, 0x9E00U}) {
        extra.u32(v);
    }
    xma.extra = extra.data;
    return {tone, stereo, xma};
}

TEST_CASE("fsbParsesHeadersAndLaysOutData", "[audio]")
{
    const std::vector<TestSample> source = testSamples();
    const FsbBank bank = FsbBank::parse(buildFsb(source, 0x40), "test.fsb");
    CHECK(bank.size() == std::size_t{3});
    CHECK(bank.version() == 0x00040000U);
    CHECK(bank.hash() == kTestHash);

    const FsbSample& tone = bank.sample(0);
    CHECK(tone.name == std::string("Tone.wav"));
    CHECK(tone.sampleCount == 4U);
    CHECK(tone.frequency == 22050);
    CHECK(tone.codec == SampleCodec::Pcm16);
    CHECK(tone.loops());
    CHECK(tone.loopStart == 1U);
    CHECK(tone.loopEnd == 3U);

    const FsbSample& stereo = bank.sample(1);
    CHECK(stereo.codec == SampleCodec::Pcm8);
    CHECK(stereo.pcmUnsigned());
    CHECK(stereo.channels == 2);

    const FsbSample& xma = bank.sample(2);
    CHECK(xma.codec == SampleCodec::Xma);
    CHECK(xma.name.size() == std::size_t{29});
    CHECK(xma.xmaSeekTable.size() == std::size_t{2});
    if (xma.xmaSeekTable.size() == 2) {
        CHECK(xma.xmaSeekTable[1] == 0x9E00U);
    }

    // Data starts after the headers and each sample on a 32-byte boundary.
    const std::size_t dataStart = 0x30 + 0x50 * 2 + 0x70;
    CHECK(tone.dataOffset == dataStart);
    CHECK(stereo.dataOffset == dataStart + 32);
    CHECK(xma.dataOffset == dataStart + 64);
    CHECK(bank.sampleData(1).size() == std::size_t{6});
    CHECK(bank.sampleData(1)[1] == 255);
}

TEST_CASE("fsbFindsSamplesByWaveformName", "[audio]")
{
    const FsbBank bank = FsbBank::parse(buildFsb(testSamples(), 0), "test.fsb");
    CHECK(bank.findSample("Tone") == std::optional<std::size_t>(0));
    CHECK(bank.findSample("wav/tone.WAV") == std::optional<std::size_t>(0));
    CHECK(bank.findSample("..\\sounds\\Stereo8.wav") == std::optional<std::size_t>(1));
    CHECK(bank.findSample("EngineLoopWithAVeryLongNameThatIsCut.wav") == std::optional<std::size_t>(2));
    CHECK(!bank.findSample("Missing").has_value());
    CHECK(bank.sampleHasName(1, "stereo8"));
    CHECK(!bank.sampleHasName(0, "stereo8"));
}

TEST_CASE("fsbBasicHeadersRepeatTheFirstSample", "[audio]")
{
    std::vector<TestSample> samples(3);
    for (std::size_t i = 0; i < samples.size(); ++i) {
        samples[i].name = "First";
        samples[i].frames = static_cast<std::uint32_t>(2 + i);
        samples[i].mode = 0x10;
        samples[i].frequency = 32000;
        samples[i].data = Bytes((2 + i) * 2, static_cast<std::uint8_t>(i));
    }
    const FsbBank bank = FsbBank::parse(buildFsb(samples, 0x2), "basic.fsb");
    CHECK(bank.size() == std::size_t{3});
    CHECK(bank.sample(2).sampleCount == 4U);
    CHECK(bank.sample(2).frequency == 32000);
    CHECK(bank.sample(2).compressedSize == 8U);
    CHECK(bank.sampleData(2)[0] == 2);
}

TEST_CASE("fsbRejectsDamagedBanks", "[audio]")
{
    const Bytes good = buildFsb(testSamples(), 0);
    Bytes badMagic = good;
    badMagic[3] = '3';
    CHECK_THROWS_AS(FsbBank::parse(badMagic, "magic.fsb"), std::exception);
    CHECK_THROWS_AS(FsbBank::parse(Bytes(good.begin(), good.begin() + 0x40), "short.fsb"), std::exception);
    Bytes truncatedData(good.begin(), good.end() - 100);
    CHECK_THROWS_AS(FsbBank::parse(truncatedData, "data.fsb"), std::exception);
    Bytes oversize = good;
    oversize[0x30 + 0x24] = 0xFF;
    CHECK_THROWS_AS(FsbBank::parse(oversize, "oversize.fsb"), std::exception);
    const FsbBank bank = FsbBank::parse(good, "ok.fsb");
    CHECK_THROWS_AS(bank.sample(3), std::exception);
}

TEST_CASE("pcmDecodesExactly", "[audio]")
{
    const FsbBank bank = FsbBank::parse(buildFsb(testSamples(), 0), "test.fsb");
    const PcmBuffer tone = decodeSample(bank, 0);
    CHECK(tone.channels == 1);
    CHECK(tone.sampleRate == 22050);
    CHECK(tone.frames() == std::size_t{4});
    CHECK_THAT(tone.samples[1], WithinAbs(0.5, 1e-6));
    CHECK_THAT(tone.samples[2], WithinAbs(-0.5, 1e-6));
    CHECK_THAT(tone.samples[3], WithinAbs(32767.0 / 32768.0, 1e-6));
    CHECK(tone.loopStart == std::size_t{1});
    CHECK(tone.loopEnd == std::size_t{4});

    const PcmBuffer stereo = decodeSample(bank, 1);
    CHECK(stereo.frames() == std::size_t{3});
    CHECK_THAT(stereo.samples[0], WithinAbs(0.0, 1e-6));
    CHECK_THAT(stereo.samples[1], WithinAbs(127.0 / 128.0, 1e-6));
    CHECK_THAT(stereo.samples[2], WithinAbs(-1.0, 1e-6));
    CHECK_THAT(stereo.samples[4], WithinAbs(0.5, 1e-6));
    CHECK_THAT(stereo.samples[5], WithinAbs(-0.5, 1e-6));

    // Two kilobytes of zeros are no XMA stream.
    CHECK_THROWS_AS(decodeSample(bank, 2), std::exception);
}

/// A small FEV1 project with a complex and a simple event, in version 0x40
/// or, for `old`, 0x37 layout.
Bytes buildFev(bool old, int soundDefinitionOfClick = 0)
{
    Writer w;
    w.text("FEV1");
    w.u32(old ? 0x00370000 : 0x00400000);
    w.u32(0x100);
    w.u32(0x200);
    if (!old) {
        w.u32(2);
        w.u32(0);
        w.u32(1);
        w.u32(1);
        w.u32(2);
    }
    w.string("Proj");
    w.u32(1);
    w.u32(0x200);
    w.u32(4);
    if (!old) {
        w.u64(kTestHash);
    }
    w.string("Bank");

    w.string("master");
    w.f32(1.0F);
    w.f32(0.0F);
    w.u32(0);
    w.u32(0);
    w.u32(1);
    w.string("music");
    w.f32(0.5F);
    w.f32(0.0F);
    w.u32(0);
    w.u32(0);
    w.u32(0);

    const auto properties = [&](float volume, std::uint32_t mode) {
        const std::size_t start = w.data.size();
        w.f32(volume);
        w.f32(0.0F);
        w.f32(0.0F);
        w.f32(0.0F);
        w.u32(128);
        w.u32(2);
        if (!old) {
            w.u32(10000);
        }
        w.u32(mode);
        w.f32(5.0F);
        w.f32(50.0F);
        w.fill((old ? 128 : 132) - (w.data.size() - start), 0);
    };
    const auto eventHeader = [&](std::uint32_t type, std::string_view name) {
        w.u32(type);
        w.string(name);
        if (!old) {
            for (std::uint8_t i = 0; i < 16; ++i) {
                w.u8(i);
            }
        }
    };
    const auto sound = [&](std::uint16_t definition, float start, float length, bool oneShot) {
        w.u16(definition);
        w.f32(start);
        w.f32(length);
        w.u32(0);
        w.u32(oneShot ? 1 : 0);
        w.u32(0xFFFFFFFFU);
        w.fill(36, 0);
    };

    w.u32(1);
    w.string("Group");
    w.u32(1);
    w.string("Placeable");
    w.u32(0);
    w.u32(7);
    w.u32(1);
    w.u32(1);

    w.string("Sub");
    w.u32(0);
    w.u32(0);
    w.u32(1);
    eventHeader(0x8, "Engine");
    properties(0.5F, 0x10);
    w.u32(1);
    w.u16(2);
    w.u16(0xFFFF);
    w.u16(0);
    w.u16(2);
    w.u16(1);
    sound(1, 0.0F, 0.5F, false);
    sound(0, 0.5F, 0.5F, true);
    w.u32(0xFFFFFFFFU);
    w.string("");
    w.u32(0);
    w.u32(0xC);
    if (!old) {
        w.u32(0);
    }
    w.u32(2);
    w.f32(0.0F);
    w.f32(0.0F);
    w.u32(1);
    w.f32(1.0F);
    w.f32(1.0F);
    w.u32(1);
    w.u32(1);
    w.u32(0);
    w.u32(2);
    w.string("RPM");
    w.f32(0.0F);
    w.f32(0.0F);
    w.f32(8000.0F);
    w.u32(3);
    w.f32(0.0F);
    w.u32(0);
    w.u32(1);
    w.f32(0.25F);
    w.string("Load");
    w.f32(0.0F);
    w.f32(0.0F);
    w.f32(1.0F);
    w.u32(2);
    w.f32(0.0F);
    w.u32(0);
    w.u32(0);
    w.u32(0);
    w.u32(1);
    w.string("master");

    eventHeader(0x10, "Click");
    properties(1.0F, 0x8);
    w.u32(1);
    sound(static_cast<std::uint16_t>(soundDefinitionOfClick), 0.0F, 1.0F, true);
    w.u32(1);
    w.string("master");

    w.u32(1);
    w.fill(old ? 60 : 70, 0);
    w.u32(2);
    w.string("/Click");
    w.u32(0);
    w.u32(2);
    w.u32(0);
    w.u32(100);
    w.string("wav/Click.wav");
    w.string("Bank");
    w.u32(1);
    w.u32(250);
    w.u32(2);
    w.u32(50);
    w.string("/Loop");
    w.u32(0);
    w.u32(1);
    w.u32(0);
    w.u32(100);
    w.string("Loop.wav");
    w.string("Bank");
    w.u32(0);
    w.u32(1000);

    w.u32(0);
    w.u32(8);
    w.text("comp");
    return w.data;
}

TEST_CASE("fevParsesEventsAndSoundDefinitions", "[audio]")
{
    for (const bool old : {false, true}) {
        const Bytes data = buildFev(old);
        const FevProject project = FevProject::parse(data, "test.fev");
        CHECK(project.name() == std::string("Proj"));
        CHECK(project.version() == (old ? 0x00370000U : 0x00400000U));
        CHECK(project.banks().size() == std::size_t{1});
        CHECK(project.banks()[0].name == std::string("Bank"));
        CHECK(project.banks()[0].hash == (old ? 0ULL : kTestHash));
        CHECK(project.categories().children.size() == std::size_t{1});
        CHECK_THAT(project.categories().children[0].volume, WithinAbs(0.5, 1e-6));

        CHECK(project.events().size() == std::size_t{2});
        const FevEvent* engine = project.findEvent("Group/Sub/Engine");
        CHECK(engine != nullptr);
        if (engine) {
            CHECK(!engine->simple);
            CHECK(engine->is3d());
            CHECK_THAT(engine->volume, WithinAbs(0.5, 1e-6));
            CHECK(engine->maxPlaybacks == 2U);
            CHECK_THAT(engine->maxDistance, WithinAbs(50.0, 1e-6));
            CHECK(engine->guid[15] == (old ? 0 : 15));
            CHECK(engine->layers.size() == std::size_t{1});
            CHECK(engine->layers[0].sounds.size() == std::size_t{2});
            CHECK(engine->layers[0].sounds[0].soundDefinition == 1);
            CHECK(!engine->layers[0].sounds[0].oneShot);
            CHECK(engine->layers[0].sounds[1].oneShot);
            CHECK(engine->layers[0].envelopes.size() == std::size_t{1});
            const FevEnvelope& envelope = engine->layers[0].envelopes[0];
            CHECK(envelope.isVolume());
            CHECK(envelope.parameter == 1U);
            CHECK_THAT(envelope.valueAt(0.25F), WithinAbs(0.25, 1e-6));
            CHECK_THAT(envelope.valueAt(2.0F), WithinAbs(1.0, 1e-6));
            CHECK(engine->parameters.size() == std::size_t{2});
            CHECK_THAT(engine->parameters[0].maximum, WithinAbs(8000.0, 1e-3));
            CHECK(engine->parameters[0].sustainPoints.size() == std::size_t{1});
            CHECK(engine->findParameter("load") == &engine->parameters[1]);
            CHECK(engine->categories.size() == std::size_t{1});
        }
        const FevEvent* click = project.findEvent("proj/group/click");
        CHECK(click != nullptr);
        if (click) {
            CHECK(click->simple);
            CHECK(!click->is3d());
            CHECK(click->path == std::string("Group/Click"));
        }
        CHECK(project.findEvent("Group/Missing") == nullptr);

        CHECK(project.groups().size() == std::size_t{2});
        const FevGroup* group = project.findGroup("proj/group");
        CHECK(group != nullptr);
        if (group) {
            const FevUserProperty* placeable = group->findUserProperty("placeable");
            CHECK(placeable != nullptr);
            if (placeable) {
                CHECK(std::holds_alternative<std::int32_t>(placeable->value));
                CHECK(std::get<std::int32_t>(placeable->value) == 7);
            }
            CHECK(group->findUserProperty("Missing") == nullptr);
        }
        const FevGroup* sub = project.findGroup("Group/Sub");
        CHECK(sub != nullptr);
        CHECK((sub && sub->userProperties.empty()));
        CHECK(project.findGroup("Sub") == nullptr);

        CHECK(project.soundDefinitions().size() == std::size_t{2});
        const FevSoundDefinition& definition = project.soundDefinition(0);
        CHECK(definition.name == std::string("/Click"));
        CHECK(definition.waveforms.size() == std::size_t{2});
        CHECK(definition.waveforms[0].type == FevWaveform::Type::Wavetable);
        CHECK(definition.waveforms[0].sampleIndex == 1U);
        CHECK(definition.waveforms[0].lengthMs == 250U);
        CHECK(definition.waveforms[1].type == FevWaveform::Type::DontPlay);
        CHECK_THROWS_AS(project.soundDefinition(2), std::exception);
    }
}

TEST_CASE("fevRejectsMisreadProjects", "[audio]")
{
    const Bytes good = buildFev(false);
    CHECK_THROWS_AS(
        FevProject::parse(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2)), "half.fev"),
        std::exception);
    Bytes trailing = good;
    trailing.push_back(0);
    CHECK_THROWS_AS(FevProject::parse(trailing, "trailing.fev"), std::exception);
    CHECK_THROWS_AS(FevProject::parse(buildFev(false, 5), "badref.fev"), std::exception);
    Bytes version = good;
    version[6] = 0x41;
    CHECK_THROWS_AS(FevProject::parse(version, "version.fev"), std::exception);
}

TEST_CASE("tyreSurfaceMapParses", "[audio]")
{
    const TyreSurfaceMap map = TyreSurfaceMap::parse(
        "<Tires>\n"
        "<SurfaceMap PhysicsSurface=\"Asphalt\" Lod1EventGroup=\"Tires/Tires/LOD1/OnRoad\" "
        "Lod2EventGroup=\"Tires/Tires/LOD2/OnRoad\" SurfaceIndex=\"0\"/>\n"
        "<SurfaceMap PhysicsSurface=\"Gravel\" Lod1EventGroup=\"Tires/Tires/LOD1/OffRoad\" "
        "Lod2EventGroup=\"Tires/Tires/LOD2/OffRoad\" SurfaceIndex=\"12\"/>\n"
        "<SurfaceMap Lod1EventGroup=\"Tires/Tires/LOD1/OnRoad\"/>\n"
        "</Tires>\n");
    CHECK(map.surfaces.size() == std::size_t{2});
    CHECK(map.find("gravel") == std::optional<std::size_t>(1));
    CHECK(!map.find("Kerb"));
    CHECK(map.surfaces[1].lod1EventGroup == std::string("Tires/Tires/LOD1/OffRoad"));
    CHECK(map.surfaces[1].lod2EventGroup == std::string("Tires/Tires/LOD2/OffRoad"));
    CHECK(map.surfaces[1].surfaceIndex == 12);
    CHECK_THROWS_AS(TyreSurfaceMap::parse("<Other/>"), std::exception);
    CHECK_THROWS_AS(TyreSurfaceMap::parse("not xml <"), std::exception);
}

TEST_CASE("wavHeaderDescribesTheData", "[audio]")
{
    PcmBuffer pcm;
    pcm.sampleRate = 44100;
    pcm.channels = 2;
    pcm.samples = {0.0F, 1.0F, -1.0F, 2.0F};
    const Bytes pcmWav = encodeWav(pcm, WavFormat::Pcm16);
    CHECK(pcmWav.size() == std::size_t{44 + 8});
    CHECK(openhorizon::loadLe32(pcmWav.data() + 24) == 44100U);
    CHECK(openhorizon::loadLe16(pcmWav.data() + 20) == 1);
    CHECK(openhorizon::loadLe16(pcmWav.data() + 22) == 2);
    CHECK(static_cast<std::int16_t>(openhorizon::loadLe16(pcmWav.data() + 46)) == 32767);
    CHECK(static_cast<std::int16_t>(openhorizon::loadLe16(pcmWav.data() + 50)) == 32767);

    const Bytes floatWav = encodeWav(pcm, WavFormat::Float32);
    CHECK(floatWav.size() == std::size_t{44 + 16});
    CHECK(openhorizon::loadLe16(floatWav.data() + 20) == 3);
    CHECK(openhorizon::loadLe32(floatWav.data() + 40) == 16U);
    CHECK_THAT(std::bit_cast<float>(openhorizon::loadLe32(floatWav.data() + 56)), WithinAbs(2.0, 0.0));
}

TEST_CASE("biquadResponses", "[audio]")
{
    const float rate = 48000.0F;
    FilterSpec lowpass;
    lowpass.type = FilterSpec::Type::Lowpass;
    lowpass.frequency = 1000.0F;
    const BiquadCoefficients lp = BiquadCoefficients::design(lowpass, rate);
    CHECK_THAT(lp.magnitudeAt(50.0F, rate), WithinAbs(1.0, 0.01));
    CHECK_THAT(lp.magnitudeAt(1000.0F, rate), WithinAbs(std::numbers::sqrt2 / 2.0, 0.01));
    CHECK(lp.magnitudeAt(10000.0F, rate) < 0.02F);

    FilterSpec highpass = lowpass;
    highpass.type = FilterSpec::Type::Highpass;
    const BiquadCoefficients hp = BiquadCoefficients::design(highpass, rate);
    CHECK(hp.magnitudeAt(50.0F, rate) < 0.01F);
    CHECK_THAT(hp.magnitudeAt(15000.0F, rate), WithinAbs(1.0, 0.02));

    FilterSpec peq;
    peq.type = FilterSpec::Type::Peaking;
    peq.frequency = 800.0F;
    peq.bandwidth = 1.0F;
    peq.gain = 2.0F;
    const BiquadCoefficients pk = BiquadCoefficients::design(peq, rate);
    CHECK_THAT(pk.magnitudeAt(800.0F, rate), WithinAbs(2.0, 0.01));
    CHECK_THAT(pk.magnitudeAt(20.0F, rate), WithinAbs(1.0, 0.02));
    CHECK_THAT(pk.magnitudeAt(20000.0F, rate), WithinAbs(1.0, 0.02));

    // Filtering a signal agrees with the designed response.
    Biquad filter;
    filter.setCoefficients(lp);
    double energy = 0.0;
    for (int i = 0; i < 48000; ++i) {
        const float x = std::sin(2.0F * std::numbers::pi_v<float> * 10000.0F * static_cast<float>(i) / rate);
        const float y = filter.process(x);
        if (i > 4800) {
            energy += static_cast<double>(y) * y;
        }
    }
    CHECK(std::sqrt(energy / 43199.0) < 0.02);

    const BiquadCoefficients half = BiquadCoefficients::interpolate(lp, pk, 0.5F);
    CHECK_THAT(half.b0, WithinAbs((lp.b0 + pk.b0) / 2.0F, 1e-6));
}

TEST_CASE("hermiteInterpolation", "[audio]")
{
    CHECK_THAT(hermite(0.0F, 1.0F, 2.0F, 3.0F, 0.0F), WithinAbs(1.0, 1e-6));
    CHECK_THAT(hermite(0.0F, 1.0F, 2.0F, 3.0F, 1.0F), WithinAbs(2.0, 1e-6));
    CHECK_THAT(hermite(0.0F, 1.0F, 2.0F, 3.0F, 0.25F), WithinAbs(1.25, 1e-6));
    CHECK_THAT(hermite(5.0F, 5.0F, 5.0F, 5.0F, 0.7F), WithinAbs(5.0, 1e-6));
}

std::shared_ptr<const PcmBuffer> constant(float value, std::size_t frames, int channels = 1, int rate = 48000)
{
    auto pcm = std::make_shared<PcmBuffer>();
    pcm->sampleRate = rate;
    pcm->channels = channels;
    pcm->samples.assign(frames * static_cast<std::size_t>(channels), value);
    pcm->loopStart = 0;
    pcm->loopEnd = frames;
    return pcm;
}

std::shared_ptr<const PcmBuffer> sine(float frequency, int rate, float seconds)
{
    auto pcm = std::make_shared<PcmBuffer>();
    pcm->sampleRate = rate;
    pcm->channels = 1;
    const auto frames = static_cast<std::size_t>(static_cast<float>(rate) * seconds);
    pcm->samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        pcm->samples[i] = 0.5F * std::sin(2.0F * std::numbers::pi_v<float> * frequency * static_cast<float>(i)
            / static_cast<float>(rate));
    }
    pcm->loopEnd = frames;
    return pcm;
}

std::vector<float> render(Mixer& mixer, std::size_t frames)
{
    std::vector<float> out(frames * 2);
    mixer.render(out);
    return out;
}

/// Sign changes of one channel, twice the frequency per second of signal.
int zeroCrossings(const std::vector<float>& stereo, int channel)
{
    int crossings = 0;
    for (std::size_t i = 2 + static_cast<std::size_t>(channel); i < stereo.size(); i += 2) {
        if ((stereo[i - 2] < 0.0F) != (stereo[i] < 0.0F)) {
            ++crossings;
        }
    }
    return crossings;
}

TEST_CASE("mixerLevelsAndPan", "[audio]")
{
    Mixer mixer(48000);
    VoiceSettings settings;
    settings.loop = true;
    const VoiceId centre = mixer.play(constant(0.5F, 4800), settings);
    CHECK(centre != kNoVoice);
    std::vector<float> out = render(mixer, 1024);
    CHECK_THAT(out[1000], WithinAbs(0.5 * std::cos(std::numbers::pi / 4.0), 1e-4));
    CHECK_THAT(out[1001], WithinAbs(0.5 * std::cos(std::numbers::pi / 4.0), 1e-4));

    mixer.setPan(centre, -1.0F);
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_THAT(out[100], WithinAbs(0.5, 1e-4));
    CHECK_THAT(out[101], WithinAbs(0.0, 1e-4));

    mixer.setVolume(centre, 0.5F);
    mixer.setPan(centre, 0.0F);
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_THAT(out[100], WithinAbs(0.25 * std::cos(std::numbers::pi / 4.0), 1e-4));

    // A stereo source keeps its channels; pan is a balance.
    Mixer stereoMixer(48000);
    auto stereo = std::make_shared<PcmBuffer>(*constant(0.0F, 1000, 2));
    for (std::size_t i = 0; i < stereo->samples.size(); i += 2) {
        stereo->samples[i] = 0.4F;
        stereo->samples[i + 1] = -0.2F;
    }
    settings.pan = 0.5F;
    stereoMixer.play(stereo, settings);
    out = render(stereoMixer, 256);
    CHECK_THAT(out[200], WithinAbs(0.2, 1e-4));
    CHECK_THAT(out[201], WithinAbs(-0.2, 1e-4));
}

TEST_CASE("mixerResamplesByRateAndPitch", "[audio]")
{
    Mixer mixer(48000);
    VoiceSettings settings;
    settings.loop = true;
    const VoiceId voice = mixer.play(sine(1000.0F, 24000, 1.0F), settings);
    std::vector<float> out = render(mixer, 48000);
    CHECK(std::abs(zeroCrossings(out, 0) - 2000) <= 4);

    mixer.setPitch(voice, 2.0F);
    render(mixer, 512);
    out = render(mixer, 48000);
    CHECK(std::abs(zeroCrossings(out, 0) - 4000) <= 6);

    mixer.setPitch(voice, 0.5F);
    render(mixer, 512);
    out = render(mixer, 48000);
    CHECK(std::abs(zeroCrossings(out, 0) - 1000) <= 4);
}

TEST_CASE("mixerVoicesEndLoopAndStop", "[audio]")
{
    Mixer mixer(48000);
    const VoiceId once = mixer.play(constant(0.5F, 100), VoiceSettings{});
    CHECK(mixer.isPlaying(once));
    std::vector<float> out = render(mixer, 256);
    CHECK(!mixer.isPlaying(once));
    CHECK(std::abs(out[2 * 50]) > 0.1F);
    CHECK(out[2 * 200] == 0.0F);

    VoiceSettings looping;
    looping.loop = true;
    const VoiceId loop = mixer.play(constant(0.5F, 100), looping);
    out = render(mixer, 1000);
    CHECK(mixer.isPlaying(loop));
    CHECK(std::abs(out[2 * 999]) > 0.1F);
    CHECK(mixer.activeVoiceCount() == std::size_t{1});

    mixer.stop(loop, 0.01F);
    out = render(mixer, 1024);
    CHECK(!mixer.isPlaying(loop));
    CHECK(out[2 * 1023] == 0.0F);
    CHECK(mixer.activeVoiceCount() == std::size_t{0});

    // A handle stays dead when its slot is reused.
    const VoiceId reused = mixer.play(constant(0.5F, 100), looping);
    CHECK(reused != loop);
    CHECK(!mixer.isPlaying(loop));
    mixer.setVolume(loop, 1.0F);
    CHECK(mixer.isPlaying(reused));
    CHECK(mixer.play(nullptr, looping) == kNoVoice);
}

TEST_CASE("rolloffShape", "[audio]")
{
    Rolloff rolloff{10.0F, 30.0F, 70.0F, 100.0F};
    CHECK_THAT(rolloff.gain(0.0F), WithinAbs(1.0, 1e-6));
    CHECK_THAT(rolloff.gain(10.0F), WithinAbs(1.0, 1e-6));
    CHECK_THAT(rolloff.gain(20.0F), WithinAbs(0.5, 1e-6));
    CHECK_THAT(rolloff.gain(30.0F), WithinAbs(1.0 / 3.0, 1e-6));
    CHECK_THAT(rolloff.gain(50.0F), WithinAbs(1.0 / 3.0, 1e-6));
    CHECK_THAT(rolloff.gain(85.0F), WithinAbs(1.0 / 6.0, 1e-6));
    CHECK_THAT(rolloff.gain(100.0F), WithinAbs(0.0, 1e-6));
    CHECK_THAT(rolloff.gain(500.0F), WithinAbs(0.0, 1e-6));
    const Rolloff inverse{1.0F, 10000.0F};
    CHECK_THAT(inverse.gain(4.0F), WithinAbs(0.25, 1e-6));
}

TEST_CASE("mixerSpatialVoices", "[audio]")
{
    Mixer mixer(48000);
    mixer.setListener({0.0F, 0.0F, 0.0F}, {1.0F, 0.0F, 0.0F});
    VoiceSettings settings;
    settings.loop = true;
    settings.spatial = true;
    settings.position = {10.0F, 0.0F, 0.0F};
    settings.rolloff = Rolloff{1.0F, 100.0F};
    const VoiceId voice = mixer.play(constant(0.5F, 4800), settings);
    std::vector<float> out = render(mixer, 512);
    CHECK_THAT(out[400], WithinAbs(0.0, 1e-5));
    CHECK_THAT(out[401], WithinAbs(0.05, 1e-4));

    // Turning the listener around swaps the sides.
    mixer.setListener({0.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F});
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_THAT(out[400], WithinAbs(0.05, 1e-4));
    CHECK_THAT(out[401], WithinAbs(0.0, 1e-5));

    mixer.setPosition(voice, {0.0F, 0.0F, 2.0F});
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_THAT(out[400], WithinAbs(0.25 * std::cos(std::numbers::pi / 4.0), 1e-4));
    CHECK_THAT(out[401], WithinAbs(0.25 * std::cos(std::numbers::pi / 4.0), 1e-4));
}

TEST_CASE("mixerBusesFilterAndScale", "[audio]")
{
    Mixer mixer(48000);
    const BusId bus = mixer.createBus();
    CHECK(bus != kMasterBus);
    mixer.setBusVolume(bus, 0.5F);
    VoiceSettings settings;
    settings.loop = true;
    settings.bus = bus;
    mixer.play(constant(0.5F, 4800), settings);
    std::vector<float> out = render(mixer, 512);
    CHECK_THAT(out[100], WithinAbs(0.25 * std::cos(std::numbers::pi / 4.0), 1e-4));

    Mixer filtered(48000);
    const BusId lowpassBus = filtered.createBus();
    FilterSpec lowpass;
    lowpass.type = FilterSpec::Type::Lowpass;
    lowpass.frequency = 500.0F;
    filtered.setBusFilters(lowpassBus, std::span<const FilterSpec>(&lowpass, 1));
    settings.bus = lowpassBus;
    filtered.play(sine(8000.0F, 48000, 1.0F), settings);
    render(filtered, 4800);
    out = render(filtered, 4800);
    float peak = 0.0F;
    for (const float s : out) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak < 0.005F);

    // Destroying a bus stops its voices.
    filtered.destroyBus(lowpassBus);
    CHECK(filtered.activeVoiceCount() == std::size_t{0});
}

TEST_CASE("mixerBusesFeedOtherBuses", "[audio]")
{
    Mixer mixer(48000);
    // A category bus with a car's emitter bus feeding it, created in either
    // order of slot numbers.
    const BusId spare = mixer.createBus();
    const BusId category = mixer.createBus();
    mixer.destroyBus(spare);
    const BusId emitter = mixer.createBus(category);
    CHECK(emitter < category);
    mixer.setBusVolume(category, 0.5F);
    mixer.setBusVolume(emitter, 0.8F);
    CHECK_THAT(mixer.busGain(emitter), WithinAbs(0.4, 1e-6));
    CHECK_THAT(mixer.busVolume(emitter), WithinAbs(0.8, 1e-6));
    VoiceSettings settings;
    settings.loop = true;
    settings.bus = emitter;
    mixer.play(constant(0.5F, 4800), settings);
    std::vector<float> out = render(mixer, 512);
    CHECK_THAT(out[100], WithinAbs(0.2 * std::cos(std::numbers::pi / 4.0), 1e-4));

    mixer.setBusVolume(category, 0.0F);
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_THAT(out[100], WithinAbs(0.0, 1e-6));
    CHECK_THAT(mixer.busGain(emitter), WithinAbs(0.0, 1e-6));

    // Without its output, a bus feeds the master.
    mixer.destroyBus(category);
    CHECK_THAT(mixer.busGain(emitter), WithinAbs(0.8, 1e-6));
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_THAT(out[100], WithinAbs(0.4 * std::cos(std::numbers::pi / 4.0), 1e-4));
    CHECK(mixer.busGain(category) == 0.0F);
}

TEST_CASE("mixerLimiterHoldsTheCeiling", "[audio]")
{
    Mixer mixer(48000);
    VoiceSettings settings;
    settings.loop = true;
    settings.pan = -1.0F;
    for (int i = 0; i < 8; ++i) {
        mixer.play(constant(0.5F, 4800), settings);
    }
    const std::vector<float> out = render(mixer, 4096);
    float peak = 0.0F;
    for (const float s : out) {
        peak = std::max(peak, std::abs(s));
    }
    CHECK(peak <= 0.98F + 1e-5F);
    CHECK(peak > 0.9F);
}

TEST_CASE("mixerControlFromAnotherThread", "[audio]")
{
    Mixer mixer(48000);
    VoiceSettings settings;
    settings.loop = true;
    const VoiceId voice = mixer.play(sine(440.0F, 48000, 0.5F), settings);
    std::atomic<bool> done{false};
    std::thread game([&] {
        int i = 0;
        while (!done) {
            mixer.setPitch(voice, 0.5F + static_cast<float>(i % 100) / 100.0F);
            mixer.setVolume(voice, static_cast<float>(i % 7) / 7.0F);
            const VoiceId shot = mixer.play(constant(0.1F, 64), VoiceSettings{});
            mixer.stop(shot);
            ++i;
        }
    });
    for (int block = 0; block < 400; ++block) {
        render(mixer, 256);
    }
    done = true;
    game.join();
    CHECK(mixer.isPlaying(voice));
}

TEST_CASE("audioDeviceRendersOfflineAndOpensTheDummyDriver", "[audio]")
{
    AudioDevice offline{AudioDevice::Offline{}};
    CHECK(!offline.isLive());
    VoiceSettings settings;
    settings.loop = true;
    offline.mixer().play(constant(0.5F, 480), settings);
    const std::vector<float> out = offline.render(480);
    CHECK(out.size() == std::size_t{960});
    CHECK(std::abs(out[500]) > 0.3F);

    // The dummy driver exercises the SDL path without making a sound.
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy");
    AudioDevice live;
    CHECK(live.isLive());
    const VoiceId voice = live.mixer().play(constant(0.5F, 4800), VoiceSettings{});
    for (int i = 0; i < 100 && live.mixer().isPlaying(voice); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!live.mixer().isPlaying(voice));
    CHECK_THROWS_AS(live.render(16), std::exception);
}

TEST_CASE("tuningCurvesWeighInputsAndInterpolate", "[audio]")
{
    TuningCurve curve;
    curve.rpm = 0.3F;
    curve.throttle = 0.7F;
    curve.points = {glm::vec2{0.0F, 0.2F}, glm::vec2{0.5F, 0.6F}, glm::vec2{1.0F, 0.8F}};
    EngineInputs in;
    in.rpm = 1.0F;
    in.throttle = 0.0F;
    CHECK_THAT(curve.input(in), WithinAbs(0.3, 1e-6));
    CHECK_THAT(curve.evaluate(in), WithinAbs(0.2 + 0.4 * 0.6, 1e-6));
    in.throttle = 1.0F;
    CHECK_THAT(curve.input(in), WithinAbs(1.0, 1e-6));
    CHECK_THAT(curve.evaluate(in), WithinAbs(0.8, 1e-6));
    CHECK_THAT(curve.at(-1.0F), WithinAbs(0.2, 1e-6));
    CHECK_THAT(curve.at(0.75F), WithinAbs(0.7, 1e-6));

    // All-zero weights and repeated x values select y0, as the disc's
    // constant EQ frequencies need.
    TuningCurve flat;
    flat.points = {glm::vec2{0.0F, 4328.0F}, glm::vec2{0.0F, 20.0F}, glm::vec2{0.0F, 20.0F}};
    CHECK_THAT(flat.evaluate(EngineInputs{}), WithinAbs(4328.0, 1e-3));
}

constexpr const char* kHarmonicXml = R"(
<Soundbank name="8N2_Test_Exh.fsb">
  <ExhaustL>
    <Loop wavebankindex="1" rpm_min="1000" rpm_sample="2000" rpm_max="3000" volume="0.5"
          pitch="1" pitchMin="1" pitchMax="1"/>
    <Loop wavebankindex="0" rpm_min="500" rpm_sample="900" rpm_max="1500" volume="0.8"
          pitch="1" pitchMin="1" pitchMax="1"/>
  </ExhaustL>
  <ExhaustR>
    <Loop wavebankindex="2" rpm_min="500" rpm_sample="900" rpm_max="1500" volume="0.8"
          pitch="1" pitchMin="1" pitchMax="1"/>
  </ExhaustR>
  <ExhaustNoise Volume="0.2"/>
  <Burble>
    <Wavebank name="EvoPops.fsb" burbles="9" backfires="4"/>
    <Volume>
      <PhysicsCoeff RPM="1" Throttle="0" PosTorque="0" NegTorque="0"/>
      <ThreePointCurve x0="0" y0="0.1" x1="0.9" y1="0.3" x2="1" y2="0.5"/>
    </Volume>
    <Timer MinTime="107" MaxTime="318" MinRPM="7098" MaxRPM="9067" SilenceWeighting="66"/>
  </Burble>
  <DSP>
    <Volume><Gain><PhysicsCoeff RPM="0" Throttle="1" PosTorque="0" NegTorque="0"/>
      <ThreePointCurve x0="0" y0="0.3" x1="0.5" y1="0.45" x2="1" y2="0.55"/></Gain></Volume>
    <PEQ Active="1">
      <Gain>
        <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
        <ThreePointCurve x0="0" y0="1.5" x1="0.5" y1="0.7" x2="1" y2="1"/>
      </Gain>
      <CenterFrequency>
        <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
        <ThreePointCurve x0="0" y0="150" x1="0" y1="20" x2="0" y2="530"/>
      </CenterFrequency>
      <Bandwidth>
        <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
        <ThreePointCurve x0="0" y0="2" x1="0" y1="0.2" x2="0" y2="5"/>
      </Bandwidth>
    </PEQ>
    <LoadPEQ>
      <PosLoad Active="1">
        <Gain>
          <PhysicsCoeff RPM="0" Throttle="0" PosTorque="1" NegTorque="0"/>
          <ThreePointCurve x0="0" y0="1" x1="0.5" y1="1.27" x2="1" y2="1.54"/>
        </Gain>
        <CenterFrequency>
          <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
          <ThreePointCurve x0="0" y0="273" x1="0.5" y1="791" x2="1" y2="880"/>
        </CenterFrequency>
        <Bandwidth>
          <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
          <ThreePointCurve x0="0" y0="3" x1="0.5" y1="5" x2="1" y2="5"/>
        </Bandwidth>
      </PosLoad>
      <NegLoad Active="0"/>
    </LoadPEQ>
    <Lowpass Active="1">
      <CutoffFrequency>
        <PhysicsCoeff RPM="0" Throttle="1" PosTorque="0" NegTorque="0"/>
        <ThreePointCurve x0="0" y0="13751" x1="0.6" y1="16051" x2="1" y2="19913"/>
      </CutoffFrequency>
      <Resonance>
        <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
        <ThreePointCurve x0="0" y0="1" x1="0.5" y1="1" x2="1" y2="1"/>
      </Resonance>
    </Lowpass>
  </DSP>
</Soundbank>)";

constexpr const char* kEngineXml = R"(
<Engine>
  <EngineSettings audio_rpm_idle="850" audio_rpm_redline="7500" num_cylinders="8"/>
  <VolumeAdjustments EngineVolume="0.8" TransmissionVolume="0.37" ShiftVolume="0.4" BackfireVolume="0.65"
                     BurbleVolume="1"/>
  <GlobalCarVolume>
    <PhysicsCoeff RPM="0.6" Throttle="0.5" PosTorque="0" NegTorque="0.15"/>
    <ThreePointCurve x0="0" y0="0.55" x1="0.5" y1="0.68" x2="1" y2="0.85"/>
  </GlobalCarVolume>
  <EngineAmbient><Upgrade L0="8N1_A_EngAmb_HT.xml"/></EngineAmbient>
  <EngineIntake><Upgrade L0="8N3_B_Int_HT.xml" L1="NA.xml" L2="8N3_C_Int_HT.xml" L3="NA.xml"/></EngineIntake>
  <Exhaust>
    <Upgrade L0="8N2_D_Exh_HT.xml" L1="8N2_D_Exh_HT.xml" L2="8N2_D_Exh_HT.xml" L3="8N2_E_Exh_HT.xml"/>
  </Exhaust>
  <HarmonicTunings>
    <EmissionGroup2>
      <Volume>
        <Gain>
          <PhysicsCoeff RPM="0" Throttle="0" PosTorque="0" NegTorque="0"/>
          <ThreePointCurve x0="0" y0="0.9" x1="0.5" y1="0.9" x2="1" y2="0.9"/>
        </Gain>
      </Volume>
    </EmissionGroup2>
  </HarmonicTunings>
  <ShiftVolumeScalar ShiftVolBoostUpPct="2" ShiftVolBoostUpTime="0.7" ShiftVolBoostDownPct="2.1"
                     ShiftVolBoostDownTime="0.6"/>
</Engine>)";

constexpr const char* kModelXml = R"(
<Car>
  <Chassis>
    <ExhaustL><OriginPosition x="-0.415" y="-0.2" z="-1.9"/></ExhaustL>
    <ExhaustR><OriginPosition x="0.415" y="-0.2" z="-1.9"/></ExhaustR>
  </Chassis>
  <Listener>
    <Basic>
      <Follow>
        <EmissionGroupVolumes EngineIntake="0.7" EngineAmbient="0.7" ExhaustL="1" ExhaustR="0.9"/>
        <OtherVolumes TireVolume="0.6" TransmissionVolume="0.6"/>
        <SurroundPanExhaustL Pan3d="0" Left="1" Right="0" Center="0.6" Lfe="0.8" LeftRear="0.5" RightRear="0"/>
        <SurroundPanEngineIntake Pan3d="0" Left="0.6" Right="0.6" Center="1" Lfe="0.5" LeftRear="0" RightRear="0"/>
      </Follow>
    </Basic>
  </Listener>
</Car>)";

TEST_CASE("engineTuningFilesParse", "[audio]")
{
    const HarmonicTuning harmonic = HarmonicTuning::parse(kHarmonicXml, "test_ht.xml");
    CHECK(harmonic.bank == std::string("8N2_Test_Exh.fsb"));
    CHECK(harmonic.loops.size() == std::size_t{2});
    const std::vector<EngineLoop>& left = harmonic.loops.at("ExhaustL");
    CHECK(left.size() == std::size_t{2});
    CHECK(left[0].sampleIndex == 0U);
    CHECK_THAT(left[1].rpmSample, WithinAbs(2000.0, 1e-3));
    CHECK_THAT(harmonic.exhaustNoiseVolume, WithinAbs(0.2, 1e-6));
    CHECK(harmonic.burble.has_value());
    if (harmonic.burble) {
        CHECK(harmonic.burble->burbles == 9U);
        CHECK(harmonic.burble->backfires == 4U);
        CHECK(harmonic.burble->silenceWeighting == 66);
        CHECK_THAT(harmonic.burble->maxRpm, WithinAbs(9067.0, 1e-3));
    }
    EngineInputs in;
    in.throttle = 1.0F;
    in.posTorque = 1.0F;
    const std::vector<FilterSpec> filters = harmonic.dsp.filters(in);
    CHECK(filters.size() == std::size_t{3});
    if (filters.size() == 3) {
        CHECK(filters[0].type == FilterSpec::Type::Peaking);
        CHECK_THAT(filters[0].frequency, WithinAbs(150.0, 1e-3));
        CHECK_THAT(filters[0].gain, WithinAbs(1.5, 1e-6));
        CHECK_THAT(filters[1].gain, WithinAbs(1.54, 1e-5));
        CHECK(filters[2].type == FilterSpec::Type::Lowpass);
        CHECK_THAT(filters[2].frequency, WithinAbs(19913.0, 1e-2));
    }
    CHECK_THAT(harmonic.dsp.volume.evaluate(in), WithinAbs(0.55, 1e-6));

    const EngineTuning engine = EngineTuning::parse(kEngineXml, "test_et.xml");
    CHECK_THAT(engine.idleRpm, WithinAbs(850.0, 1e-3));
    CHECK_THAT(engine.redlineRpm, WithinAbs(7500.0, 1e-3));
    CHECK(engine.cylinders == 8);
    CHECK_THAT(engine.shiftVolume, WithinAbs(0.4, 1e-6));
    CHECK(engine.upgradeFile(EmissionGroup::Intake, 0) == std::string("8N3_B_Int_HT.xml"));
    CHECK(engine.upgradeFile(EmissionGroup::Intake, 1) == std::string("8N3_B_Int_HT.xml"));
    CHECK(engine.upgradeFile(EmissionGroup::Intake, 3) == std::string("8N3_C_Int_HT.xml"));
    CHECK(engine.upgradeFile(EmissionGroup::Ambient, 3) == std::string("8N1_A_EngAmb_HT.xml"));
    CHECK(engine.upgradeFile(EmissionGroup::Exhaust, 3) == std::string("8N2_E_Exh_HT.xml"));
    EngineTuning raceCar = engine;
    raceCar.upgrades[0] = {"NA.xml", "NA_Int_HT.xml", "NA.xml", "12N3_Race_Int_HT.xml"};
    CHECK(raceCar.upgradeFile(EmissionGroup::Intake, 0) == std::string("12N3_Race_Int_HT.xml"));
    raceCar.upgrades[0] = {"NA.xml", "NA.xml", "NA.xml", "NA.xml"};
    CHECK(raceCar.upgradeFile(EmissionGroup::Intake, 2).empty());
    CHECK(!engine.groupDsp[0].has_value());
    CHECK(engine.groupDsp[2].has_value());
    CHECK_THAT(engine.shiftBoostDownTime, WithinAbs(0.6, 1e-6));
    CHECK_THROWS_AS(EngineTuning::parse("<Engine><EngineSettings/></Engine>", "broken_et.xml"), std::exception);
    CHECK_THROWS_AS(EngineTuning::parse("<Engine", "bad_et.xml"), std::exception);

    const CarModelTuning model = CarModelTuning::parse(kModelXml, "test_cmt.xml");
    CHECK_THAT(model.emitterPositions.at("ExhaustL").x, WithinAbs(-0.415, 1e-6));
    const CarModelTuning::View* follow = model.view("Follow");
    CHECK(follow != nullptr);
    if (follow) {
        CHECK_THAT(follow->emitterVolumes.at("ExhaustR"), WithinAbs(0.9, 1e-6));
        CHECK_THAT(follow->otherVolumes.at("TireVolume"), WithinAbs(0.6, 1e-6));
        CHECK(follow->stereoPan("ExhaustL") < -0.3F);
        CHECK_THAT(follow->stereoPan("EngineIntake"), WithinAbs(0.0, 1e-6));
        CHECK_THAT(follow->stereoPan("Missing"), WithinAbs(0.0, 1e-6));
    }
    CHECK(model.view("Cockpit") == nullptr);
}

TEST_CASE("globalRolloffsAndBackfiresParse", "[audio]")
{
    const auto rolloffs = parseGlobalRolloffs(R"(<GlobalRolloffs>
        <Burble><Gameplay minDist="5" maxDist="7" beginFadeToZeroDist="9" fadeToZeroDist="11"/></Burble>
        <Chassis><ExhaustL><Gameplay minDist="10" maxDist="30" beginFadeToZeroDist="70" fadeToZeroDist="100"/>
        <Replay minDist="1" maxDist="2" beginFadeToZeroDist="3" fadeToZeroDist="4"/></ExhaustL></Chassis>
        </GlobalRolloffs>)",
        "rolloffs.xml");
    CHECK(rolloffs.size() == std::size_t{2});
    CHECK_THAT(rolloffs.at("ExhaustL").fadeStart, WithinAbs(70.0, 1e-6));
    CHECK_THAT(rolloffs.at("Burble").maxDistance, WithinAbs(7.0, 1e-6));

    const std::vector<BackfireLevel> levels = parseBackfireConfig(R"(<BackfireTimerConfig>
          <Backfire><Timer MinTime="0" MaxTime="0" MinNormRPM="0" MaxNormRPM="0" SilenceWeighting="100"/></Backfire>
          <Backfire>
            <Timer MinTime="900" MaxTime="1800" MinNormRPM="0.6" MaxNormRPM="0.75" SilenceWeighting="95"/>
          </Backfire>
        </BackfireTimerConfig>)",
        R"(<BackfireVolumeConfig>
          <Backfire><Volume/></Backfire>
          <Backfire>
            <Volume>
              <PhysicsCoeff RPM="1"/>
              <ThreePointCurve x0="0" y0="0.1" x1="0.5" y1="0.5" x2="1" y2="1"/>
            </Volume>
          </Backfire>
        </BackfireVolumeConfig>)",
        "backfire.xml");
    CHECK(levels.size() == std::size_t{2});
    CHECK_THAT(levels[1].maxTime, WithinAbs(1800.0, 1e-3));
    CHECK(levels[1].silenceWeighting == 95);
    EngineInputs in;
    in.rpm = 1.0F;
    CHECK_THAT(levels[1].volume.evaluate(in), WithinAbs(1.0, 1e-6));
}

TEST_CASE("loopCrossfadeWeights", "[audio]")
{
    std::vector<EngineLoop> loops(3);
    loops[0] = EngineLoop{0, 500.0F, 1000.0F, 1500.0F, 1.0F, 1.0F};
    loops[1] = EngineLoop{1, 1200.0F, 1700.0F, 2200.0F, 1.0F, 1.0F};
    loops[2] = EngineLoop{2, 2400.0F, 2800.0F, 3000.0F, 1.0F, 1.0F};
    const auto check = [&](float rpm, std::vector<float> expected) {
        const std::vector<float> weights = EngineSound::loopWeights(loops, rpm);
        CHECK(weights.size() == expected.size());
        for (std::size_t i = 0; i < std::min(weights.size(), expected.size()); ++i) {
            CHECK_THAT(weights[i], WithinAbs(expected[i], 1e-4));
        }
    };
    check(800.0F, {1.0F, 0.0F, 0.0F});
    check(1350.0F, {std::numbers::sqrt2_v<float> / 2.0F, std::numbers::sqrt2_v<float> / 2.0F, 0.0F});
    check(2000.0F, {0.0F, 1.0F, 0.0F});
    check(2300.0F, {0.0F, 0.0F, 1.0F});
    check(100.0F, {1.0F, 0.0F, 0.0F});
    check(9000.0F, {0.0F, 0.0F, 1.0F});
    for (float rpm = 0.0F; rpm < 4000.0F; rpm += 37.0F) {
        float sum = 0.0F;
        for (const float w : EngineSound::loopWeights(loops, rpm)) {
            sum += w * w;
        }
        CHECK_THAT(sum, WithinAbs(1.0, 1e-4));
    }
    CHECK(EngineSound::loopWeights({}, 1000.0F).empty());
}

TEST_CASE("loopIndicesCountChannels", "[audio]")
{
    const std::optional<LoopSource> left = resolveLoopIndex(3, 15, 2);
    const std::optional<LoopSource> right = resolveLoopIndex(18, 15, 2);
    const std::optional<LoopSource> mono = resolveLoopIndex(18, 15, 1);
    CHECK((left && right && mono));
    if (left && right && mono) {
        CHECK(left->sample == std::size_t{3});
        CHECK(left->channel == 0);
        CHECK(right->sample == std::size_t{3});
        CHECK(right->channel == 1);
        CHECK(mono->channel == 0);
    }
    CHECK(!resolveLoopIndex(30, 15, 2).has_value());
}

std::vector<std::string> filesUnder(const openhorizon::Vfs& vfs, std::string_view suffix)
{
    std::vector<std::string> files;
    for (const std::string& path : vfs.list("media")) {
        if (openhorizon::endsWithNoCase(path, suffix)) {
            files.push_back(path);
        }
    }
    return files;
}

/// Whether sample `index` of `bank` decodes to its full length; why not goes
/// to stderr.
bool decodesWhole(const FsbBank& bank, std::size_t index, std::string_view path)
{
    try {
        const PcmBuffer pcm = decodeSample(bank, index);
        if (pcm.frames() == bank.sample(index).sampleCount) {
            return true;
        }
        std::fprintf(stderr, "%.*s sample %zu: %zu of %u frames\n", static_cast<int>(path.size()), path.data(), index,
            pcm.frames(), bank.sample(index).sampleCount);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "decode error: %s\n", e.what());
    }
    return false;
}

// discDecodesEverySample takes most of a minute, nearly all of it in the
// radio's MPEG banks. These cover the kinds of sample it meets: XMA mono
// and stereo at the engine loops' 36 and 48 kHz (an exhaust bank, as most
// banks are), XMA at odd rates from a track's bin.zip (the festival's
// ambience), and MPEG, one sample of each channel count and rate the
// radio's voice bank holds, sync points among them.
TEST_CASE("discDecodesRepresentativeBanks", "[audio]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    for (const char* path :
        {"media/audio/cars/engines/soundbanks/lod1/2rt2_rx7_exh.fsb", "media/tracks/colorado/bin/amb_festival.fsb"}) {
        const FsbBank bank = FsbBank::parse(vfs.readOrThrow(path), path);
        std::set<int> channels;
        for (std::size_t i = 0; i < bank.size(); ++i) {
            CHECK(bank.sample(i).codec == SampleCodec::Xma);
            channels.insert(bank.sample(i).channels);
            CHECK(decodesWhole(bank, i, path));
        }
        CHECK((channels == std::set<int>{1, 2}));
    }

    const char* radio = "media/audio/radio/radio_vo_en.fsb";
    const FsbBank voice = FsbBank::parse(vfs.readOrThrow(radio), radio);
    std::set<std::pair<int, int>> kinds;
    bool syncPoints = false;
    for (std::size_t i = 0; i < voice.size(); ++i) {
        const FsbSample& sample = voice.sample(i);
        CHECK(sample.codec == SampleCodec::Mpeg);
        if (kinds.insert({sample.channels, sample.frequency}).second) {
            syncPoints = syncPoints || !sample.syncPoints.empty();
            CHECK(decodesWhole(voice, i, radio));
        }
    }
    CHECK(kinds.size() > 1);
    CHECK(syncPoints);
}

TEST_CASE("discDecodesEverySample", "[audio][slow][.]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const std::vector<std::string> banks = filesUnder(vfs, ".fsb");
    CHECK(banks.size() == std::size_t{1006});

    std::mutex mutex;
    std::map<std::string, std::size_t> decoded;
    std::size_t samples = 0;
    std::vector<std::string> errors;
    std::atomic<std::size_t> next{0};
    const auto work = [&] {
        for (std::size_t b = next++; b < banks.size(); b = next++) {
            std::map<std::string, std::size_t> local;
            std::size_t count = 0;
            std::vector<std::string> localErrors;
            try {
                const FsbBank bank = FsbBank::parse(vfs.readOrThrow(banks[b]), banks[b]);
                count = bank.size();
                for (std::size_t i = 0; i < bank.size(); ++i) {
                    try {
                        const PcmBuffer pcm = decodeSample(bank, i);
                        if (pcm.frames() == bank.sample(i).sampleCount) {
                            ++local[codecName(bank.sample(i).codec)];
                        }
                    } catch (const std::exception& e) {
                        localErrors.emplace_back(e.what());
                    }
                }
            } catch (const std::exception& e) {
                localErrors.emplace_back(e.what());
            }
            std::lock_guard lock(mutex);
            samples += count;
            for (const auto& [codec, n] : local) {
                decoded[codec] += n;
            }
            errors.insert(errors.end(), localErrors.begin(), localErrors.end());
        }
    };
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < std::max(1U, std::thread::hardware_concurrency()); ++t) {
        threads.emplace_back(work);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (const std::string& error : errors) {
        std::fprintf(stderr, "decode error: %s\n", error.c_str());
    }
    CHECK(errors.empty());
    CHECK(samples == std::size_t{22393});
    CHECK(decoded["xma"] == std::size_t{14790});
    CHECK(decoded["mpeg"] == std::size_t{7603});
}

TEST_CASE("discProjectsParseAndResolve", "[audio]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    AudioLibrary library(vfs);
    const std::vector<std::string> projects = filesUnder(vfs, ".fev");
    CHECK(projects.size() == std::size_t{54});
    std::size_t events = 0;
    std::size_t wavetables = 0;
    std::size_t namedAtIndex = 0;
    std::size_t unresolved = 0;
    for (const std::string& path : projects) {
        std::shared_ptr<const FevProject> project;
        try {
            project = library.project(path);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "%s\n", e.what());
            CHECK(false);
            continue;
        }
        events += project->events().size();
        for (const FevSoundDefinition& definition : project->soundDefinitions()) {
            for (const FevWaveform& waveform : definition.waveforms) {
                if (waveform.type != FevWaveform::Type::Wavetable) {
                    continue;
                }
                ++wavetables;
                const std::string bankPath = AudioLibrary::bankPathFor(path, waveform.bankName);
                try {
                    const std::shared_ptr<const FsbBank> bank = library.bank(bankPath);
                    const std::size_t index = library.resolveWaveform(*bank, waveform);
                    namedAtIndex += index == waveform.sampleIndex && bank->sampleHasName(index, waveform.fileName);
                } catch (const std::exception& e) {
                    ++unresolved;
                    std::fprintf(stderr, "%s\n", e.what());
                }
            }
        }
    }
    std::fprintf(stderr, "%zu events, %zu wavetable waveforms, %zu named at their index\n", events, wavetables,
        namedAtIndex);
    CHECK(events == std::size_t{1472});
    CHECK(wavetables == std::size_t{6315});
    // Two banks were rebuilt smaller than their projects expect: Damage.fsb
    // (2 samples, 2 waveforms point past it) and amb_main_town.fsb (5 night
    // dog waveforms point past its 28 samples). Everything else resolves,
    // and 6251 waveforms find their own file name at their index.
    CHECK(unresolved == std::size_t{7});
    CHECK(namedAtIndex == std::size_t{6251});

    const std::shared_ptr<const FevProject> horns = library.project("media/audio/cars/horns/horns.fev");
    const FevEvent* horn = horns->findEvent("Horns/HornA_Short");
    CHECK(horn != nullptr);
    if (horn) {
        AudioDevice device{AudioDevice::Offline{}};
        EventInstance instance(device.mixer(), library, "media/audio/cars/horns/horns.fev", horns, *horn);
        instance.start();
        CHECK(instance.isPlaying());
        const std::vector<float> out = device.render(24000);
        double energy = 0.0;
        for (const float s : out) {
            energy += static_cast<double>(s) * s;
        }
        CHECK(std::sqrt(energy / static_cast<double>(out.size())) > 0.01);
    }
}

TEST_CASE("discEngineTuningsLoad", "[audio]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    std::size_t loaded = 0;
    std::size_t total = 0;
    for (const std::string& path : vfs.list("media/audio/cars/engines/enginetuning")) {
        const std::string file = path.substr(path.find_last_of('/') + 1);
        if (!file.ends_with("_et.xml")) {
            continue;
        }
        ++total;
        try {
            const CarAudioTuning car = CarAudioTuning::load(vfs, file.substr(0, file.size() - 7));
            if (car.harmonics[0] || car.harmonics[2]) {
                ++loaded;
            } else {
                std::fprintf(stderr, "no intake or exhaust: %s\n", path.c_str());
            }
        } catch (const openhorizon::DataError& e) {
            std::fprintf(stderr, "not loaded: %s\n", e.what());
        }
    }
    std::fprintf(stderr, "%zu of %zu engine tunings load\n", loaded, total);
    // The electric Volt, Leaf and Roadster have no harmonic tunings at all;
    // the traffic bus, van and truck have only engine-ambient loops.
    CHECK(total == std::size_t{807});
    CHECK(loaded == std::size_t{801});
}

/// Four wheels in the same state, for the tyre tests.
std::array<TyreInput, TyreSound::kWheels> sameWheels(std::string_view surface, bool onGround, float spin,
    float slipAngle, float slipRatio, float combinedSlip, float load)
{
    std::array<TyreInput, TyreSound::kWheels> wheels{};
    for (TyreInput& wheel : wheels) {
        wheel.onGround = onGround;
        wheel.surface = surface;
        wheel.spin = spin;
        wheel.slipAngle = slipAngle;
        wheel.slipRatio = slipRatio;
        wheel.combinedSlip = combinedSlip;
        wheel.load = load;
    }
    return wheels;
}

double renderRms(AudioDevice& device, const std::function<void()>& update, float seconds)
{
    double energy = 0.0;
    std::size_t count = 0;
    for (int i = 0; i < static_cast<int>(seconds * 60.0F); ++i) {
        update();
        for (const float sample : device.render(800)) {
            energy += static_cast<double>(sample) * sample;
            ++count;
        }
    }
    return std::sqrt(energy / static_cast<double>(std::max<std::size_t>(count, 1)));
}

TEST_CASE("discTyreSoundFollowsTheGame", "[audio]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const auto library = std::make_shared<AudioLibrary>(vfs);
    AudioDevice device{AudioDevice::Offline{}};
    TyreSound tyres(device.mixer(), library);

    const TyreSmoothing& smoothing = tyres.smoothing();
    CHECK_THAT(smoothing.combinedSlipRate, WithinAbs(10.0, 1e-6));
    CHECK_THAT(smoothing.slipAngleRate, WithinAbs(10.0, 1e-6));
    CHECK_THAT(smoothing.slipRatioRate, WithinAbs(10.0, 1e-6));
    CHECK_THAT(smoothing.normLoadRate, WithinAbs(3.0, 1e-6));
    CHECK_THAT(smoothing.tireRpmRate, WithinAbs(500000.0, 1e-2));
    CHECK_THAT(smoothing.timeUntilLeavesGround, WithinAbs(0.15, 1e-6));
    CHECK(tyres.surfaces().surfaces.size() == std::size_t{42});
    const std::optional<std::size_t> asphalt = tyres.surfaces().find("Asphalt");
    CHECK(asphalt.has_value());

    constexpr float kMass = 1500.0F;
    const float quarterWeight = kMass * 9.80665F / 4.0F;
    TyreCarInput car;
    car.mass = kMass;
    constexpr float kDt = 0.1F;

    // Slips move at 10 units per second, load at 3, and rpm at once.
    tyres.update(sameWheels("Asphalt", true, 100.0F, 3.0F, 2.0F, 4.0F, 2.0F * quarterWeight), car, kDt);
    const TyreSound::Parameters& front = tyres.parameters(0);
    CHECK_THAT(front.slipAngle, WithinAbs(1.0, 1e-4));
    CHECK_THAT(front.slipRatio, WithinAbs(1.0, 1e-4));
    CHECK_THAT(front.combinedSlip, WithinAbs(1.0, 1e-4));
    CHECK_THAT(front.normLoad, WithinAbs(0.3, 1e-4));
    CHECK_THAT(front.tireRpm, WithinAbs(100.0 * 60.0 / (2.0 * std::numbers::pi), 1e-2));
    CHECK((asphalt && front.surface == static_cast<int>(*asphalt)));
    CHECK(front.grounded);

    // The game's clamps: slips to 5, load to 3 quarter weights, rpm to
    // 20000; slip ratio flips sign in reverse.
    car.reversing = true;
    for (int i = 0; i < 20; ++i) {
        tyres.update(sameWheels("Asphalt", true, 5000.0F, -9.0F, 9.0F, 9.0F, 9.0F * quarterWeight), car, kDt);
    }
    CHECK_THAT(front.slipAngle, WithinAbs(5.0, 1e-4));
    CHECK_THAT(front.slipRatio, WithinAbs(-5.0, 1e-4));
    CHECK_THAT(front.combinedSlip, WithinAbs(5.0, 1e-4));
    CHECK_THAT(front.normLoad, WithinAbs(3.0, 1e-4));
    CHECK_THAT(front.tireRpm, WithinAbs(20000.0, 1e-2));
    car.reversing = false;

    // Briefly airborne, a wheel keeps its values and surface; after
    // TimeUntilTireLeavesGround it takes the raw ones and loses the surface.
    tyres.update(sameWheels("Asphalt", false, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F), car, kDt);
    CHECK(front.grounded);
    CHECK_THAT(front.slipAngle, WithinAbs(5.0, 1e-4));
    tyres.update(sameWheels("Asphalt", false, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F), car, kDt);
    CHECK(!front.grounded);
    CHECK(front.surface == -1);
    CHECK_THAT(front.slipAngle, WithinAbs(0.0, 1e-6));
    device.render(4800);
    CHECK(!tyres.isPlaying());

    // Standing still makes no sound; rolling plays the road noise alone, the
    // skid events staying below every skid layer's range; sliding is louder.
    const auto standing = sameWheels("Asphalt", true, 0.0F, 0.0F, 0.0F, 0.0F, quarterWeight);
    const double standingRms = renderRms(device, [&] { tyres.update(standing, car, 1.0F / 60.0F); }, 1.0F);
    CHECK(standingRms < 1e-4);
    const auto rolling = sameWheels("Asphalt", true, 60.0F, 0.05F, 0.05F, 0.07F, quarterWeight);
    const double rollingRms = renderRms(device, [&] { tyres.update(rolling, car, 1.0F / 60.0F); }, 1.0F);
    CHECK(rollingRms > 0.005);
    const auto sliding = sameWheels("Asphalt", true, 50.0F, 3.0F, -0.5F, 3.0F, quarterWeight);
    const double slidingRms = renderRms(device, [&] { tyres.update(sliding, car, 1.0F / 60.0F); }, 1.0F);
    CHECK(slidingRms > 2.0 * rollingRms);

    // A surface Tires.xml does not list has no tyre sounds.
    const auto kerb = sameWheels("Kerb", true, 60.0F, 3.0F, 0.0F, 3.0F, quarterWeight);
    renderRms(device, [&] { tyres.update(kerb, car, 1.0F / 60.0F); }, 0.5F);
    CHECK(!tyres.isPlaying());
    CHECK(tyres.parameters(0).surface == -1);
    CHECK(tyres.parameters(0).grounded);

    // LOD2 follows the most slipping wheel and needs some slip to sound.
    TyreSoundOptions single;
    single.perWheel = false;
    TyreSound pair(device.mixer(), library, single);
    const double stillRms = renderRms(device, [&] { pair.update(standing, car, 1.0F / 60.0F); }, 0.5F);
    CHECK(stillRms < 1e-4);
    CHECK(!pair.isPlaying());
    renderRms(device, [&] { pair.update(sliding, car, 1.0F / 60.0F); }, 0.5F);
    CHECK(pair.isPlaying());
}

TEST_CASE("discEngineSoundPlays", "[audio]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    AudioDevice device{AudioDevice::Offline{}};
    EngineSound engine(device, vfs, "ALF_8C_08");
    CHECK_THAT(engine.tuning().engine.redlineRpm, WithinAbs(7500.0, 1e-3));
    // Loops take a mixer voice only while their crossfade makes them audible,
    // so a car that is not yet running holds none of the mixer's 256.
    const std::size_t silentVoices = device.mixer().activeVoiceCount();
    CHECK(silentVoices < 8);

    const auto run = [&](float seconds, float rpm, float throttle) {
        double energy = 0.0;
        float peak = 0.0F;
        std::size_t count = 0;
        for (int i = 0; i < static_cast<int>(seconds * 60.0F); ++i) {
            engine.update(rpm, throttle, 1.0F / 60.0F);
            for (const float s : device.render(800)) {
                energy += static_cast<double>(s) * s;
                peak = std::max(peak, std::abs(s));
                ++count;
            }
        }
        return std::pair{std::sqrt(energy / static_cast<double>(count)), peak};
    };
    const auto [idleRms, idlePeak] = run(1.0F, 900.0F, 0.0F);
    const std::size_t idleVoices = device.mixer().activeVoiceCount();
    CHECK(idleVoices > silentVoices);
    CHECK(idleVoices < 40);
    const auto [highRms, highPeak] = run(1.0F, 6000.0F, 1.0F);
    CHECK(idleRms > 0.01);
    CHECK(highRms > 0.03);
    CHECK(idlePeak < 0.99F);
    CHECK(highPeak < 0.99F);

    run(0.2F, 7500.0F, 1.0F);
    CHECK(engine.limiterActive());

    // Lifting off inside the burble window fires one-shots on top of the loops.
    const std::size_t loops = device.mixer().activeVoiceCount();
    std::size_t most = loops;
    for (int i = 0; i < 180; ++i) {
        engine.update(7300.0F, 0.0F, 1.0F / 60.0F);
        device.render(800);
        most = std::max(most, device.mixer().activeVoiceCount());
    }
    CHECK(most > loops);

    engine.shiftGear(2, 3);
    CHECK(engine.tyres() != nullptr);
    std::array<TyreInput, TyreSound::kWheels> sliding{};
    for (TyreInput& wheel : sliding) {
        wheel.onGround = true;
        wheel.surface = "Asphalt";
        wheel.spin = 50.0F;
        wheel.slipAngle = 3.0F;
        wheel.combinedSlip = 3.0F;
        wheel.load = 3500.0F;
    }
    TyreCarInput car;
    car.mass = 1500.0F;
    engine.updateTyres(sliding, car, 1.0F / 60.0F);
    const auto [effectsRms, effectsPeak] = run(0.5F, 5000.0F, 1.0F);
    CHECK(effectsRms > 0.03);
    CHECK(effectsPeak < 0.99F);
    CHECK(device.mixer().activeVoiceCount() > loops);
    CHECK((engine.tyres() && engine.tyres()->isPlaying()));

    CHECK_THROWS_AS(EngineSound(device, vfs, "NOT_A_CAR_00"), std::exception);
}

/// A few snapshots in the layout of audiomixersnapshots.xml, with the disc's
/// values for the categories used.
constexpr std::string_view kSnapshotXml = R"(<?xml version="1.0" encoding="utf-8"?>
<MixSnapshots>
  <DynamicsData><CrashData threshold="50" drop="10"/></DynamicsData>
  <FreeRoam group="Gameplay">
    <FocusCar  active="1" destvolume="0.350000" fadetime="0.500000" returntime="0.000000" returnfadetime="0.000000"/>
    <GameMusic active="1" destvolume="1.000000" fadetime="0.500000" returntime="0.000000" returnfadetime="0.000000"/>
  </FreeRoam>
  <Loading group="Gameplay">
    <FocusCar  active="1" destvolume="0.00000" fadetime="2.000000" returntime="0.000000" returnfadetime="0.000000"/>
  </Loading>
  <Paused group="Gameplay">
    <FocusCar  active="1" destvolume="0.000000" fadetime="0.200000" returntime="0.000000" returnfadetime="0.100000"/>
    <GameMusic active="1" destvolume="0.2500000" fadetime="0.200000" returntime="0.000000" returnfadetime="0.100000"/>
    <GameUI    active="1" destvolume="1.000000" fadetime="0.000000" returntime="0.000000" returnfadetime="0.100000"/>
  </Paused>
  <Crash group="Impact" >
    <FocusCar  active="1" destvolume="0.700000" fadetime="0.000000" returntime="0.200000" returnfadetime="0.300000"/>
    <Collision active="0" destvolume="1.00000"  fadetime="0.000000" returntime="0.000000" returnfadetime="0.000000"/>
  </Crash>
  <TopSpeed group="Speed">
    <GameMusic active="1" destvolume="1.1500000" fadetime="0.000000" returntime="0.000000" returnfadetime="0.000000"/>
  </TopSpeed>
  <Video group="Elsewhere">
    <FocusCar  active="1" destvolume="0.000000" fadetime="0.000000" returntime="0.000000" returnfadetime="0.000000"/>
  </Video>
</MixSnapshots>
)";

/// Runs `updates` updates of `seconds` each.
void advance(SnapshotMixer& mixer, int updates, float seconds)
{
    for (int i = 0; i < updates; ++i) {
        mixer.update(seconds);
    }
}

TEST_CASE("mixSnapshotsParse", "[audio]")
{
    const MixSnapshots snapshots = MixSnapshots::parse(kSnapshotXml, "test");
    const MixSnapshot& paused = snapshots.snapshot(MixSnapshotId::Paused);
    CHECK(paused.present);
    CHECK(paused.group == MixGroup::Gameplay);
    const MixSetting& focus = paused.settings[static_cast<std::size_t>(MixCategory::FocusCar)];
    CHECK(focus.active);
    CHECK_THAT(focus.volume, WithinAbs(0.0, 1e-6));
    CHECK_THAT(focus.fadeTime, WithinAbs(0.2, 1e-6));
    CHECK_THAT(focus.returnFadeTime, WithinAbs(0.1, 1e-6));
    CHECK_THAT(paused.settings[static_cast<std::size_t>(MixCategory::GameMusic)].volume, WithinAbs(0.25, 1e-6));
    CHECK(!paused.settings[static_cast<std::size_t>(MixCategory::Tire)].active);
    CHECK(!snapshots.snapshot(MixSnapshotId::Crash).settings[static_cast<std::size_t>(MixCategory::Collision)].active);
    CHECK(!snapshots.snapshot(MixSnapshotId::Race).present);
    CHECK(!snapshots.snapshot(MixSnapshotId::Video).group.has_value());
    CHECK(findMixSnapshot("PausedMultiplayer") == MixSnapshotId::PausedMultiplayer);
    CHECK(!findMixSnapshot("paused").has_value());
    CHECK(mixCategoryName(MixCategory::RallyCoDriver) == std::string_view("RallyCoDriver"));
    CHECK_THROWS_AS(MixSnapshots::parse("<NotSnapshots/>", "test"), std::exception);
}

TEST_CASE("snapshotMixerFadesPausesAndRestores", "[audio]")
{
    SnapshotMixer mixer(MixSnapshots::parse(kSnapshotXml, "test"));
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(1.0, 1e-6));

    // A new fade covers the distance from the group's volume in fadetime,
    // starting one update after it was applied.
    mixer.apply(MixSnapshotId::FreeRoam);
    CHECK(mixer.current(MixGroup::Gameplay) == MixSnapshotId::FreeRoam);
    CHECK(!mixer.saved().has_value());
    mixer.update(0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(1.0, 1e-6));
    mixer.update(0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(1.0 - 0.65 * 0.2, 1e-5));
    advance(mixer, 5, 0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35, 1e-6));
    CHECK(mixer.faderCount() == std::size_t{0});

    // Pausing saves FreeRoam and fades the car out in 0.2 s and the music
    // down to a quarter; the UI stays.
    mixer.apply(MixSnapshotId::Paused);
    CHECK(mixer.saved() == MixSnapshotId::FreeRoam);
    mixer.update(0.05F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35, 1e-6));
    mixer.update(0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.175, 1e-5));
    CHECK_THAT(mixer.volume(MixCategory::GameMusic), WithinAbs(1.0 - 0.75 * 0.5, 1e-5));
    advance(mixer, 2, 0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.0, 1e-6));
    CHECK_THAT(mixer.volume(MixCategory::GameMusic), WithinAbs(0.25, 1e-6));
    CHECK_THAT(mixer.volume(MixCategory::GameUI), WithinAbs(1.0, 1e-6));

    // Leaving it goes back to FreeRoam over Paused's 0.1 s return fade, and
    // FreeRoam stays the one saved.
    mixer.restore();
    CHECK(mixer.current(MixGroup::Gameplay) == MixSnapshotId::FreeRoam);
    CHECK(mixer.saved() == MixSnapshotId::FreeRoam);
    mixer.update(0.05F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.0, 1e-6));
    mixer.update(0.05F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.175, 1e-5));
    advance(mixer, 2, 0.05F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35, 1e-6));

    // Loading is not saved either.
    mixer.apply(MixSnapshotId::Loading);
    mixer.apply(MixSnapshotId::Paused);
    CHECK(mixer.saved() == MixSnapshotId::FreeRoam);
}

TEST_CASE("snapshotMixerRestartsRunningFades", "[audio]")
{
    SnapshotMixer mixer(MixSnapshots::parse(kSnapshotXml, "test"));
    mixer.apply(MixSnapshotId::FreeRoam);
    advance(mixer, 10, 0.1F);
    mixer.apply(MixSnapshotId::Paused);
    advance(mixer, 2, 0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.175, 1e-5));
    // Still fading: the fader restarts from where the group is with a step
    // of |1 - target| per fade time.
    mixer.restore();
    mixer.update(0.02F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.175 + 0.65 * 0.2, 1e-5));
    advance(mixer, 2, 0.02F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35, 1e-6));
}

TEST_CASE("snapshotMixerGroupsMultiplyAndReturn", "[audio]")
{
    SnapshotMixer mixer(MixSnapshots::parse(kSnapshotXml, "test"));
    mixer.apply(MixSnapshotId::FreeRoam);
    advance(mixer, 10, 0.1F);

    // A crash dips the car in its own group, straight away after the timer
    // starts, and returns to 1 after 0.2 s over 0.3 s.
    mixer.apply(MixSnapshotId::Crash);
    mixer.update(0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35, 1e-6));
    mixer.update(0.1F);
    CHECK_THAT(mixer.groupVolume(MixGroup::Impact, MixCategory::FocusCar), WithinAbs(0.7, 1e-6));
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35 * 0.7, 1e-6));
    mixer.update(0.1F);
    CHECK_THAT(mixer.groupVolume(MixGroup::Impact, MixCategory::FocusCar), WithinAbs(0.7, 1e-6));
    mixer.update(0.1F);
    mixer.update(0.15F);
    CHECK_THAT(mixer.groupVolume(MixGroup::Impact, MixCategory::FocusCar), WithinAbs(0.85, 1e-5));
    advance(mixer, 2, 0.15F);
    CHECK_THAT(mixer.groupVolume(MixGroup::Impact, MixCategory::FocusCar), WithinAbs(1.0, 1e-6));
    CHECK(mixer.faderCount() == std::size_t{0});
    CHECK_THAT(mixer.groupVolume(MixGroup::Impact, MixCategory::Collision), WithinAbs(1.0, 1e-6));

    // The product is clamped to 1.
    mixer.apply(MixSnapshotId::TopSpeed);
    advance(mixer, 2, 0.1F);
    CHECK_THAT(mixer.groupVolume(MixGroup::Speed, MixCategory::GameMusic), WithinAbs(1.15, 1e-6));
    CHECK_THAT(mixer.volume(MixCategory::GameMusic), WithinAbs(1.0, 1e-6));

    // A snapshot without a known group changes nothing.
    mixer.apply(MixSnapshotId::Video);
    advance(mixer, 2, 0.1F);
    CHECK_THAT(mixer.volume(MixCategory::FocusCar), WithinAbs(0.35, 1e-6));
    CHECK(mixer.current(MixGroup::Gameplay) == MixSnapshotId::FreeRoam);
}

TEST_CASE("discMixSnapshots", "[audio]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const MixSnapshots snapshots = MixSnapshots::load(vfs);
    std::size_t present = 0;
    for (std::size_t s = 0; s < kMixSnapshotCount; ++s) {
        const MixSnapshot& snapshot = snapshots.snapshot(static_cast<MixSnapshotId>(s));
        if (!snapshot.present) {
            continue;
        }
        ++present;
        CHECK(snapshot.group.has_value());
        std::size_t active = 0;
        for (const MixSetting& setting : snapshot.settings) {
            active += setting.active ? 1 : 0;
        }
        CHECK(active > 0);
        CHECK(!snapshot.settings[static_cast<std::size_t>(MixCategory::RallyCoDriver)].active);
    }
    CHECK(present == std::size_t{39});
    CHECK(!snapshots.snapshot(MixSnapshotId::LoadingRally).present);

    const MixSnapshot& paused = snapshots.snapshot(MixSnapshotId::Paused);
    CHECK(paused.group == MixGroup::Gameplay);
    for (const MixCategory category : {MixCategory::FocusCar, MixCategory::OppCar, MixCategory::Tire,
             MixCategory::Transmission, MixCategory::Backfire}) {
        const MixSetting& setting = paused.settings[static_cast<std::size_t>(category)];
        CHECK(setting.active);
        CHECK_THAT(setting.volume, WithinAbs(0.0, 1e-6));
        CHECK_THAT(setting.fadeTime, WithinAbs(0.2, 1e-6));
        CHECK_THAT(setting.returnFadeTime, WithinAbs(0.1, 1e-6));
    }
    CHECK_THAT(paused.settings[static_cast<std::size_t>(MixCategory::GameMusic)].volume, WithinAbs(0.25, 1e-6));
    CHECK_THAT(paused.settings[static_cast<std::size_t>(MixCategory::GameUI)].volume, WithinAbs(1.0, 1e-6));
    const MixSnapshot& freeRoam = snapshots.snapshot(MixSnapshotId::FreeRoam);
    CHECK_THAT(freeRoam.settings[static_cast<std::size_t>(MixCategory::FocusCar)].volume, WithinAbs(0.35, 1e-6));
    CHECK_THAT(freeRoam.settings[static_cast<std::size_t>(MixCategory::FocusCar)].fadeTime, WithinAbs(0.5, 1e-6));
    CHECK(snapshots.snapshot(MixSnapshotId::Crash).group == MixGroup::Impact);
}

constexpr std::string_view kCollisionXml = R"(<CollisionData MSBetweenDiffCollisions="75">
<CollisionEvent Surface1="Asphalt" Surface2="CarAluminum">
  <Impact3D Group="Collisions/Collisions/Asphalt/CarAluminum" Event="IMPACT_3D"/>
  <ImpactLFE Group="Collisions/Collisions/LFE" Event="IMPACT_LFE"/>
  <Scrape3D Group="Collisions/Collisions/Asphalt/CarAluminum" Event="SCRAPE_3D"/>
  <Whoosh3D Group="AMB_Default/TrackAmbient/Track3D" Event="Default_Reflections"/>
</CollisionEvent>
<CollisionEvent Surface1="CarAluminum" Surface2="Dirt">
  <Impact3D Group="Collisions/Collisions/CarAluminum/Dirt" Event="IMPACT_3D"/>
</CollisionEvent>
<CollisionEvent Surface1="Dirt" Surface2="CarAluminum">
  <Impact3D Group="Collisions/Collisions/CarCarbonFiber/Dirt" Event="IMPACT_3D"/>
</CollisionEvent>
<CollisionEvent Surface1="Asphault2" Surface2="CarAluminum">
  <Impact3D Group="Collisions/Collisions/Asphalt/CarAluminum" Event="IMPACT_3D"/>
</CollisionEvent>
<BreakEvent Smashable="co_fest_speedcamera_001">
  <Impact3D Group="Collisions/Smashables/Break/Speedcamera" Event="IMPACT_3D"/>
  <Whoosh3D Group="AMB_Default/TrackAmbient/Track3D" Event="Default_Reflections_Smashable"/>
</BreakEvent>
</CollisionData>)";

// collisiondata.xml as the game's loader reads it: pairs by surface id in
// both orders, later entries replacing earlier ones, surfaces not in the
// list skipped, Whoosh3D left out, break events by smashable name.
TEST_CASE("collisionSoundTableParses", "[audio]")
{
    using Slot = CollisionSoundTable::Slot;
    const std::vector<std::string> surfaces{"Asphalt", "Dirt", "CarAluminum"};
    const CollisionSoundTable table = CollisionSoundTable::parse(kCollisionXml, surfaces, "test");
    CHECK(table.msBetweenDiffCollisions() == std::uint32_t{75});
    CHECK(table.surfaceCount() == std::size_t{3});
    const CollisionEventRef* impact = table.event(0, 2, Slot::Impact3D);
    CHECK(impact != nullptr);
    if (impact) {
        CHECK(impact->project == std::string("Collisions"));
        CHECK(impact->event == std::string("Collisions/Asphalt/CarAluminum/IMPACT_3D"));
    }
    const CollisionEventRef* reversed = table.event(2, 0, Slot::Impact3D);
    CHECK((reversed && impact && reversed->event == impact->event));
    const CollisionEventRef* lfe = table.event(2, 0, Slot::ImpactLFE);
    CHECK((lfe && lfe->event == "Collisions/LFE/IMPACT_LFE"));
    const CollisionEventRef* scrape = table.event(0, 2, Slot::Scrape3D);
    CHECK((scrape && scrape->event == "Collisions/Asphalt/CarAluminum/SCRAPE_3D"));
    // The second Dirt entry replaces the first for both orders.
    const CollisionEventRef* dirt = table.event(2, 1, Slot::Impact3D);
    CHECK((dirt && dirt->event == "Collisions/CarCarbonFiber/Dirt/IMPACT_3D"));
    CHECK(table.event(1, 2, Slot::ImpactLFE) == nullptr);
    CHECK(table.event(0, 1, Slot::Impact3D) == nullptr);
    CHECK(table.event(0, 3, Slot::Impact3D) == nullptr);
    CHECK(table.pairCount(Slot::Impact3D) == std::size_t{4});
    CHECK(table.pairCount(Slot::Scrape3D) == std::size_t{2});
    const CollisionEventRef* broken = table.breakEvent("CO_Fest_SpeedCamera_001");
    CHECK((broken && broken->event == "Smashables/Break/Speedcamera/IMPACT_3D"));
    CHECK(table.breakEvent("co_bench_001") == nullptr);
    CHECK(table.breakEventCount() == std::size_t{1});

    const CollisionSoundTable bare = CollisionSoundTable::parse("<CollisionData/>", surfaces, "test");
    CHECK(bare.msBetweenDiffCollisions() == CollisionSoundTable::kDefaultMsBetweenDiffCollisions);
    CHECK_THROWS_AS(CollisionSoundTable::parse("<Other/>", surfaces, "test"), std::exception);
}

// A contact lasts while the same two bodies keep touching through the same
// surfaces, in either order, and ends once it has not been reported for
// more than MSBetweenDiffCollisions.
TEST_CASE("collisionSoundsTrackContacts", "[audio]")
{
    const std::vector<std::string> surfaces{"Asphalt", "Dirt", "CarAluminum"};
    Mixer mixer(48000);
    openhorizon::Vfs vfs;
    AudioLibrary library(vfs);
    CollisionSounds sounds(mixer, library, CollisionSoundTable::parse(kCollisionXml, surfaces, "test"), kMasterBus);
    CollisionReport report;
    report.bodyA = 1;
    report.bodyB = 0;
    report.surfaceA = 2;
    report.surfaceB = 0;
    report.relativeVelocity = glm::vec3(0.0F, 0.0F, 20.0F);
    sounds.report(report, 1000);
    CHECK(sounds.contactCount() == std::size_t{1});
    CollisionReport swapped = report;
    std::swap(swapped.bodyA, swapped.bodyB);
    std::swap(swapped.surfaceA, swapped.surfaceB);
    sounds.report(swapped, 1040);
    CHECK(sounds.contactCount() == std::size_t{1});
    CollisionReport other = report;
    other.surfaceB = 1;
    sounds.report(other, 1040);
    CHECK(sounds.contactCount() == std::size_t{2});
    sounds.update(1115);
    CHECK(sounds.contactCount() == std::size_t{2});
    sounds.update(1116);
    CHECK(sounds.contactCount() == std::size_t{0});
    // Without the project on this empty file system nothing plays.
    CHECK(sounds.started().empty());
}

// The Crash snapshot follows the focus car's speed from one update to the
// next: above the threshold, a fall of more than the drop.
TEST_CASE("crashMixFollowsTheSpeedDrop", "[audio]")
{
    const CrashMix crash = CrashMix::parse(R"(<MixSnapshots><DynamicsData>
      <CrashData threshold="50" drop="10"/></DynamicsData></MixSnapshots>)",
        "test");
    CHECK_THAT(crash.threshold, WithinAbs(50.0, 1e-6));
    CHECK_THAT(crash.drop, WithinAbs(10.0, 1e-6));
    constexpr float kMph = 1.0F / 2.2369F;
    CrashMix mix = crash;
    CHECK(!mix.update(60.0F * kMph));
    CHECK(!mix.update(55.0F * kMph));
    CHECK(mix.update(40.0F * kMph));
    CHECK(!mix.update(20.0F * kMph));
    CHECK(!mix.update(45.0F * kMph));
    // From 45 mph a stop is below the threshold.
    CHECK(!mix.update(0.0F));
    const CrashMix none = CrashMix::parse("<MixSnapshots/>", "test");
    CHECK_THAT(none.threshold, WithinAbs(0.0, 1e-6));
    CHECK_THROWS_AS(CrashMix::parse("<Other/>", "test"), std::exception);
}

std::vector<std::string> discSurfaceNames(const openhorizon::Vfs& vfs)
{
    std::vector<std::string> names;
    const std::optional<std::string> text = vfs.readText("media/physics/surfacetypes.xml");
    pugi::xml_document document;
    if (text && document.load_buffer(text->data(), text->size())) {
        for (const pugi::xml_node node : document.child("Root").child("SurfaceTypesList").children("SurfaceType")) {
            names.emplace_back(node.attribute("value").as_string());
        }
    }
    return names;
}

// The disc's collisiondata.xml against surfaceTypes.xml and Collisions.fev,
// and a car scraping along asphalt played through it.
TEST_CASE("discCollisionSounds", "[audio]")
{
    const char* dir = gameDir();
    using Slot = CollisionSoundTable::Slot;
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const std::vector<std::string> surfaces = discSurfaceNames(vfs);
    CHECK(surfaces.size() == std::size_t{68});
    const auto id = [&](std::string_view name) {
        return static_cast<std::size_t>(std::find(surfaces.begin(), surfaces.end(), name) - surfaces.begin());
    };
    // A car's PhysicsMaterialID indexes this list: 7 and 8 are its two
    // body materials.
    CHECK(id("CarAluminum") == std::size_t{7});
    CHECK(id("CarCarbonFiber") == std::size_t{8});
    const CollisionSoundTable table = CollisionSoundTable::load(vfs, surfaces);
    CHECK(table.msBetweenDiffCollisions() == std::uint32_t{75});
    AudioLibrary library(vfs);
    const auto project = library.project("Game:\\Media\\Audio\\Collisions\\Collisions.fev");
    // Every event a car-against-world or car-against-car pair names is in
    // the project; the ones missing name props this game has no surfaces
    // for (the MetalSign and RubberCone groups).
    std::set<std::string> missing;
    std::size_t carPairs = 0;
    for (const std::size_t car : {id("CarAluminum"), id("CarCarbonFiber")}) {
        for (std::size_t other = 0; other < surfaces.size(); ++other) {
            for (const Slot slot : {Slot::Impact3D, Slot::ImpactLFE, Slot::Scrape3D}) {
                const CollisionEventRef* ref = table.event(car, other, slot);
                if (!ref) {
                    continue;
                }
                ++carPairs;
                CHECK(ref->project == std::string("Collisions"));
                if (!project->findEvent(ref->event)) {
                    missing.insert(ref->event);
                }
            }
        }
    }
    CHECK(carPairs > 150);
    for (const std::string& event : missing) {
        CHECK((event.find("MetalSign") != std::string::npos || event.find("RubberCone") != std::string::npos));
    }
    const CollisionEventRef* asphalt = table.event(id("CarAluminum"), id("Asphalt"), Slot::Impact3D);
    CHECK((asphalt && asphalt->event == "Collisions/Asphalt/CarAluminum/IMPACT_3D"));
    const CollisionEventRef* cars = table.event(id("CarAluminum"), id("CarCarbonFiber"), Slot::Impact3D);
    CHECK((cars && project->findEvent(cars->event) != nullptr));
    const CollisionEventRef* camera = table.breakEvent("co_fest_speedcamera_001");
    CHECK((camera && project->findEvent(camera->event) != nullptr));

    AudioDevice device{AudioDevice::Offline{}};
    CollisionSounds sounds(device.mixer(), library, table, kMasterBus);
    device.mixer().setListener(glm::vec3(0.0F, 0.0F, -3.0F), glm::vec3(1.0F, 0.0F, 0.0F));
    CollisionReport report;
    report.bodyA = 1;
    report.surfaceA = id("CarAluminum");
    report.surfaceB = id("Asphalt");
    report.relativeVelocity = glm::vec3(0.0F, 0.0F, 60.0F / 2.2369F);
    report.carSpeed = 60.0F / 2.2369F;
    std::uint32_t now = 0;
    double energy = 0.0;
    for (int frame = 0; frame < 30; ++frame, now += 16) {
        sounds.report(report, now);
        sounds.update(now);
        for (const float s : device.render(768)) {
            energy += static_cast<double>(s) * s;
        }
    }
    CHECK(sounds.started().size() == std::size_t{3});
    if (sounds.started().size() == 3) {
        CHECK(sounds.started()[0].slot == Slot::Impact3D);
        CHECK_THAT(sounds.started()[0].relativeVelocityMph, WithinAbs(60.0, 1e-3));
        CHECK(sounds.started()[1].slot == Slot::ImpactLFE);
        CHECK(sounds.started()[2].slot == Slot::Scrape3D);
        CHECK_THAT(sounds.started()[2].carSpeedMph, WithinAbs(60.0, 1e-3));
    }
    CHECK(sounds.scrapesPlaying() == std::size_t{1});
    CHECK(energy > 0.0);
    sounds.update(now + 100);
    CHECK(sounds.contactCount() == std::size_t{0});
    CHECK(sounds.scrapesPlaying() == std::size_t{0});
    sounds.playBreak("CO_Fest_SpeedCamera_001", glm::vec3(0.0F), 20.0F);
    CHECK(sounds.started().size() == std::size_t{4});
    CHECK_THAT(sounds.started().back().relativeVelocityMph, WithinAbs(20.0 * 2.2369, 1e-3));

    const CrashMix crash = CrashMix::load(vfs);
    CHECK_THAT(crash.threshold, WithinAbs(50.0, 1e-6));
    CHECK_THAT(crash.drop, WithinAbs(10.0, 1e-6));
}

} // namespace
