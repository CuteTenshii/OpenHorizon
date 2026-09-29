#include "TyreSound.h"

#include "AudioLibrary.h"
#include "EventPlayer.h"
#include "Fev.h"

#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>

namespace fh::audio {

namespace {

/// The executable's per-wheel clamps (0x82514F00 to 0x82515600) and unit
/// conversions: rad/s to rpm by 60 / 2 pi (the constant at 0x8200C910),
/// standard gravity 9.80665 (0x822499DC) and the 4 wheels of 0x82000D70.
constexpr float kRadPerSecToRpm = 60.0F / (2.0F * std::numbers::pi_v<float>);
constexpr float kMaxTireRpm = 20000.0F;
constexpr float kMaxNormLoad = 3.0F;
constexpr float kMaxSlip = 5.0F;
constexpr float kGravity = 9.80665F;
constexpr float kWheelsPerCar = 4.0F;

/// Event name suffixes in CarPhysics wheel order (front left, front right,
/// rear left, rear right); the game names its per-wheel events by corner.
constexpr std::array<std::string_view, TyreSound::kWheels> kCorners{"LF", "RF", "LR", "RR"};

/// Moves `current` towards `target` by at most `step`, as the game's helper
/// at 0x82D797D8 does.
float approach(float current, float target, float step)
{
    const float difference = target - current;
    if (difference > step) {
        return current + step;
    }
    if (difference < -step) {
        return current - step;
    }
    return target;
}

float floatProperty(const FevGroup& group, std::string_view name)
{
    const FevUserProperty* property = group.findUserProperty(name);
    if (!property) {
        throw DataError("{}: event group {} has no user property {}", kTiresProjectPath, group.path, name);
    }
    if (const float* value = std::get_if<float>(&property->value)) {
        return *value;
    }
    if (const std::int32_t* value = std::get_if<std::int32_t>(&property->value)) {
        return static_cast<float>(*value);
    }
    throw DataError("{}: user property {} of {} is not a number", kTiresProjectPath, name, group.path);
}

} // namespace

TyreSurfaceMap TyreSurfaceMap::parse(std::string_view xml, const std::string& source)
{
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw DataError("{}: not XML ({})", source, result.description());
    }
    const pugi::xml_node root = document.child("Tires");
    if (!root) {
        throw DataError("{}: no <Tires> element", source);
    }
    TyreSurfaceMap map;
    for (const pugi::xml_node node : root.children("SurfaceMap")) {
        TyreSurface surface;
        surface.physicsSurface = node.attribute("PhysicsSurface").as_string();
        surface.lod1EventGroup = node.attribute("Lod1EventGroup").as_string();
        surface.lod2EventGroup = node.attribute("Lod2EventGroup").as_string();
        surface.surfaceIndex = node.attribute("SurfaceIndex").as_int();
        if (!surface.physicsSurface.empty()) {
            map.surfaces.push_back(std::move(surface));
        }
    }
    return map;
}

TyreSurfaceMap TyreSurfaceMap::load(const Vfs& vfs)
{
    const Bytes data = vfs.readOrThrow(kTiresXmlPath);
    return parse(std::string_view(reinterpret_cast<const char*>(data.data()), data.size()));
}

std::optional<std::size_t> TyreSurfaceMap::find(std::string_view physicsSurface) const
{
    for (std::size_t i = 0; i < surfaces.size(); ++i) {
        if (iequals(surfaces[i].physicsSurface, physicsSurface)) {
            return i;
        }
    }
    return std::nullopt;
}

TyreSmoothing TyreSmoothing::fromProject(const FevProject& project)
{
    const FevGroup* group = project.findGroup("Tires/Tires");
    if (!group) {
        throw DataError("{}: no Tires event group", kTiresProjectPath);
    }
    TyreSmoothing smoothing;
    smoothing.combinedSlipRate = floatProperty(*group, "CombinedSlipUnitsPerSecDamper");
    smoothing.slipAngleRate = floatProperty(*group, "SlipAngleUnitsPerSecDamper");
    smoothing.slipRatioRate = floatProperty(*group, "SlipRatioUnitsPerSecDamper");
    smoothing.normLoadRate = floatProperty(*group, "NormLoadUnitsPerSecDamper");
    smoothing.tireRpmRate = floatProperty(*group, "TireRPMUnitsPerSecDamper");
    smoothing.timeUntilLeavesGround = floatProperty(*group, "TimeUntilTireLeavesGround");
    return smoothing;
}

TyreSound::TyreSound(Mixer& mixer, std::shared_ptr<AudioLibrary> library, TyreSoundOptions options)
    : m_mixer(mixer)
    , m_library(std::move(library))
    , m_options(options)
    , m_nextSeed(options.seed)
{
    m_project = m_library->project(kTiresProjectPath);
    m_surfaces = TyreSurfaceMap::load(m_library->vfs());
    m_smoothing = TyreSmoothing::fromProject(*m_project);
    for (Wheel& wheel : m_wheels) {
        wheel.events.resize(m_surfaces.surfaces.size());
    }
    m_lod2Events.resize(m_surfaces.surfaces.size());

    // Decode everything the mapped groups can play now rather than on the
    // game thread when a wheel first reaches a new surface.
    for (const TyreSurface& surface : m_surfaces.surfaces) {
        const std::string& group = m_options.perWheel ? surface.lod1EventGroup : surface.lod2EventGroup;
        for (const FevEvent& event : m_project->events()) {
            const std::string path = std::string(m_project->name()) + "/" + event.path;
            if (!startsWithNoCase(path, group) || path.size() <= group.size() || path[group.size()] != '/') {
                continue;
            }
            for (const FevLayer& layer : event.layers) {
                for (const FevSoundInstance& sound : layer.sounds) {
                    for (const FevWaveform& waveform : m_project->soundDefinition(sound.soundDefinition).waveforms) {
                        if (waveform.type == FevWaveform::Type::Wavetable) {
                            m_library->waveform(kTiresProjectPath, waveform);
                        }
                    }
                }
            }
        }
    }
}

TyreSound::~TyreSound() = default;

std::unique_ptr<EventInstance> TyreSound::makeEvent(const std::string& path)
{
    const FevEvent* event = m_project->findEvent(path);
    if (!event) {
        logWarning("audio: {} has no event {}", kTiresProjectPath, path);
        return nullptr;
    }
    EventSettings settings;
    settings.volume = m_options.volume;
    settings.spatial = m_options.spatial;
    settings.bus = m_options.bus;
    settings.seed = m_nextSeed++;
    return std::make_unique<EventInstance>(
        m_mixer, *m_library, std::string(kTiresProjectPath), m_project, *event, settings);
}

TyreSound::EventPair& TyreSound::eventsFor(Wheel& wheel, int surface, std::size_t wheelIndex)
{
    EventPair& pair = wheel.events[static_cast<std::size_t>(surface)];
    if (!pair.created) {
        pair.created = true;
        const std::string& group = m_surfaces.surfaces[static_cast<std::size_t>(surface)].lod1EventGroup;
        pair.roll = makeEvent(std::format("{}/Roll_{}", group, kCorners[wheelIndex]));
        pair.skid = makeEvent(std::format("{}/Skid_{}", group, kCorners[wheelIndex]));
    }
    return pair;
}

TyreSound::EventPair& TyreSound::lod2EventsFor(int surface)
{
    EventPair& pair = m_lod2Events[static_cast<std::size_t>(surface)];
    if (!pair.created) {
        pair.created = true;
        const std::string& group = m_surfaces.surfaces[static_cast<std::size_t>(surface)].lod2EventGroup;
        pair.roll = makeEvent(group + "/Roll");
        pair.skid = makeEvent(group + "/Skid");
    }
    return pair;
}

void TyreSound::stop(EventPair& pair)
{
    for (const std::unique_ptr<EventInstance>* event : {&pair.roll, &pair.skid}) {
        if (*event && (*event)->isStarted()) {
            (*event)->stop();
        }
    }
}

void TyreSound::smooth(Wheel& wheel, const TyreInput& input, const TyreCarInput& car, float dt) const
{
    Parameters raw;
    raw.tireRpm = std::min(std::abs(input.spin) * kRadPerSecToRpm, kMaxTireRpm);
    const float weight = std::max(car.mass, 1e-3F) * kGravity;
    raw.normLoad = std::clamp(input.load * kWheelsPerCar / weight, 0.0F, kMaxNormLoad);
    raw.slipAngle = std::min(std::abs(input.slipAngle), kMaxSlip);
    raw.slipRatio = std::clamp(input.slipRatio, -kMaxSlip, kMaxSlip) * (car.reversing ? -1.0F : 1.0F);
    raw.combinedSlip = std::clamp(input.combinedSlip, 0.0F, kMaxSlip);
    raw.grounded = input.onGround;
    if (input.onGround) {
        const std::optional<std::size_t> entry = m_surfaces.find(input.surface);
        raw.surface = entry ? static_cast<int>(*entry) : -1;
    }

    Parameters& out = wheel.smoothed;
    if (!input.onGround) {
        wheel.airborneTime += dt;
        if (wheel.airborneTime > m_smoothing.timeUntilLeavesGround) {
            out = raw;
        }
        return;
    }
    out.tireRpm = approach(out.tireRpm, raw.tireRpm, m_smoothing.tireRpmRate * dt);
    out.normLoad = approach(out.normLoad, raw.normLoad, m_smoothing.normLoadRate * dt);
    out.slipAngle = approach(out.slipAngle, raw.slipAngle, m_smoothing.slipAngleRate * dt);
    out.slipRatio = approach(out.slipRatio, raw.slipRatio, m_smoothing.slipRatioRate * dt);
    out.combinedSlip = approach(out.combinedSlip, raw.combinedSlip, m_smoothing.combinedSlipRate * dt);
    out.surface = raw.surface;
    out.grounded = true;
    wheel.airborneTime = 0.0F;
}

void TyreSound::feed(EventInstance& event, const Parameters& parameters, bool combined) const
{
    // The game starts an event before setting its parameters, but FMOD
    // only acts on the start at its next update, by which time the new
    // values are in; setting them first does the same here.
    event.setParameter("TireRPM", parameters.tireRpm);
    if (combined) {
        event.setParameter("CombinedSlip", parameters.combinedSlip);
    } else {
        event.setParameter("SlipAngle", parameters.slipAngle);
        event.setParameter("SlipRatio", parameters.slipRatio);
    }
    event.setParameter("Load", parameters.normLoad);
    const int index = m_surfaces.surfaces[static_cast<std::size_t>(parameters.surface)].surfaceIndex;
    event.setParameter("SurfaceIndex", static_cast<float>(index));
    if (!event.isStarted()) {
        event.start();
    }
}

void TyreSound::updatePerWheel(std::span<const TyreInput, kWheels> wheels)
{
    for (std::size_t i = 0; i < kWheels; ++i) {
        Wheel& wheel = m_wheels[i];
        const int surface = wheel.smoothed.surface;
        const int previous = wheel.previousSurface;
        if (surface < 0) {
            if (previous >= 0) {
                stop(wheel.events[static_cast<std::size_t>(previous)]);
            }
            if (!wheel.smoothed.grounded) {
                continue;
            }
            // On ground Tires.xml does not list: the game finds no events
            // for it and moves on, so returning later counts as a change.
            wheel.previousSurface = -1;
            continue;
        }
        if (surface != previous && previous >= 0) {
            stop(wheel.events[static_cast<std::size_t>(previous)]);
        }
        EventPair& pair = eventsFor(wheel, surface, i);
        for (const std::unique_ptr<EventInstance>* event : {&pair.roll, &pair.skid}) {
            if (*event) {
                if (m_options.spatial) {
                    (*event)->setPosition(wheels[i].position);
                }
                feed(**event, wheel.smoothed, false);
            }
        }
        wheel.previousSurface = surface;
    }
}

void TyreSound::updateSingle(const TyreCarInput& car)
{
    // The game picks the grounded wheel with the most combined slip, starting
    // from zero, so wheels that do not slip at all never qualify.
    std::optional<std::size_t> loudest;
    float most = 0.0F;
    for (std::size_t i = 0; i < kWheels; ++i) {
        const Parameters& p = m_wheels[i].smoothed;
        if (p.grounded && p.combinedSlip > most) {
            most = p.combinedSlip;
            loudest = i;
        }
    }
    const int previous = m_lod2PreviousSurface;
    const int surface = loudest ? m_wheels[*loudest].smoothed.surface : -1;
    if (surface < 0) {
        if (previous >= 0) {
            stop(m_lod2Events[static_cast<std::size_t>(previous)]);
        }
        if (loudest) {
            m_lod2PreviousSurface = -1;
        }
        return;
    }
    if (surface != previous && previous >= 0) {
        stop(m_lod2Events[static_cast<std::size_t>(previous)]);
    }
    EventPair& pair = lod2EventsFor(surface);
    for (const std::unique_ptr<EventInstance>* event : {&pair.roll, &pair.skid}) {
        if (*event) {
            if (m_options.spatial) {
                (*event)->setPosition(car.position);
            }
            feed(**event, m_wheels[*loudest].smoothed, true);
        }
    }
    m_lod2PreviousSurface = surface;
}

void TyreSound::update(std::span<const TyreInput, kWheels> wheels, const TyreCarInput& car, float dt)
{
    dt = std::max(dt, 0.0F);
    for (std::size_t i = 0; i < kWheels; ++i) {
        smooth(m_wheels[i], wheels[i], car, dt);
    }
    if (m_options.perWheel) {
        updatePerWheel(wheels);
    } else {
        updateSingle(car);
    }
}

void TyreSound::setVolume(float volume)
{
    m_options.volume = std::max(volume, 0.0F);
    const auto apply = [this](EventPair& pair) {
        for (const std::unique_ptr<EventInstance>* event : {&pair.roll, &pair.skid}) {
            if (*event) {
                (*event)->setVolume(m_options.volume);
            }
        }
    };
    for (Wheel& wheel : m_wheels) {
        std::for_each(wheel.events.begin(), wheel.events.end(), apply);
    }
    std::for_each(m_lod2Events.begin(), m_lod2Events.end(), apply);
}

bool TyreSound::isPlaying() const
{
    const auto playing = [](const EventPair& pair) {
        return (pair.roll && pair.roll->isPlaying()) || (pair.skid && pair.skid->isPlaying());
    };
    for (const Wheel& wheel : m_wheels) {
        if (std::any_of(wheel.events.begin(), wheel.events.end(), playing)) {
            return true;
        }
    }
    return std::any_of(m_lod2Events.begin(), m_lod2Events.end(), playing);
}

} // namespace fh::audio
