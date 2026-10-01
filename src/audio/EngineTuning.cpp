#include "EngineTuning.h"

#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>

namespace openhorizon::audio {

namespace {

constexpr std::array<const char*, 3> kGroupElements{"EngineIntake", "EngineAmbient", "Exhaust"};

pugi::xml_document loadXml(std::string_view xml, const std::string& source)
{
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw DataError("{}: XML error at byte {}: {}", source, result.offset, result.description());
    }
    return document;
}

pugi::xml_node required(const pugi::xml_node& parent, const char* name, const std::string& source)
{
    const pugi::xml_node child = parent.child(name);
    if (!child) {
        throw DataError("{}: <{}> has no <{}>", source, parent.name(), name);
    }
    return child;
}

/// A missing node gives the flat default curve.
TuningCurve readCurve(const pugi::xml_node& node)
{
    TuningCurve curve;
    if (!node) {
        return curve;
    }
    const pugi::xml_node coefficients = node.child("PhysicsCoeff");
    curve.rpm = coefficients.attribute("RPM").as_float();
    curve.throttle = coefficients.attribute("Throttle").as_float();
    curve.posTorque = coefficients.attribute("PosTorque").as_float();
    curve.negTorque = coefficients.attribute("NegTorque").as_float();
    const pugi::xml_node points = node.child("ThreePointCurve");
    if (points) {
        for (std::size_t i = 0; i < 3; ++i) {
            const std::string x = std::format("x{}", i);
            const std::string y = std::format("y{}", i);
            curve.points[i] = {points.attribute(x.c_str()).as_float(), points.attribute(y.c_str()).as_float()};
        }
    }
    return curve;
}

EmissionDsp::Peq readPeq(const pugi::xml_node& node)
{
    EmissionDsp::Peq peq;
    if (!node) {
        return peq;
    }
    peq.active = node.attribute("Active").as_int() != 0;
    peq.gain = readCurve(node.child("Gain"));
    peq.center = readCurve(node.child("CenterFrequency"));
    peq.bandwidth = readCurve(node.child("Bandwidth"));
    return peq;
}

/// A harmonic tuning's <DSP> or an engine tuning's <EmissionGroupN>; both have the same children.
EmissionDsp readDsp(const pugi::xml_node& node)
{
    EmissionDsp dsp;
    dsp.volume = readCurve(node.child("Volume").child("Gain"));
    dsp.peq = readPeq(node.child("PEQ"));
    dsp.posLoad = readPeq(node.child("LoadPEQ").child("PosLoad"));
    dsp.negLoad = readPeq(node.child("LoadPEQ").child("NegLoad"));
    const pugi::xml_node lowpass = node.child("Lowpass");
    dsp.lowpassActive = lowpass && lowpass.attribute("Active").as_int() != 0;
    dsp.lowpassCutoff = readCurve(lowpass.child("CutoffFrequency"));
    dsp.lowpassResonance = readCurve(lowpass.child("Resonance"));
    return dsp;
}

FilterSpec peqFilter(const EmissionDsp::Peq& peq, const EngineInputs& in)
{
    FilterSpec filter;
    filter.type = FilterSpec::Type::Peaking;
    filter.frequency = peq.center.evaluate(in);
    filter.bandwidth = peq.bandwidth.evaluate(in);
    filter.gain = peq.gain.evaluate(in);
    return filter;
}

} // namespace

float TuningCurve::input(const EngineInputs& in) const
{
    return std::clamp(rpm * in.rpm + throttle * in.throttle + posTorque * in.posTorque + negTorque * in.negTorque,
        0.0F, 1.0F);
}

float TuningCurve::at(float x) const
{
    if (x <= points[0].x) {
        return points[0].y;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (x <= points[i].x) {
            const float span = points[i].x - points[i - 1].x;
            const float t = span > 0.0F ? (x - points[i - 1].x) / span : 1.0F;
            return points[i - 1].y + (points[i].y - points[i - 1].y) * t;
        }
    }
    return points.back().y;
}

float TuningCurve::evaluate(const EngineInputs& in) const
{
    return at(input(in));
}

std::vector<FilterSpec> EmissionDsp::filters(const EngineInputs& in) const
{
    std::vector<FilterSpec> chain;
    for (const Peq* band : {&peq, &posLoad, &negLoad}) {
        if (band->active) {
            chain.push_back(peqFilter(*band, in));
        }
    }
    if (lowpassActive) {
        FilterSpec filter;
        filter.type = FilterSpec::Type::Lowpass;
        filter.frequency = lowpassCutoff.evaluate(in);
        filter.resonance = std::max(1.0F, lowpassResonance.evaluate(in));
        chain.push_back(filter);
    }
    return chain;
}

HarmonicTuning HarmonicTuning::parse(std::string_view xml, const std::string& source)
{
    const pugi::xml_document document = loadXml(xml, source);
    const pugi::xml_node root = document.child("Soundbank");
    if (!root) {
        throw DataError("{}: not a harmonic tuning (no <Soundbank>)", source);
    }
    HarmonicTuning tuning;
    tuning.bank = root.attribute("name").as_string();
    if (tuning.bank.empty()) {
        throw DataError("{}: <Soundbank> names no bank", source);
    }
    for (const char* group : {"EngineIntake", "EngineAmbient", "ExhaustL", "ExhaustR"}) {
        const pugi::xml_node node = root.child(group);
        if (!node) {
            continue;
        }
        std::vector<EngineLoop> loops;
        for (const pugi::xml_node loop : node.children("Loop")) {
            EngineLoop entry;
            entry.sampleIndex = loop.attribute("wavebankindex").as_uint();
            entry.rpmMin = loop.attribute("rpm_min").as_float();
            entry.rpmSample = loop.attribute("rpm_sample").as_float();
            entry.rpmMax = loop.attribute("rpm_max").as_float();
            entry.volume = loop.attribute("volume").as_float(1.0F);
            entry.pitch = loop.attribute("pitch").as_float(1.0F);
            if (entry.rpmSample <= 0.0F || entry.rpmMax < entry.rpmMin) {
                throw DataError("{}: <{}> loop {} has rpm range {}..{} around {}", source, group, entry.sampleIndex,
                    entry.rpmMin, entry.rpmMax, entry.rpmSample);
            }
            loops.push_back(entry);
        }
        std::sort(loops.begin(), loops.end(), [](const EngineLoop& a, const EngineLoop& b) {
            return a.rpmSample < b.rpmSample;
        });
        if (!loops.empty()) {
            tuning.loops.emplace(group, std::move(loops));
        }
    }
    tuning.dsp = readDsp(root.child("DSP"));
    tuning.exhaustNoiseVolume = root.child("ExhaustNoise").attribute("Volume").as_float();
    const pugi::xml_node burble = root.child("Burble");
    if (burble && burble.child("Wavebank")) {
        BurbleSettings settings;
        const pugi::xml_node bank = burble.child("Wavebank");
        settings.bank = bank.attribute("name").as_string();
        settings.burbles = bank.attribute("burbles").as_uint();
        settings.backfires = bank.attribute("backfires").as_uint();
        settings.volume = readCurve(burble.child("Volume"));
        const pugi::xml_node timer = burble.child("Timer");
        settings.minTime = timer.attribute("MinTime").as_float();
        settings.maxTime = timer.attribute("MaxTime").as_float();
        settings.minRpm = timer.attribute("MinRPM").as_float();
        settings.maxRpm = timer.attribute("MaxRPM").as_float();
        settings.silenceWeighting = timer.attribute("SilenceWeighting").as_int(100);
        if (!settings.bank.empty() && settings.burbles + settings.backfires > 0) {
            tuning.burble = std::move(settings);
        }
    }
    return tuning;
}

EngineTuning EngineTuning::parse(std::string_view xml, const std::string& source)
{
    const pugi::xml_document document = loadXml(xml, source);
    const pugi::xml_node root = document.child("Engine");
    if (!root) {
        throw DataError("{}: not an engine tuning (no <Engine>)", source);
    }
    EngineTuning tuning;
    const pugi::xml_node settings = required(root, "EngineSettings", source);
    tuning.idleRpm = settings.attribute("audio_rpm_idle").as_float(800.0F);
    tuning.redlineRpm = settings.attribute("audio_rpm_redline").as_float(7000.0F);
    tuning.cylinders = settings.attribute("num_cylinders").as_int();
    if (!(tuning.redlineRpm > tuning.idleRpm)) {
        throw DataError("{}: redline {} is not above idle {}", source, tuning.redlineRpm, tuning.idleRpm);
    }
    const pugi::xml_node volumes = root.child("VolumeAdjustments");
    tuning.engineVolume = volumes.attribute("EngineVolume").as_float(1.0F);
    tuning.transmissionVolume = volumes.attribute("TransmissionVolume").as_float(1.0F);
    tuning.shiftVolume = volumes.attribute("ShiftVolume").as_float(1.0F);
    tuning.backfireVolume = volumes.attribute("BackfireVolume").as_float(1.0F);
    tuning.burbleVolume = volumes.attribute("BurbleVolume").as_float(1.0F);
    tuning.globalVolume = readCurve(root.child("GlobalCarVolume"));
    for (std::size_t g = 0; g < kGroupElements.size(); ++g) {
        const pugi::xml_node upgrade = required(required(root, kGroupElements[g], source), "Upgrade", source);
        for (std::size_t level = 0; level < 4; ++level) {
            const std::string attribute = std::format("L{}", level);
            tuning.upgrades[g][level] = upgrade.attribute(attribute.c_str()).as_string();
        }
        if (tuning.upgrades[g][0].empty()) {
            throw DataError("{}: <{}> has no level 0 tuning", source, kGroupElements[g]);
        }
    }
    const pugi::xml_node harmonics = root.child("HarmonicTunings");
    for (std::size_t g = 0; g < tuning.groupDsp.size(); ++g) {
        const std::string name = std::format("EmissionGroup{}", g);
        const pugi::xml_node group = harmonics.child(name.c_str());
        if (group) {
            tuning.groupDsp[g] = readDsp(group);
        }
    }
    const pugi::xml_node shift = root.child("ShiftVolumeScalar");
    tuning.shiftBoostUp = shift.attribute("ShiftVolBoostUpPct").as_float();
    tuning.shiftBoostUpTime = shift.attribute("ShiftVolBoostUpTime").as_float();
    tuning.shiftBoostDown = shift.attribute("ShiftVolBoostDownPct").as_float();
    tuning.shiftBoostDownTime = shift.attribute("ShiftVolBoostDownTime").as_float();
    return tuning;
}

std::string EngineTuning::upgradeFile(EmissionGroup group, int level) const
{
    const auto& files = upgrades[static_cast<std::size_t>(group)];
    const auto available = [&](int l) {
        const std::string& file = files[static_cast<std::size_t>(l)];
        return !file.empty() && !startsWithNoCase(file, "NA.") && !startsWithNoCase(file, "NA_");
    };
    const int wanted = std::clamp(level, 0, 3);
    for (int l = wanted; l >= 0; --l) {
        if (available(l)) {
            return files[static_cast<std::size_t>(l)];
        }
    }
    for (int l = wanted + 1; l <= 3; ++l) {
        if (available(l)) {
            return files[static_cast<std::size_t>(l)];
        }
    }
    return {};
}

CarModelTuning CarModelTuning::parse(std::string_view xml, const std::string& source)
{
    const pugi::xml_document document = loadXml(xml, source);
    const pugi::xml_node root = document.child("Car");
    if (!root) {
        throw DataError("{}: not a car model tuning (no <Car>)", source);
    }
    CarModelTuning tuning;
    for (const pugi::xml_node emitter : root.child("Chassis").children()) {
        const pugi::xml_node origin = emitter.child("OriginPosition");
        if (origin) {
            tuning.emitterPositions[emitter.name()] = {origin.attribute("x").as_float(),
                origin.attribute("y").as_float(), origin.attribute("z").as_float()};
        }
    }
    for (const pugi::xml_node listener : root.child("Listener").children()) {
        for (const pugi::xml_node viewNode : listener.children()) {
            View view;
            for (const pugi::xml_attribute a : viewNode.child("EmissionGroupVolumes").attributes()) {
                view.emitterVolumes[a.name()] = a.as_float(1.0F);
            }
            for (const pugi::xml_attribute a : viewNode.child("OtherVolumes").attributes()) {
                view.otherVolumes[a.name()] = a.as_float(1.0F);
            }
            for (const pugi::xml_node child : viewNode.children()) {
                const std::string_view name = child.name();
                if (name.starts_with("SurroundPan")) {
                    view.surroundPan[std::string(name.substr(11))] = {child.attribute("Left").as_float(1.0F),
                        child.attribute("Right").as_float(1.0F), child.attribute("Center").as_float(0.0F)};
                }
            }
            tuning.views[viewNode.name()] = std::move(view);
        }
    }
    return tuning;
}

float CarModelTuning::View::stereoPan(std::string_view emitter) const
{
    const auto it = surroundPan.find(emitter);
    if (it == surroundPan.end()) {
        return 0.0F;
    }
    const float centre = it->second.z * 0.70710678F;
    const float left = it->second.x + centre;
    const float right = it->second.y + centre;
    return left + right > 0.0F ? (right - left) / (left + right) : 0.0F;
}

const CarModelTuning::View* CarModelTuning::view(std::string_view name) const
{
    const auto it = views.find(name);
    return it == views.end() ? nullptr : &it->second;
}

std::map<std::string, Rolloff, std::less<>> parseGlobalRolloffs(std::string_view xml, const std::string& source)
{
    const pugi::xml_document document = loadXml(xml, source);
    const pugi::xml_node root = document.child("GlobalRolloffs");
    if (!root) {
        throw DataError("{}: no <GlobalRolloffs>", source);
    }
    std::map<std::string, Rolloff, std::less<>> rolloffs;
    const auto read = [&](const pugi::xml_node& node) {
        const pugi::xml_node gameplay = node.child("Gameplay");
        if (!gameplay) {
            return;
        }
        Rolloff rolloff;
        rolloff.minDistance = gameplay.attribute("minDist").as_float(1.0F);
        rolloff.maxDistance = gameplay.attribute("maxDist").as_float(rolloff.minDistance);
        rolloff.fadeStart = gameplay.attribute("beginFadeToZeroDist").as_float(rolloff.fadeStart);
        rolloff.fadeEnd = gameplay.attribute("fadeToZeroDist").as_float(rolloff.fadeEnd);
        rolloffs[node.name()] = rolloff;
    };
    for (const pugi::xml_node node : root.children()) {
        if (std::string_view(node.name()) == "Chassis") {
            for (const pugi::xml_node emitter : node.children()) {
                read(emitter);
            }
        } else {
            read(node);
        }
    }
    return rolloffs;
}

std::vector<BackfireLevel> parseBackfireConfig(std::string_view timerXml, std::string_view volumeXml,
    const std::string& source)
{
    const pugi::xml_document timers = loadXml(timerXml, source);
    const pugi::xml_document volumes = loadXml(volumeXml, source);
    std::vector<BackfireLevel> levels;
    for (const pugi::xml_node backfire : timers.child("BackfireTimerConfig").children("Backfire")) {
        const pugi::xml_node timer = backfire.child("Timer");
        BackfireLevel level;
        level.minTime = timer.attribute("MinTime").as_float();
        level.maxTime = timer.attribute("MaxTime").as_float();
        level.minRpm = timer.attribute("MinNormRPM").as_float();
        level.maxRpm = timer.attribute("MaxNormRPM").as_float();
        level.silenceWeighting = timer.attribute("SilenceWeighting").as_int(100);
        levels.push_back(level);
    }
    std::size_t index = 0;
    for (const pugi::xml_node backfire : volumes.child("BackfireVolumeConfig").children("Backfire")) {
        if (index < levels.size()) {
            levels[index].volume = readCurve(backfire.child("Volume"));
        }
        ++index;
    }
    return levels;
}

std::optional<LoopSource> resolveLoopIndex(std::uint32_t wavebankIndex, std::size_t bankSamples, int sampleChannels)
{
    if (wavebankIndex < bankSamples) {
        return LoopSource{wavebankIndex, 0};
    }
    if (wavebankIndex < 2 * bankSamples) {
        return LoopSource{wavebankIndex - bankSamples, sampleChannels > 1 ? 1 : 0};
    }
    return std::nullopt;
}

std::string engineBankPath(const Vfs& vfs, std::string_view bankName)
{
    const std::string name = toLower(bankName);
    for (const char* folder : {"lod1", "burbles"}) {
        std::string path = std::format("media/audio/cars/engines/soundbanks/{}/{}", folder, name);
        if (vfs.exists(path)) {
            return path;
        }
    }
    return {};
}

CarAudioTuning CarAudioTuning::load(const Vfs& vfs, std::string_view carMedia, int intakeLevel, int exhaustLevel)
{
    CarAudioTuning car;
    car.media = std::string(carMedia);
    const std::string lower = toLower(carMedia);
    const std::string enginePath = std::format("media/audio/cars/engines/enginetuning/{}_et.xml", lower);
    const std::optional<std::string> engineXml = vfs.readText(enginePath);
    if (!engineXml) {
        throw DataError("{}: the car '{}' has no engine tuning", enginePath, carMedia);
    }
    car.engine = EngineTuning::parse(*engineXml, enginePath);

    const std::string modelPath = std::format("media/audio/cars/carmodeltuning/{}_cmt.xml", lower);
    if (const std::optional<std::string> modelXml = vfs.readText(modelPath)) {
        car.model = CarModelTuning::parse(*modelXml, modelPath);
    } else {
        logWarning("audio: {} is missing; the engine plays without per-view mixing", modelPath);
    }

    const std::array<int, 3> levels{intakeLevel, 0, exhaustLevel};
    for (std::size_t g = 0; g < 3; ++g) {
        const std::string file = car.engine.upgradeFile(static_cast<EmissionGroup>(g), levels[g]);
        if (file.empty()) {
            continue;
        }
        const std::string path = std::format("media/audio/cars/engines/harmonictuning/{}", toLower(file));
        const std::optional<std::string> xml = vfs.readText(path);
        if (!xml) {
            logWarning("audio: {} names {}, which is missing", enginePath, path);
            continue;
        }
        car.harmonics[g] = HarmonicTuning::parse(*xml, path);
        car.harmonicFiles[g] = path;
    }
    if (!car.harmonics[0] && !car.harmonics[1] && !car.harmonics[2]) {
        throw DataError("{}: none of the car's harmonic tunings exist", enginePath);
    }

    if (const std::optional<std::string> rolloffs = vfs.readText("media/audio/globalrolloffs.xml")) {
        car.rolloffs = parseGlobalRolloffs(*rolloffs, "media/audio/globalrolloffs.xml");
    }
    const std::optional<std::string> timers = vfs.readText("media/audio/backfiretimerconfig.xml");
    const std::optional<std::string> volumes = vfs.readText("media/audio/backfirevolumeconfig.xml");
    if (timers && volumes) {
        car.backfires = parseBackfireConfig(*timers, *volumes, "media/audio/backfire*config.xml");
    }
    return car;
}

} // namespace openhorizon::audio
