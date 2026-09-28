#include "TimeOfDay.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace fh {

namespace {

constexpr float kMinutesPerDay = 1440.0F;
constexpr float kSecondsPerDay = 86400.0F;

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
    const auto next = std::upper_bound(
        keys.begin(), keys.end(), t, [](float time, const Key& key) { return time < key.time; });
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

void TimeOfDay::setSeconds(float seconds)
{
    m_seconds = std::fmod(seconds, kSecondsPerDay);
    if (m_seconds < 0.0F) {
        m_seconds += kSecondsPerDay;
    }
}

glm::vec3 TimeOfDay::sunDirection() const
{
    const glm::vec3 towardsSun = m_curves.sample("SunPos", minutes());
    const float length = glm::length(towardsSun);
    if (length < 1e-4F) {
        return glm::vec3(0.0F, -1.0F, 0.0F);
    }
    return -towardsSun / length;
}

glm::vec3 TimeOfDay::sunColour() const
{
    return m_curves.sample("SunColor", minutes()) * m_curves.sampleScalar("SunColorMult", minutes());
}

glm::vec3 TimeOfDay::skyTopColour() const
{
    return m_curves.sample("AtmosphereTopColour", minutes()) * m_curves.sampleScalar("SkyGain", minutes());
}

glm::vec3 TimeOfDay::skyBottomColour() const
{
    return m_curves.sample("AtmosphereBottomColour", minutes()) * m_curves.sampleScalar("SkyGain", minutes());
}

glm::vec3 TimeOfDay::hazeColour() const
{
    return m_curves.sample("AtmosphereHazeColour", minutes());
}

float TimeOfDay::hazeIntensity() const
{
    return m_curves.sampleScalar("AtmosphereHazeIntensity", minutes());
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
