// Tests of the audio layer: the FSB and FEV readers on files built here in
// the layouts the headers document, the DSP, mixer and engine-sound logic on
// synthetic signals, and, when FH1_GAME_DIR names an extracted disc, every
// bank, project and engine tuning on it.

#include "TestHarness.h"

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/Decode.h"
#include "audio/Dsp.h"
#include "audio/EngineSound.h"
#include "audio/EngineTuning.h"
#include "audio/EventPlayer.h"
#include "audio/Fev.h"
#include "audio/Fsb.h"
#include "audio/Mixer.h"
#include "audio/Wav.h"
#include "base/Bytes.h"
#include "vfs/Vfs.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

namespace {

using fh::Bytes;
using namespace fh::audio;

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

TEST(fsbParsesHeadersAndLaysOutData)
{
    const std::vector<TestSample> source = testSamples();
    const FsbBank bank = FsbBank::parse(buildFsb(source, 0x40), "test.fsb");
    CHECK_EQ(bank.size(), std::size_t{3});
    CHECK_EQ(bank.version(), 0x00040000U);
    CHECK_EQ(bank.hash(), kTestHash);

    const FsbSample& tone = bank.sample(0);
    CHECK_EQ(tone.name, std::string("Tone.wav"));
    CHECK_EQ(tone.sampleCount, 4U);
    CHECK_EQ(tone.frequency, 22050);
    CHECK(tone.codec == SampleCodec::Pcm16);
    CHECK(tone.loops());
    CHECK_EQ(tone.loopStart, 1U);
    CHECK_EQ(tone.loopEnd, 3U);

    const FsbSample& stereo = bank.sample(1);
    CHECK(stereo.codec == SampleCodec::Pcm8);
    CHECK(stereo.pcmUnsigned());
    CHECK_EQ(stereo.channels, 2);

    const FsbSample& xma = bank.sample(2);
    CHECK(xma.codec == SampleCodec::Xma);
    CHECK_EQ(xma.name.size(), std::size_t{29});
    CHECK_EQ(xma.xmaSeekTable.size(), std::size_t{2});
    if (xma.xmaSeekTable.size() == 2) {
        CHECK_EQ(xma.xmaSeekTable[1], 0x9E00U);
    }

    // Data starts after the headers and each sample on a 32-byte boundary.
    const std::size_t dataStart = 0x30 + 0x50 * 2 + 0x70;
    CHECK_EQ(tone.dataOffset, dataStart);
    CHECK_EQ(stereo.dataOffset, dataStart + 32);
    CHECK_EQ(xma.dataOffset, dataStart + 64);
    CHECK_EQ(bank.sampleData(1).size(), std::size_t{6});
    CHECK_EQ(bank.sampleData(1)[1], 255);
}

TEST(fsbFindsSamplesByWaveformName)
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

TEST(fsbBasicHeadersRepeatTheFirstSample)
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
    CHECK_EQ(bank.size(), std::size_t{3});
    CHECK_EQ(bank.sample(2).sampleCount, 4U);
    CHECK_EQ(bank.sample(2).frequency, 32000);
    CHECK_EQ(bank.sample(2).compressedSize, 8U);
    CHECK_EQ(bank.sampleData(2)[0], 2);
}

TEST(fsbRejectsDamagedBanks)
{
    const Bytes good = buildFsb(testSamples(), 0);
    Bytes badMagic = good;
    badMagic[3] = '3';
    CHECK_THROWS(FsbBank::parse(badMagic, "magic.fsb"));
    CHECK_THROWS(FsbBank::parse(Bytes(good.begin(), good.begin() + 0x40), "short.fsb"));
    Bytes truncatedData(good.begin(), good.end() - 100);
    CHECK_THROWS(FsbBank::parse(truncatedData, "data.fsb"));
    Bytes oversize = good;
    oversize[0x30 + 0x24] = 0xFF;
    CHECK_THROWS(FsbBank::parse(oversize, "oversize.fsb"));
    const FsbBank bank = FsbBank::parse(good, "ok.fsb");
    CHECK_THROWS(bank.sample(3));
}

TEST(pcmDecodesExactly)
{
    const FsbBank bank = FsbBank::parse(buildFsb(testSamples(), 0), "test.fsb");
    const PcmBuffer tone = decodeSample(bank, 0);
    CHECK_EQ(tone.channels, 1);
    CHECK_EQ(tone.sampleRate, 22050);
    CHECK_EQ(tone.frames(), std::size_t{4});
    CHECK_NEAR(tone.samples[1], 0.5, 1e-6);
    CHECK_NEAR(tone.samples[2], -0.5, 1e-6);
    CHECK_NEAR(tone.samples[3], 32767.0 / 32768.0, 1e-6);
    CHECK_EQ(tone.loopStart, std::size_t{1});
    CHECK_EQ(tone.loopEnd, std::size_t{4});

    const PcmBuffer stereo = decodeSample(bank, 1);
    CHECK_EQ(stereo.frames(), std::size_t{3});
    CHECK_NEAR(stereo.samples[0], 0.0, 1e-6);
    CHECK_NEAR(stereo.samples[1], 127.0 / 128.0, 1e-6);
    CHECK_NEAR(stereo.samples[2], -1.0, 1e-6);
    CHECK_NEAR(stereo.samples[4], 0.5, 1e-6);
    CHECK_NEAR(stereo.samples[5], -0.5, 1e-6);

    // Two kilobytes of zeros are no XMA stream.
    CHECK_THROWS(decodeSample(bank, 2));
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

TEST(fevParsesEventsAndSoundDefinitions)
{
    for (const bool old : {false, true}) {
        const Bytes data = buildFev(old);
        const FevProject project = FevProject::parse(data, "test.fev");
        CHECK_EQ(project.name(), std::string("Proj"));
        CHECK_EQ(project.version(), old ? 0x00370000U : 0x00400000U);
        CHECK_EQ(project.banks().size(), std::size_t{1});
        CHECK_EQ(project.banks()[0].name, std::string("Bank"));
        CHECK_EQ(project.banks()[0].hash, old ? 0ULL : kTestHash);
        CHECK_EQ(project.categories().children.size(), std::size_t{1});
        CHECK_NEAR(project.categories().children[0].volume, 0.5, 1e-6);

        CHECK_EQ(project.events().size(), std::size_t{2});
        const FevEvent* engine = project.findEvent("Group/Sub/Engine");
        CHECK(engine != nullptr);
        if (engine) {
            CHECK(!engine->simple);
            CHECK(engine->is3d());
            CHECK_NEAR(engine->volume, 0.5, 1e-6);
            CHECK_EQ(engine->maxPlaybacks, 2U);
            CHECK_NEAR(engine->maxDistance, 50.0, 1e-6);
            CHECK_EQ(engine->guid[15], old ? 0 : 15);
            CHECK_EQ(engine->layers.size(), std::size_t{1});
            CHECK_EQ(engine->layers[0].sounds.size(), std::size_t{2});
            CHECK_EQ(engine->layers[0].sounds[0].soundDefinition, 1);
            CHECK(!engine->layers[0].sounds[0].oneShot);
            CHECK(engine->layers[0].sounds[1].oneShot);
            CHECK_EQ(engine->layers[0].envelopes.size(), std::size_t{1});
            const FevEnvelope& envelope = engine->layers[0].envelopes[0];
            CHECK(envelope.isVolume());
            CHECK_EQ(envelope.parameter, 1U);
            CHECK_NEAR(envelope.valueAt(0.25F), 0.25, 1e-6);
            CHECK_NEAR(envelope.valueAt(2.0F), 1.0, 1e-6);
            CHECK_EQ(engine->parameters.size(), std::size_t{2});
            CHECK_NEAR(engine->parameters[0].maximum, 8000.0, 1e-3);
            CHECK_EQ(engine->parameters[0].sustainPoints.size(), std::size_t{1});
            CHECK(engine->findParameter("load") == &engine->parameters[1]);
            CHECK_EQ(engine->categories.size(), std::size_t{1});
        }
        const FevEvent* click = project.findEvent("proj/group/click");
        CHECK(click != nullptr);
        if (click) {
            CHECK(click->simple);
            CHECK(!click->is3d());
            CHECK_EQ(click->path, std::string("Group/Click"));
        }
        CHECK(project.findEvent("Group/Missing") == nullptr);

        CHECK_EQ(project.soundDefinitions().size(), std::size_t{2});
        const FevSoundDefinition& definition = project.soundDefinition(0);
        CHECK_EQ(definition.name, std::string("/Click"));
        CHECK_EQ(definition.waveforms.size(), std::size_t{2});
        CHECK(definition.waveforms[0].type == FevWaveform::Type::Wavetable);
        CHECK_EQ(definition.waveforms[0].sampleIndex, 1U);
        CHECK_EQ(definition.waveforms[0].lengthMs, 250U);
        CHECK(definition.waveforms[1].type == FevWaveform::Type::DontPlay);
        CHECK_THROWS(project.soundDefinition(2));
    }
}

TEST(fevRejectsMisreadProjects)
{
    const Bytes good = buildFev(false);
    CHECK_THROWS(FevProject::parse(Bytes(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(good.size() / 2)),
        "half.fev"));
    Bytes trailing = good;
    trailing.push_back(0);
    CHECK_THROWS(FevProject::parse(trailing, "trailing.fev"));
    CHECK_THROWS(FevProject::parse(buildFev(false, 5), "badref.fev"));
    Bytes version = good;
    version[6] = 0x41;
    CHECK_THROWS(FevProject::parse(version, "version.fev"));
}

TEST(wavHeaderDescribesTheData)
{
    PcmBuffer pcm;
    pcm.sampleRate = 44100;
    pcm.channels = 2;
    pcm.samples = {0.0F, 1.0F, -1.0F, 2.0F};
    const Bytes pcmWav = encodeWav(pcm, WavFormat::Pcm16);
    CHECK_EQ(pcmWav.size(), std::size_t{44 + 8});
    CHECK_EQ(fh::loadLe32(pcmWav.data() + 24), 44100U);
    CHECK_EQ(fh::loadLe16(pcmWav.data() + 20), 1);
    CHECK_EQ(fh::loadLe16(pcmWav.data() + 22), 2);
    CHECK_EQ(static_cast<std::int16_t>(fh::loadLe16(pcmWav.data() + 46)), 32767);
    CHECK_EQ(static_cast<std::int16_t>(fh::loadLe16(pcmWav.data() + 50)), 32767);

    const Bytes floatWav = encodeWav(pcm, WavFormat::Float32);
    CHECK_EQ(floatWav.size(), std::size_t{44 + 16});
    CHECK_EQ(fh::loadLe16(floatWav.data() + 20), 3);
    CHECK_EQ(fh::loadLe32(floatWav.data() + 40), 16U);
    CHECK_NEAR(std::bit_cast<float>(fh::loadLe32(floatWav.data() + 56)), 2.0, 0.0);
}

TEST(biquadResponses)
{
    const float rate = 48000.0F;
    FilterSpec lowpass;
    lowpass.type = FilterSpec::Type::Lowpass;
    lowpass.frequency = 1000.0F;
    const BiquadCoefficients lp = BiquadCoefficients::design(lowpass, rate);
    CHECK_NEAR(lp.magnitudeAt(50.0F, rate), 1.0, 0.01);
    CHECK_NEAR(lp.magnitudeAt(1000.0F, rate), std::numbers::sqrt2 / 2.0, 0.01);
    CHECK(lp.magnitudeAt(10000.0F, rate) < 0.02F);

    FilterSpec highpass = lowpass;
    highpass.type = FilterSpec::Type::Highpass;
    const BiquadCoefficients hp = BiquadCoefficients::design(highpass, rate);
    CHECK(hp.magnitudeAt(50.0F, rate) < 0.01F);
    CHECK_NEAR(hp.magnitudeAt(15000.0F, rate), 1.0, 0.02);

    FilterSpec peq;
    peq.type = FilterSpec::Type::Peaking;
    peq.frequency = 800.0F;
    peq.bandwidth = 1.0F;
    peq.gain = 2.0F;
    const BiquadCoefficients pk = BiquadCoefficients::design(peq, rate);
    CHECK_NEAR(pk.magnitudeAt(800.0F, rate), 2.0, 0.01);
    CHECK_NEAR(pk.magnitudeAt(20.0F, rate), 1.0, 0.02);
    CHECK_NEAR(pk.magnitudeAt(20000.0F, rate), 1.0, 0.02);

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
    CHECK_NEAR(half.b0, (lp.b0 + pk.b0) / 2.0F, 1e-6);
}

TEST(hermiteInterpolation)
{
    CHECK_NEAR(hermite(0.0F, 1.0F, 2.0F, 3.0F, 0.0F), 1.0, 1e-6);
    CHECK_NEAR(hermite(0.0F, 1.0F, 2.0F, 3.0F, 1.0F), 2.0, 1e-6);
    CHECK_NEAR(hermite(0.0F, 1.0F, 2.0F, 3.0F, 0.25F), 1.25, 1e-6);
    CHECK_NEAR(hermite(5.0F, 5.0F, 5.0F, 5.0F, 0.7F), 5.0, 1e-6);
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

TEST(mixerLevelsAndPan)
{
    Mixer mixer(48000);
    VoiceSettings settings;
    settings.loop = true;
    const VoiceId centre = mixer.play(constant(0.5F, 4800), settings);
    CHECK(centre != kNoVoice);
    std::vector<float> out = render(mixer, 1024);
    CHECK_NEAR(out[1000], 0.5 * std::cos(std::numbers::pi / 4.0), 1e-4);
    CHECK_NEAR(out[1001], 0.5 * std::cos(std::numbers::pi / 4.0), 1e-4);

    mixer.setPan(centre, -1.0F);
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_NEAR(out[100], 0.5, 1e-4);
    CHECK_NEAR(out[101], 0.0, 1e-4);

    mixer.setVolume(centre, 0.5F);
    mixer.setPan(centre, 0.0F);
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_NEAR(out[100], 0.25 * std::cos(std::numbers::pi / 4.0), 1e-4);

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
    CHECK_NEAR(out[200], 0.2, 1e-4);
    CHECK_NEAR(out[201], -0.2, 1e-4);
}

TEST(mixerResamplesByRateAndPitch)
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

TEST(mixerVoicesEndLoopAndStop)
{
    Mixer mixer(48000);
    const VoiceId once = mixer.play(constant(0.5F, 100), VoiceSettings{});
    CHECK(mixer.isPlaying(once));
    std::vector<float> out = render(mixer, 256);
    CHECK(!mixer.isPlaying(once));
    CHECK(std::abs(out[2 * 50]) > 0.1F);
    CHECK_EQ(out[2 * 200], 0.0F);

    VoiceSettings looping;
    looping.loop = true;
    const VoiceId loop = mixer.play(constant(0.5F, 100), looping);
    out = render(mixer, 1000);
    CHECK(mixer.isPlaying(loop));
    CHECK(std::abs(out[2 * 999]) > 0.1F);
    CHECK_EQ(mixer.activeVoiceCount(), std::size_t{1});

    mixer.stop(loop, 0.01F);
    out = render(mixer, 1024);
    CHECK(!mixer.isPlaying(loop));
    CHECK_EQ(out[2 * 1023], 0.0F);
    CHECK_EQ(mixer.activeVoiceCount(), std::size_t{0});

    // A handle stays dead when its slot is reused.
    const VoiceId reused = mixer.play(constant(0.5F, 100), looping);
    CHECK(reused != loop);
    CHECK(!mixer.isPlaying(loop));
    mixer.setVolume(loop, 1.0F);
    CHECK(mixer.isPlaying(reused));
    CHECK_EQ(mixer.play(nullptr, looping), kNoVoice);
}

TEST(rolloffShape)
{
    Rolloff rolloff{10.0F, 30.0F, 70.0F, 100.0F};
    CHECK_NEAR(rolloff.gain(0.0F), 1.0, 1e-6);
    CHECK_NEAR(rolloff.gain(10.0F), 1.0, 1e-6);
    CHECK_NEAR(rolloff.gain(20.0F), 0.5, 1e-6);
    CHECK_NEAR(rolloff.gain(30.0F), 1.0 / 3.0, 1e-6);
    CHECK_NEAR(rolloff.gain(50.0F), 1.0 / 3.0, 1e-6);
    CHECK_NEAR(rolloff.gain(85.0F), 1.0 / 6.0, 1e-6);
    CHECK_NEAR(rolloff.gain(100.0F), 0.0, 1e-6);
    CHECK_NEAR(rolloff.gain(500.0F), 0.0, 1e-6);
    const Rolloff inverse{1.0F, 10000.0F};
    CHECK_NEAR(inverse.gain(4.0F), 0.25, 1e-6);
}

TEST(mixerSpatialVoices)
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
    CHECK_NEAR(out[400], 0.0, 1e-5);
    CHECK_NEAR(out[401], 0.05, 1e-4);

    // Turning the listener around swaps the sides.
    mixer.setListener({0.0F, 0.0F, 0.0F}, {-1.0F, 0.0F, 0.0F});
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_NEAR(out[400], 0.05, 1e-4);
    CHECK_NEAR(out[401], 0.0, 1e-5);

    mixer.setPosition(voice, {0.0F, 0.0F, 2.0F});
    render(mixer, 512);
    out = render(mixer, 512);
    CHECK_NEAR(out[400], 0.25 * std::cos(std::numbers::pi / 4.0), 1e-4);
    CHECK_NEAR(out[401], 0.25 * std::cos(std::numbers::pi / 4.0), 1e-4);
}

TEST(mixerBusesFilterAndScale)
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
    CHECK_NEAR(out[100], 0.25 * std::cos(std::numbers::pi / 4.0), 1e-4);

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
    CHECK_EQ(filtered.activeVoiceCount(), std::size_t{0});
}

TEST(mixerLimiterHoldsTheCeiling)
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

TEST(mixerControlFromAnotherThread)
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

TEST(audioDeviceRendersOfflineAndOpensTheDummyDriver)
{
    AudioDevice offline{AudioDevice::Offline{}};
    CHECK(!offline.isLive());
    VoiceSettings settings;
    settings.loop = true;
    offline.mixer().play(constant(0.5F, 480), settings);
    const std::vector<float> out = offline.render(480);
    CHECK_EQ(out.size(), std::size_t{960});
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
    CHECK_THROWS(live.render(16));
}

TEST(tuningCurvesWeighInputsAndInterpolate)
{
    TuningCurve curve;
    curve.rpm = 0.3F;
    curve.throttle = 0.7F;
    curve.points = {glm::vec2{0.0F, 0.2F}, glm::vec2{0.5F, 0.6F}, glm::vec2{1.0F, 0.8F}};
    EngineInputs in;
    in.rpm = 1.0F;
    in.throttle = 0.0F;
    CHECK_NEAR(curve.input(in), 0.3, 1e-6);
    CHECK_NEAR(curve.evaluate(in), 0.2 + 0.4 * 0.6, 1e-6);
    in.throttle = 1.0F;
    CHECK_NEAR(curve.input(in), 1.0, 1e-6);
    CHECK_NEAR(curve.evaluate(in), 0.8, 1e-6);
    CHECK_NEAR(curve.at(-1.0F), 0.2, 1e-6);
    CHECK_NEAR(curve.at(0.75F), 0.7, 1e-6);

    // All-zero weights and repeated x values select y0, as the disc's
    // constant EQ frequencies need.
    TuningCurve flat;
    flat.points = {glm::vec2{0.0F, 4328.0F}, glm::vec2{0.0F, 20.0F}, glm::vec2{0.0F, 20.0F}};
    CHECK_NEAR(flat.evaluate(EngineInputs{}), 4328.0, 1e-3);
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

TEST(engineTuningFilesParse)
{
    const HarmonicTuning harmonic = HarmonicTuning::parse(kHarmonicXml, "test_ht.xml");
    CHECK_EQ(harmonic.bank, std::string("8N2_Test_Exh.fsb"));
    CHECK_EQ(harmonic.loops.size(), std::size_t{2});
    const std::vector<EngineLoop>& left = harmonic.loops.at("ExhaustL");
    CHECK_EQ(left.size(), std::size_t{2});
    CHECK_EQ(left[0].sampleIndex, 0U);
    CHECK_NEAR(left[1].rpmSample, 2000.0, 1e-3);
    CHECK_NEAR(harmonic.exhaustNoiseVolume, 0.2, 1e-6);
    CHECK(harmonic.burble.has_value());
    if (harmonic.burble) {
        CHECK_EQ(harmonic.burble->burbles, 9U);
        CHECK_EQ(harmonic.burble->backfires, 4U);
        CHECK_EQ(harmonic.burble->silenceWeighting, 66);
        CHECK_NEAR(harmonic.burble->maxRpm, 9067.0, 1e-3);
    }
    EngineInputs in;
    in.throttle = 1.0F;
    in.posTorque = 1.0F;
    const std::vector<FilterSpec> filters = harmonic.dsp.filters(in);
    CHECK_EQ(filters.size(), std::size_t{3});
    if (filters.size() == 3) {
        CHECK(filters[0].type == FilterSpec::Type::Peaking);
        CHECK_NEAR(filters[0].frequency, 150.0, 1e-3);
        CHECK_NEAR(filters[0].gain, 1.5, 1e-6);
        CHECK_NEAR(filters[1].gain, 1.54, 1e-5);
        CHECK(filters[2].type == FilterSpec::Type::Lowpass);
        CHECK_NEAR(filters[2].frequency, 19913.0, 1e-2);
    }
    CHECK_NEAR(harmonic.dsp.volume.evaluate(in), 0.55, 1e-6);

    const EngineTuning engine = EngineTuning::parse(kEngineXml, "test_et.xml");
    CHECK_NEAR(engine.idleRpm, 850.0, 1e-3);
    CHECK_NEAR(engine.redlineRpm, 7500.0, 1e-3);
    CHECK_EQ(engine.cylinders, 8);
    CHECK_NEAR(engine.shiftVolume, 0.4, 1e-6);
    CHECK_EQ(engine.upgradeFile(EmissionGroup::Intake, 0), std::string("8N3_B_Int_HT.xml"));
    CHECK_EQ(engine.upgradeFile(EmissionGroup::Intake, 1), std::string("8N3_B_Int_HT.xml"));
    CHECK_EQ(engine.upgradeFile(EmissionGroup::Intake, 3), std::string("8N3_C_Int_HT.xml"));
    CHECK_EQ(engine.upgradeFile(EmissionGroup::Ambient, 3), std::string("8N1_A_EngAmb_HT.xml"));
    CHECK_EQ(engine.upgradeFile(EmissionGroup::Exhaust, 3), std::string("8N2_E_Exh_HT.xml"));
    EngineTuning raceCar = engine;
    raceCar.upgrades[0] = {"NA.xml", "NA_Int_HT.xml", "NA.xml", "12N3_Race_Int_HT.xml"};
    CHECK_EQ(raceCar.upgradeFile(EmissionGroup::Intake, 0), std::string("12N3_Race_Int_HT.xml"));
    raceCar.upgrades[0] = {"NA.xml", "NA.xml", "NA.xml", "NA.xml"};
    CHECK(raceCar.upgradeFile(EmissionGroup::Intake, 2).empty());
    CHECK(!engine.groupDsp[0].has_value());
    CHECK(engine.groupDsp[2].has_value());
    CHECK_NEAR(engine.shiftBoostDownTime, 0.6, 1e-6);
    CHECK_THROWS(EngineTuning::parse("<Engine><EngineSettings/></Engine>", "broken_et.xml"));
    CHECK_THROWS(EngineTuning::parse("<Engine", "bad_et.xml"));

    const CarModelTuning model = CarModelTuning::parse(kModelXml, "test_cmt.xml");
    CHECK_NEAR(model.emitterPositions.at("ExhaustL").x, -0.415, 1e-6);
    const CarModelTuning::View* follow = model.view("Follow");
    CHECK(follow != nullptr);
    if (follow) {
        CHECK_NEAR(follow->emitterVolumes.at("ExhaustR"), 0.9, 1e-6);
        CHECK_NEAR(follow->otherVolumes.at("TireVolume"), 0.6, 1e-6);
        CHECK(follow->stereoPan("ExhaustL") < -0.3F);
        CHECK_NEAR(follow->stereoPan("EngineIntake"), 0.0, 1e-6);
        CHECK_NEAR(follow->stereoPan("Missing"), 0.0, 1e-6);
    }
    CHECK(model.view("Cockpit") == nullptr);
}

TEST(globalRolloffsAndBackfiresParse)
{
    const auto rolloffs = parseGlobalRolloffs(R"(<GlobalRolloffs>
        <Burble><Gameplay minDist="5" maxDist="7" beginFadeToZeroDist="9" fadeToZeroDist="11"/></Burble>
        <Chassis><ExhaustL><Gameplay minDist="10" maxDist="30" beginFadeToZeroDist="70" fadeToZeroDist="100"/>
        <Replay minDist="1" maxDist="2" beginFadeToZeroDist="3" fadeToZeroDist="4"/></ExhaustL></Chassis>
        </GlobalRolloffs>)",
        "rolloffs.xml");
    CHECK_EQ(rolloffs.size(), std::size_t{2});
    CHECK_NEAR(rolloffs.at("ExhaustL").fadeStart, 70.0, 1e-6);
    CHECK_NEAR(rolloffs.at("Burble").maxDistance, 7.0, 1e-6);

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
    CHECK_EQ(levels.size(), std::size_t{2});
    CHECK_NEAR(levels[1].maxTime, 1800.0, 1e-3);
    CHECK_EQ(levels[1].silenceWeighting, 95);
    EngineInputs in;
    in.rpm = 1.0F;
    CHECK_NEAR(levels[1].volume.evaluate(in), 1.0, 1e-6);
}

TEST(loopCrossfadeWeights)
{
    std::vector<EngineLoop> loops(3);
    loops[0] = EngineLoop{0, 500.0F, 1000.0F, 1500.0F, 1.0F, 1.0F};
    loops[1] = EngineLoop{1, 1200.0F, 1700.0F, 2200.0F, 1.0F, 1.0F};
    loops[2] = EngineLoop{2, 2400.0F, 2800.0F, 3000.0F, 1.0F, 1.0F};
    const auto check = [&](float rpm, std::vector<float> expected) {
        const std::vector<float> weights = EngineSound::loopWeights(loops, rpm);
        CHECK_EQ(weights.size(), expected.size());
        for (std::size_t i = 0; i < std::min(weights.size(), expected.size()); ++i) {
            CHECK_NEAR(weights[i], expected[i], 1e-4);
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
        CHECK_NEAR(sum, 1.0, 1e-4);
    }
    CHECK(EngineSound::loopWeights({}, 1000.0F).empty());
}

TEST(loopIndicesCountChannels)
{
    const std::optional<LoopSource> left = resolveLoopIndex(3, 15, 2);
    const std::optional<LoopSource> right = resolveLoopIndex(18, 15, 2);
    const std::optional<LoopSource> mono = resolveLoopIndex(18, 15, 1);
    CHECK(left && right && mono);
    if (left && right && mono) {
        CHECK_EQ(left->sample, std::size_t{3});
        CHECK_EQ(left->channel, 0);
        CHECK_EQ(right->sample, std::size_t{3});
        CHECK_EQ(right->channel, 1);
        CHECK_EQ(mono->channel, 0);
    }
    CHECK(!resolveLoopIndex(30, 15, 2).has_value());
}

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

std::vector<std::string> filesUnder(const fh::Vfs& vfs, std::string_view suffix)
{
    std::vector<std::string> files;
    for (const std::string& path : vfs.list("media")) {
        if (fh::endsWithNoCase(path, suffix)) {
            files.push_back(path);
        }
    }
    return files;
}

TEST(discDecodesEverySample)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const std::vector<std::string> banks = filesUnder(vfs, ".fsb");
    CHECK_EQ(banks.size(), std::size_t{1006});

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
    CHECK_EQ(samples, std::size_t{22393});
    CHECK_EQ(decoded["xma"], std::size_t{14790});
    CHECK_EQ(decoded["mpeg"], std::size_t{7603});
}

TEST(discProjectsParseAndResolve)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    AudioLibrary library(vfs);
    const std::vector<std::string> projects = filesUnder(vfs, ".fev");
    CHECK_EQ(projects.size(), std::size_t{54});
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
    CHECK_EQ(events, std::size_t{1472});
    CHECK_EQ(wavetables, std::size_t{6315});
    // Two banks were rebuilt smaller than their projects expect: Damage.fsb
    // (2 samples, 2 waveforms point past it) and amb_main_town.fsb (5 night
    // dog waveforms point past its 28 samples). Everything else resolves,
    // and 6251 waveforms find their own file name at their index.
    CHECK_EQ(unresolved, std::size_t{7});
    CHECK_EQ(namedAtIndex, std::size_t{6251});

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

TEST(discEngineTuningsLoad)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
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
        } catch (const fh::DataError& e) {
            std::fprintf(stderr, "not loaded: %s\n", e.what());
        }
    }
    std::fprintf(stderr, "%zu of %zu engine tunings load\n", loaded, total);
    // The electric Volt, Leaf and Roadster have no harmonic tunings at all;
    // the traffic bus, van and truck have only engine-ambient loops.
    CHECK_EQ(total, std::size_t{807});
    CHECK_EQ(loaded, std::size_t{801});
}

TEST(discEngineSoundPlays)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    AudioDevice device{AudioDevice::Offline{}};
    EngineSound engine(device, vfs, "ALF_8C_08");
    CHECK_NEAR(engine.tuning().engine.redlineRpm, 7500.0, 1e-3);
    // Loops take a mixer voice only while their crossfade makes them audible,
    // so a car that is not yet running holds none of the mixer's 256.
    const std::size_t silentVoices = device.mixer().activeVoiceCount();
    CHECK(silentVoices < 8);

    const auto run = [&](float seconds, float rpm, float throttle) {
        double energy = 0.0;
        float peak = 0.0F;
        std::size_t count = 0;
        for (int i = 0; i < static_cast<int>(seconds * 60.0F); ++i) {
            engine.update(rpm, throttle, 20.0F, 1.0F / 60.0F);
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
        engine.update(7300.0F, 0.0F, 30.0F, 1.0F / 60.0F);
        device.render(800);
        most = std::max(most, device.mixer().activeVoiceCount());
    }
    CHECK(most > loops);

    engine.shiftGear(2, 3);
    engine.setTyreSlip(0.8F);
    const auto [effectsRms, effectsPeak] = run(0.5F, 5000.0F, 1.0F);
    CHECK(effectsRms > 0.03);
    CHECK(effectsPeak < 0.99F);
    CHECK(device.mixer().activeVoiceCount() > loops);
    engine.setTyreSlip(0.0F);

    CHECK_THROWS(EngineSound(device, vfs, "NOT_A_CAR_00"));
}

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
