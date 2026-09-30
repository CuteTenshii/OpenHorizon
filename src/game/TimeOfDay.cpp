#include "TimeOfDay.h"

#include "base/Error.h"
#include "base/Log.h"
#include "render/WorldRenderer.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace fh {

namespace {

constexpr float kMinutesPerDay = 1440.0F;
constexpr float kSecondsPerDay = 86400.0F;
/// The executable puts `FogDensity * 0.01` into the shaders' base-2
/// extinction per metre (0x824297B4, the factor at 0x82000CCC).
constexpr float kFogDensityPerMetre = 0.01F;
/// The template the game falls back to outside every post-processing zone
/// is not decoded; "NoFog" is used, and "LightFog", the other template
/// most zones use, holds the same values.
constexpr const char* kDefaultFogTemplate = "NoFog";

pugi::xml_document parseTrackFile(const Vfs& vfs, const std::string& path)
{
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} not found", path);
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text->data(), text->size());
    if (!parsed) {
        throw DataError("{}: {}", path, parsed.description());
    }
    return doc;
}

} // namespace

TimeOfDayCurves TimeOfDayCurves::load(const Vfs& vfs, std::string_view track)
{
    const std::string path = std::format("media/tracks/{}/Ribbon_00/TimeOfDayA.xml", track);
    const pugi::xml_document doc = parseTrackFile(vfs, path);
    const pugi::xml_node root = doc.child("TOD");
    if (!root) {
        throw DataError("{} is not a TOD document", path);
    }
    TimeOfDayCurves curves;
    for (const pugi::xml_node curve : root.children()) {
        std::vector<Key> keys;
        for (const pugi::xml_node key : curve.children("Key")) {
            keys.push_back({key.attribute("time").as_float(),
                {key.attribute("x").as_float(), key.attribute("y").as_float(), key.attribute("z").as_float()}});
        }
        if (keys.empty()) {
            continue;
        }
        std::sort(keys.begin(), keys.end(), [](const Key& a, const Key& b) { return a.time < b.time; });
        curves.m_curves[curve.name()] = std::move(keys);
    }
    logInfo("time of day: {} curves from {}", curves.m_curves.size(), path);

    const std::string fogPath = std::format("media/dynamicpost/tracks/{}/fog_templates.xml", track);
    if (vfs.exists(fogPath)) {
        const pugi::xml_document fogDoc = parseTrackFile(vfs, fogPath);
        const pugi::xml_node node
            = fogDoc.child("Templates").find_child_by_attribute("Template", "Name", kDefaultFogTemplate);
        if (node) {
            FogTemplate fog;
            fog.colour = {node.attribute("ColorR").as_float(1.0F), node.attribute("ColorG").as_float(1.0F),
                node.attribute("ColorB").as_float(1.0F)};
            fog.density = node.attribute("Density").as_float(1.0F);
            fog.startDistance = node.attribute("StartDistance").as_float(1.0F);
            fog.scatterColour = {node.attribute("SunScatterColorR").as_float(1.0F),
                node.attribute("SunScatterColorG").as_float(1.0F), node.attribute("SunScatterColorB").as_float(1.0F)};
            fog.scatterPower = node.attribute("SunScatterPower").as_float(1.0F);
            fog.heightFadeAmount = node.attribute("HeightFadeAmount").as_float(1.0F);
            fog.heightFadeStart = node.attribute("HeightFadeStart").as_float(1.0F);
            fog.heightFadeEnd = node.attribute("HeightFadeEnd").as_float(1.0F);
            fog.scatterStartDistance = node.attribute("SunScatterStartDistance").as_float();
            fog.scatterRampDistance = node.attribute("SunScatterRampupDistance").as_float();
            curves.m_fogTemplate = fog;
        } else {
            logWarning("time of day: {} has no {} template", fogPath, kDefaultFogTemplate);
        }
    } else {
        logWarning("time of day: no {}, the fog takes no colour from the sun", fogPath);
    }
    return curves;
}

bool TimeOfDayCurves::has(std::string_view curve) const
{
    return m_curves.contains(std::string(curve));
}

glm::vec3 TimeOfDayCurves::sample(std::string_view curve, float minutes) const
{
    const auto it = m_curves.find(std::string(curve));
    if (it == m_curves.end()) {
        return glm::vec3(0.0F);
    }
    const std::vector<Key>& keys = it->second;
    float t = std::fmod(minutes, kMinutesPerDay);
    if (t < 0.0F) {
        t += kMinutesPerDay;
    }
    const auto next
        = std::upper_bound(keys.begin(), keys.end(), t, [](float time, const Key& key) { return time < key.time; });
    // Between the last key and the first, wrap across midnight.
    const Key& after = next == keys.end() ? keys.front() : *next;
    const Key& before = next == keys.begin() ? keys.back() : *(next - 1);
    float span = after.time - before.time;
    float into = t - before.time;
    if (span <= 0.0F) {
        span += kMinutesPerDay;
    }
    if (into < 0.0F) {
        into += kMinutesPerDay;
    }
    const float f = span > 0.0F ? std::clamp(into / span, 0.0F, 1.0F) : 0.0F;
    return glm::mix(before.value, after.value, f);
}

PostEffectSettings PostEffectSettings::load(const Vfs& vfs, std::string_view track)
{
    const std::string path = std::format("media/tracks/{}/Ribbon_00/TimeOfDay.xml", track);
    const pugi::xml_document doc = parseTrackFile(vfs, path);
    const pugi::xml_node effects = doc.child("posteffects");
    PostEffectSettings s;
    if (const pugi::xml_node tone = effects.child("filmicTone")) {
        s.white = tone.attribute("white").as_float(s.white);
        s.shoulderStrength = tone.attribute("shoulderStrength").as_float(s.shoulderStrength);
        s.linearStrength = tone.attribute("linearStrength").as_float(s.linearStrength);
        s.linearAngle = tone.attribute("linearAngle").as_float(s.linearAngle);
        s.toeStrength = tone.attribute("toeStrength").as_float(s.toeStrength);
        s.toeNumerator = tone.attribute("toeNumerator").as_float(s.toeNumerator);
        s.toeDenominator = tone.attribute("toeDenominator").as_float(s.toeDenominator);
        s.exposure = tone.attribute("exposure").as_float(s.exposure);
    }
    if (const pugi::xml_node vignette = effects.child("vignette")) {
        s.vignettePower = vignette.attribute("vignettePower").as_float(s.vignettePower);
    }
    if (const pugi::xml_node desat = effects.child("desat")) {
        s.desaturate = desat.attribute("desaturateAmount").as_float(s.desaturate);
    }
    return s;
}

TimeOfDay::TimeOfDay(TimeOfDayCurves curves)
    : m_curves(std::move(curves))
{
}

void TimeOfDay::advance(float realSeconds)
{
    setSeconds(m_seconds + realSeconds * m_curves.sampleScalar("TimeSpeed", minutes()));
}

void TimeOfDay::setSeconds(float seconds)
{
    m_seconds = std::fmod(seconds, kSecondsPerDay);
    if (m_seconds < 0.0F) {
        m_seconds += kSecondsPerDay;
    }
}

glm::vec3 TimeOfDay::aim(std::string_view position, std::string_view target) const
{
    const glm::vec3 direction = m_curves.sample(target, minutes()) - m_curves.sample(position, minutes());
    const float length = glm::length(direction);
    if (length < 1e-4F) {
        return glm::vec3(0.0F, -1.0F, 0.0F);
    }
    return direction / length;
}

glm::vec3 TimeOfDay::sunDirection() const
{
    return aim("SunPos", "SunTargetPos");
}

render::SceneLighting TimeOfDay::lighting() const
{
    const float t = minutes();
    render::SceneLighting l;
    l.sunDirection = sunDirection();
    l.sunColour = m_curves.sample("SunColor", t) * m_curves.sampleScalar("SunColorMult", t);
    // The hemisphere pair the track shaders call skyColor and groundColor;
    // the executable samples these curves together with the sun's into the
    // one lighting block it hands the renderer (0x8265A7CC).
    l.ambientColour = m_curves.sample("AmbientColour", t);
    l.ambientTop = m_curves.sample("HLTopColour", t);
    l.ambientBottom = m_curves.sample("HLBottomColour", t);
    l.carAmbientTop = m_curves.sample("CarHLColourTop", t);
    l.carAmbientBottom = m_curves.sample("CarHLColourBottom", t);

    l.skyTop = m_curves.sample("AtmosphereTopColour", t);
    l.skyBottom = m_curves.sample("AtmosphereBottomColour", t);
    // The sky object raises the power to at least 0.01 (0x82E34728).
    l.skyColourPower = std::max(m_curves.sampleScalar("AtmosphereColourPower", t), 0.01F);
    l.skyClampTop = m_curves.sampleScalar("AtmosphereClampPower", t);
    l.skyClampBottom = m_curves.sampleScalar("AtmosphereBottomClampPower", t);
    l.skyGain = m_curves.sampleScalar("SkyGain", t);
    l.hazeColour = m_curves.sample("AtmosphereHazeColour", t);
    l.hazeDirection = -aim("HazeObjectPos", "HazeObjectTargetPos");
    l.hazeIntensity = m_curves.sampleScalar("AtmosphereHazeIntensity", t);
    l.hazePower = m_curves.sampleScalar("AtmosphereHazePower", t);
    l.hazeEllipticalScale = m_curves.sampleScalar("AtmosphereHazeEllipticalScale", t);
    // The sampler hands these to the sky together (0x8265A7CC). It prefers
    // the rotation curve SunObjectRot when that has keys; Colorado's has
    // none.
    l.sunObjectDirection = aim("SunObjectPos", "SunObjectTargetPos");
    l.sunObjectColour = m_curves.sample("SunObjectColour", t);
    l.sunObjectAlpha = m_curves.sampleScalar("SunObjectAlpha", t);
    l.sunObjectSize = m_curves.sampleScalar("SunObjectSize", t);
    l.sunObjectMoon = m_curves.sampleScalar("SunObjectMoon", t);
    l.starFade = m_curves.sampleScalar("StarFade", t);
    // Sampled with the others (0x8265AF60), unclamped.
    l.switchOnLights = m_curves.sampleScalar("SwitchOnLights", t);
    // The sampler stores it unclamped (0x8265A8FC); Colorado's runs from -1
    // at night to 1 by day.
    l.shadowIntensity = m_curves.sampleScalar("ShadowIntensity", t);

    const TimeOfDayCurves::FogTemplate fog = m_curves.fogTemplate().value_or(TimeOfDayCurves::FogTemplate{});
    l.fogColour = m_curves.sample("FogColour", t) * fog.colour;
    l.fogDensity = m_curves.sampleScalar("FogDensity", t) * fog.density * kFogDensityPerMetre;
    l.fogStart = m_curves.sampleScalar("FogStartDistance", t) * fog.startDistance;
    l.fogScatterPower = m_curves.sampleScalar("FogSunScatterPower", t) * fog.scatterPower;
    l.fogHeightFadeAmount = m_curves.sampleScalar("FogHeightFadeAmount", t) * fog.heightFadeAmount;
    l.fogHeightFadeStart = m_curves.sampleScalar("FogHeightFadeStart", t) * fog.heightFadeStart;
    l.fogHeightFadeEnd = m_curves.sampleScalar("FogHeightFadeEnd", t) * fog.heightFadeEnd;
    l.fogSkyHeightFadeStart = m_curves.sampleScalar("FogSkyHeightFadeStart", t) * fog.heightFadeStart;
    l.fogSkyHeightFadeEnd = m_curves.sampleScalar("FogSkyHeightFadeEnd", t) * fog.heightFadeEnd;
    if (m_curves.fogTemplate()) {
        l.fogScatterColour = m_curves.sample("FogSunScatterColour", t) * fog.scatterColour;
        l.fogScatterStart = fog.scatterStartDistance;
        l.fogScatterRamp = fog.scatterRampDistance;
    } else {
        l.fogScatterColour = l.fogColour;
    }
    return l;
}

float TimeOfDay::lights() const
{
    return std::clamp(m_curves.sampleScalar("SwitchOnLights", minutes()), 0.0F, 1.0F);
}

float TimeOfDay::stars() const
{
    return std::clamp(m_curves.sampleScalar("StarFade", minutes()), 0.0F, 1.0F);
}

} // namespace fh
