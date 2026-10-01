#include "PostProcessing.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace openhorizon {

namespace {

constexpr const char* kGradingMapFolder = "media/dynamicpost/colourgradingmaps/";
constexpr std::size_t kLutSide = 16;
constexpr std::size_t kDdsHeaderBytes = 128;
/// DDSD_DEPTH in the header flags and DDSCAPS2_VOLUME in caps2.
constexpr std::uint32_t kDdsDepthFlag = 0x800000;
constexpr std::uint32_t kDdsVolumeCaps = 0x200000;

pugi::xml_document parseXml(std::string_view text, std::string_view what)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text.data(), text.size());
    if (!parsed) {
        throw DataError("{}: {}", what, parsed.description());
    }
    return doc;
}

/// No default: those loaders parse into a reused stack buffer, so a missing attribute keeps stale data.
float required(const pugi::xml_node& node, const char* attribute, std::string_view what)
{
    const pugi::xml_attribute a = node.attribute(attribute);
    if (!a) {
        throw DataError("{} template {} has no {}", what, node.attribute("Name").value(), attribute);
    }
    return a.as_float();
}

pugi::xml_node templatesRoot(const pugi::xml_document& doc, std::string_view what)
{
    const pugi::xml_node root = doc.child("Templates");
    if (!root) {
        throw DataError("{} is not a Templates document", what);
    }
    return root;
}

template <typename T> std::optional<std::size_t> findByName(const std::vector<T>& items, std::string_view name)
{
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (items[i].name == name) {
            return i;
        }
    }
    return std::nullopt;
}

float cross(glm::vec2 a, glm::vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

/// The element's attribute, or DataError naming both.
float attributeOf(const pugi::xml_node& root, const char* element, const char* attribute)
{
    const pugi::xml_attribute a = root.child(element).attribute(attribute);
    if (!a) {
        throw DataError("TrackSettings.xml has no {} {}", element, attribute);
    }
    return a.as_float();
}

float lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

glm::vec3 lerp(const glm::vec3& a, const glm::vec3& b, float t)
{
    return a + (b - a) * t;
}

VignetteSettings lerp(const VignetteSettings& a, const VignetteSettings& b, float t)
{
    VignetteSettings v;
    v.angleDegrees = lerp(a.angleDegrees, b.angleDegrees, t);
    v.position = a.position + (b.position - a.position) * t;
    v.scale = a.scale + (b.scale - a.scale) * t;
    v.power = lerp(a.power, b.power, t);
    v.colour = a.colour + (b.colour - a.colour) * t;
    v.minIntensity = lerp(a.minIntensity, b.minIntensity, t);
    return v;
}

TimeOfDayCurves::FogTemplate lerp(const TimeOfDayCurves::FogTemplate& a, const TimeOfDayCurves::FogTemplate& b, float t)
{
    TimeOfDayCurves::FogTemplate f;
    f.colour = lerp(a.colour, b.colour, t);
    f.density = lerp(a.density, b.density, t);
    f.startDistance = lerp(a.startDistance, b.startDistance, t);
    f.scatterColour = lerp(a.scatterColour, b.scatterColour, t);
    f.scatterPower = lerp(a.scatterPower, b.scatterPower, t);
    f.heightFadeAmount = lerp(a.heightFadeAmount, b.heightFadeAmount, t);
    f.heightFadeStart = lerp(a.heightFadeStart, b.heightFadeStart, t);
    f.heightFadeEnd = lerp(a.heightFadeEnd, b.heightFadeEnd, t);
    f.scatterStartDistance = lerp(a.scatterStartDistance, b.scatterStartDistance, t);
    f.scatterRampDistance = lerp(a.scatterRampDistance, b.scatterRampDistance, t);
    return f;
}

/// The fog outside every zone (0x82556A30).
TimeOfDayCurves::FogTemplate defaultFog()
{
    TimeOfDayCurves::FogTemplate f;
    f.colour = glm::vec3(1.0F);
    f.density = 1.0F;
    f.startDistance = 1.0F;
    f.scatterColour = glm::vec3(1.0F);
    f.scatterPower = 1.0F;
    f.heightFadeAmount = 1.0F;
    f.heightFadeStart = 1.0F;
    f.heightFadeEnd = 1.0F;
    f.scatterStartDistance = 2000.0F;
    f.scatterRampDistance = 2000.0F;
    return f;
}

/// 1, or the curve where it has keys (0x8265BC6C resets each field before sampling).
float multiplier(const TimeOfDayCurves& curves, std::string_view curve, float minutes, float reset = 1.0F)
{
    return curves.has(curve) ? curves.sampleScalar(curve, minutes) : reset;
}

glm::vec3 multiplier3(const TimeOfDayCurves& curves, std::string_view curve, float minutes)
{
    return curves.has(curve) ? curves.sample(curve, minutes) : glm::vec3(1.0F);
}

std::uint32_t le32(std::span<const std::uint8_t> bytes, std::size_t at)
{
    return std::uint32_t(bytes[at]) | (std::uint32_t(bytes[at + 1]) << 8) | (std::uint32_t(bytes[at + 2]) << 16)
        | (std::uint32_t(bytes[at + 3]) << 24);
}

GradingLut loadLut(const Vfs& vfs, const std::string& path)
{
    const std::optional<Bytes> data = vfs.read(path);
    if (!data) {
        throw DataError("{} not found", path);
    }
    return parseGradingLut(*data, path);
}

} // namespace

bool PostZone::contains(glm::vec2 xz) const
{
    for (const std::array<glm::vec2, 3>& t : triangles) {
        const float c0 = cross(t[1] - t[0], xz - t[0]);
        const float c1 = cross(t[2] - t[1], xz - t[1]);
        const float c2 = cross(t[0] - t[2], xz - t[2]);
        if ((c0 > 0.0F && c1 > 0.0F && c2 > 0.0F) || (c0 < 0.0F && c1 < 0.0F && c2 < 0.0F)) {
            return true;
        }
    }
    return false;
}

PostTemplates parsePostTemplates(std::string_view bloomXml, std::string_view colourSettingsXml,
    std::string_view vignetteXml, std::string_view fogXml)
{
    PostTemplates out;

    const pugi::xml_document bloomDoc = parseXml(bloomXml, "bloom_templates.xml");
    for (const pugi::xml_node node : templatesRoot(bloomDoc, "bloom_templates.xml").children("Template")) {
        PostTemplates::Bloom b;
        b.name = node.attribute("Name").value();
        b.cutoff = required(node, "Cutoff", "bloom");
        b.scale = required(node, "Scale", "bloom");
        b.persist = required(node, "Persist", "bloom");
        out.bloom.push_back(std::move(b));
    }

    const pugi::xml_document colourDoc = parseXml(colourSettingsXml, "colorsettings_templates.xml");
    for (const pugi::xml_node node : templatesRoot(colourDoc, "colorsettings_templates.xml").children("Template")) {
        PostTemplates::ColourSettings c;
        c.name = node.attribute("Name").value();
        c.desaturation = required(node, "DesatAmount", "colour settings");
        c.luminosity = {required(node, "DesatLuminosityR", "colour settings"),
            required(node, "DesatLuminosityG", "colour settings"),
            required(node, "DesatLuminosityB", "colour settings")};
        // The game's loader (0x8257A070) reads blue from DesatPostModR too.
        c.postMod = {required(node, "DesatPostModR", "colour settings"),
            required(node, "DesatPostModG", "colour settings"), required(node, "DesatPostModR", "colour settings")};
        c.exposureBias = required(node, "ExposureBias", "colour settings");
        c.brightness = required(node, "Brightness", "colour settings");
        c.contrast = required(node, "Contrast", "colour settings");
        out.colourSettings.push_back(std::move(c));
    }

    const pugi::xml_document vignetteDoc = parseXml(vignetteXml, "vignette_templates.xml");
    for (const pugi::xml_node node : templatesRoot(vignetteDoc, "vignette_templates.xml").children("Template")) {
        PostTemplates::Vignette v;
        v.name = node.attribute("Name").value();
        VignetteSettings& s = v.settings;
        s.angleDegrees = required(node, "Angle", "vignette");
        s.position = {required(node, "PosX", "vignette"), required(node, "PosY", "vignette")};
        s.scale = {required(node, "ScaleX", "vignette"), required(node, "ScaleY", "vignette")};
        s.power = required(node, "Power", "vignette");
        s.colour = {required(node, "ColorR", "vignette"), required(node, "ColorG", "vignette"),
            required(node, "ColorB", "vignette"), required(node, "ColorA", "vignette")};
        s.minIntensity = required(node, "MinIntensity", "vignette");
        out.vignette.push_back(std::move(v));
    }

    // The fog loader (0x8257A368) starts every template from its defaults.
    const pugi::xml_document fogDoc = parseXml(fogXml, "fog_templates.xml");
    for (const pugi::xml_node node : templatesRoot(fogDoc, "fog_templates.xml").children("Template")) {
        PostTemplates::Fog f;
        f.name = node.attribute("Name").value();
        TimeOfDayCurves::FogTemplate& s = f.settings;
        s.colour = {node.attribute("ColorR").as_float(0.5F), node.attribute("ColorG").as_float(0.5F),
            node.attribute("ColorB").as_float(0.5F)};
        s.density = node.attribute("Density").as_float(0.02F);
        s.startDistance = node.attribute("StartDistance").as_float(10.0F);
        s.scatterColour = {node.attribute("SunScatterColorR").as_float(0.5F),
            node.attribute("SunScatterColorG").as_float(0.5F), node.attribute("SunScatterColorB").as_float(0.5F)};
        s.scatterPower = node.attribute("SunScatterPower").as_float(10.0F);
        s.scatterStartDistance = node.attribute("SunScatterStartDistance").as_float(2000.0F);
        s.scatterRampDistance = node.attribute("SunScatterRampupDistance").as_float(2000.0F);
        s.heightFadeAmount = node.attribute("HeightFadeAmount").as_float(0.8F);
        s.heightFadeStart = node.attribute("HeightFadeStart").as_float(100.0F);
        s.heightFadeEnd = node.attribute("HeightFadeEnd").as_float(400.0F);
        out.fog.push_back(std::move(f));
    }
    return out;
}

std::vector<PostZone> parsePostZones(std::string_view xml, const PostTemplates& templates)
{
    const pugi::xml_document doc = parseXml(xml, "PostProcessingZones_safe.xml");
    const pugi::xml_node root = doc.child("PostProcessingZones");
    if (!root) {
        throw DataError("PostProcessingZones_safe.xml is not a PostProcessingZones document");
    }
    std::vector<PostZone> zones;
    for (const pugi::xml_node node : root.children("PostProcessingZone")) {
        PostZone zone;
        zone.name = node.attribute("Name").value();
        const auto bloom = findByName(templates.bloom, node.attribute("BloomTemplate").value());
        const auto top = findByName(templates.vignette, node.attribute("VignetteTop").value());
        const auto bottom = findByName(templates.vignette, node.attribute("VignetteBottom").value());
        const auto colour = findByName(templates.colourSettings, node.attribute("ColorSettings").value());
        const auto fog = findByName(templates.fog, node.attribute("Fog").value());
        if (!bloom || !top || !bottom || !colour || !fog) {
            logWarning("post-processing: zone {} names a template that does not exist and is left out", zone.name);
            continue;
        }
        zone.bloom = *bloom;
        zone.vignetteTop = *top;
        zone.vignetteBottom = *bottom;
        zone.colourSettings = *colour;
        zone.fog = *fog;
        zone.lerpTime = node.attribute("LerpTime").as_float();
        zone.gradingMap = node.attribute("ColorGradingMap").value();
        zone.gradingMapNight = node.attribute("ColorGradingMapNight").value();
        const unsigned count = node.attribute("NumTriangles").as_uint();
        for (const pugi::xml_node triangle : node.children("PostProcessingZoneTriangle")) {
            if (zone.triangles.size() >= count) {
                break;
            }
            std::array<glm::vec2, 3> points{};
            std::size_t n = 0;
            for (const pugi::xml_node point : triangle.children("PostProcessingZoneTrianglePoint")) {
                if (n == points.size()) {
                    break;
                }
                points[n++] = {point.attribute("posX").as_float(), point.attribute("posZ").as_float()};
            }
            if (n == points.size()) {
                zone.triangles.push_back(points);
            }
        }
        zones.push_back(std::move(zone));
    }
    return zones;
}

void PostZoneTracker::update(const std::vector<PostZone>& zones, std::optional<glm::vec2> position, float dt)
{
    if (!position) {
        m_previous = m_target = m_source = kNone;
        m_elapsed = 0.0F;
        return;
    }
    int current = kNone;
    for (std::size_t i = 0; i < zones.size(); ++i) {
        if (zones[i].contains(*position)) {
            current = static_cast<int>(i);
            break;
        }
    }
    const auto lerpTime = [&](int zone, int other) {
        const int index = zone != kNone ? zone : other;
        return index != kNone ? zones[static_cast<std::size_t>(index)].lerpTime : 0.0F;
    };
    if (current == m_previous) {
        m_elapsed += dt;
    } else {
        if (current == m_source) {
            // Turning back into the zone being left reverses the blend.
            const float previousLerp = lerpTime(m_previous, current);
            const float done = previousLerp > 0.0F ? std::clamp(m_elapsed / previousLerp, 0.0F, 1.0F) : 1.0F;
            m_elapsed = (1.0F - done) * lerpTime(current, m_previous);
        } else {
            m_elapsed = 0.0F;
        }
        m_source = m_previous;
    }
    m_previous = current;
    m_target = current;
    if (m_source == kNone) {
        m_elapsed = std::numeric_limits<float>::max();
    }
}

float PostZoneTracker::blend(const std::vector<PostZone>& zones) const
{
    if (m_target == kNone && m_source == kNone) {
        return 1.0F;
    }
    const int zone = m_target != kNone ? m_target : m_source;
    const float lerpTime = zones[static_cast<std::size_t>(zone)].lerpTime;
    const float x = lerpTime > 0.0F ? std::clamp(m_elapsed / lerpTime, 0.0F, 1.0F) : 1.0F;
    return x * x * (3.0F - 2.0F * x);
}

TrackPostSettings parseTrackPostSettings(std::string_view trackSettingsXml)
{
    const pugi::xml_document doc = parseXml(trackSettingsXml, "TrackSettings.xml");
    const pugi::xml_node root = doc.child("TrackSettings");
    if (!root) {
        throw DataError("TrackSettings.xml is not a TrackSettings document");
    }
    PostSettings s;

    const pugi::xml_node bloom = root.child("bloom");
    s.bloomCutoff = attributeOf(bloom, "cutoff", "value");
    s.bloomLowerGradient = attributeOf(bloom, "curve", "lowerGradient");
    s.bloomUpperGradient = attributeOf(bloom, "curve", "upperGradient");
    s.bloomScale = attributeOf(bloom, "scale", "value");
    s.bloomPersist = attributeOf(bloom, "persist", "value");
    for (std::size_t i = 0; i < s.bloomWeights.size(); ++i) {
        const std::string layer = std::format("layer{}", i);
        s.bloomWeights[i] = attributeOf(bloom, "weights", layer.c_str());
    }

    s.desaturation = attributeOf(root, "DesaturateAmount", "value");
    s.desatLuminosity = {attributeOf(root, "DesatLuminosity", "r"), attributeOf(root, "DesatLuminosity", "g"),
        attributeOf(root, "DesatLuminosity", "b")};
    s.desatPostMod = {attributeOf(root, "DesatPostMod", "r"), attributeOf(root, "DesatPostMod", "g"),
        attributeOf(root, "DesatPostMod", "b")};
    s.exposureBias = attributeOf(root, "ExposureBias", "value");
    s.brightness = attributeOf(root, "Brightness", "value");
    s.contrast = attributeOf(root, "Contrast", "value");
    s.tonemapDelay = attributeOf(root, "ToneMapeDelay", "val");
    s.cameraExposureRange
        = {attributeOf(root, "CameraExposureRange", "minval"), attributeOf(root, "CameraExposureRange", "maxval")};

    // ToneMapCockpitExposure is for the cockpit view, which this game lacks.
    const char* const adaptive = "ToneMapAdaptiveExposure";
    s.adaptiveKeys = {attributeOf(root, adaptive, "Key_ExtraDark"), attributeOf(root, adaptive, "Key_Dark"),
        attributeOf(root, adaptive, "Key_Light"), attributeOf(root, adaptive, "Key_ExtraLight")};
    s.adaptiveExposures = {attributeOf(root, adaptive, "Exp_ExtraDark"), attributeOf(root, adaptive, "Exp_Dark"),
        attributeOf(root, adaptive, "Exp_Light"), attributeOf(root, adaptive, "Exp_ExtraLight")};

    const char* const filmic = "filmicTone";
    s.filmic.white = attributeOf(root, filmic, "white");
    s.filmic.shoulderStrength = attributeOf(root, filmic, "shoulderStrength");
    s.filmic.linearStrength = attributeOf(root, filmic, "linearStrength");
    s.filmic.linearAngle = attributeOf(root, filmic, "linearAngle");
    s.filmic.toeStrength = attributeOf(root, filmic, "toeStrength");
    s.filmic.toeNumerator = attributeOf(root, filmic, "toeNumerator");
    s.filmic.toeDenominator = attributeOf(root, filmic, "toeDenominator");
    s.filmic.exposure = attributeOf(root, filmic, "exposure");
    FilmicCurve night = s.filmic;
    const pugi::xml_node tone = root.child(filmic);
    night.white = tone.attribute("whiteNight").as_float(night.white);
    night.shoulderStrength = tone.attribute("shoulderStrengthNight").as_float(night.shoulderStrength);
    night.linearStrength = tone.attribute("linearStrengthNight").as_float(night.linearStrength);
    night.linearAngle = tone.attribute("linearAngleNight").as_float(night.linearAngle);
    night.toeStrength = tone.attribute("toeStrengthNight").as_float(night.toeStrength);
    night.toeNumerator = tone.attribute("toeNumeratorNight").as_float(night.toeNumerator);
    night.toeDenominator = tone.attribute("toeDenominatorNight").as_float(night.toeDenominator);
    night.exposure = tone.attribute("exposureNight").as_float(night.exposure);

    // The top vignette; the bottom one starts as a copy of it (0x825E6D08).
    const pugi::xml_node vignette = root.child("vignette");
    VignetteSettings& v = s.vignetteTop;
    v.angleDegrees = attributeOf(vignette, "angle", "value");
    v.position = {attributeOf(vignette, "pos", "x"), attributeOf(vignette, "pos", "y")};
    v.scale = {attributeOf(vignette, "scale", "x"), attributeOf(vignette, "scale", "y")};
    v.power = attributeOf(vignette, "power", "value");
    v.colour = {attributeOf(vignette, "color", "r"), attributeOf(vignette, "color", "g"),
        attributeOf(vignette, "color", "b"), 0.0F};
    v.minIntensity = attributeOf(vignette, "minIntensity", "value");
    s.vignetteBottom = v;
    return {s, night};
}

GradingLut parseGradingLut(std::span<const std::uint8_t> dds, std::string_view source)
{
    constexpr std::size_t kTexels = kLutSide * kLutSide * kLutSide;
    if (dds.size() < kDdsHeaderBytes + kTexels * 4 || std::memcmp(dds.data(), "DDS ", 4) != 0) {
        throw DataError("{} is not a DDS file of 16x16x16 texels", source);
    }
    const std::uint32_t flags = le32(dds, 8);
    const std::uint32_t height = le32(dds, 12);
    const std::uint32_t width = le32(dds, 16);
    const std::uint32_t depth = le32(dds, 24);
    const std::uint32_t bits = le32(dds, 88);
    const std::uint32_t redMask = le32(dds, 92);
    const std::uint32_t greenMask = le32(dds, 96);
    const std::uint32_t blueMask = le32(dds, 100);
    const std::uint32_t caps2 = le32(dds, 112);
    if ((flags & kDdsDepthFlag) == 0 || (caps2 & kDdsVolumeCaps) == 0 || width != kLutSide || height != kLutSide
        || depth != kLutSide || bits != 32 || redMask != 0xFF0000 || greenMask != 0xFF00 || blueMask != 0xFF) {
        throw DataError("{} is not a 16x16x16 A8R8G8B8 volume", source);
    }
    GradingLut lut{};
    for (std::size_t i = 0; i < kTexels; ++i) {
        const std::uint32_t texel = le32(dds, kDdsHeaderBytes + i * 4);
        lut[i * 4 + 0] = static_cast<std::uint8_t>((texel >> 16) & 0xFF);
        lut[i * 4 + 1] = static_cast<std::uint8_t>((texel >> 8) & 0xFF);
        lut[i * 4 + 2] = static_cast<std::uint8_t>(texel & 0xFF);
        lut[i * 4 + 3] = static_cast<std::uint8_t>((texel >> 24) & 0xFF);
    }
    return lut;
}

GradingLut blendGradingLuts(const GradingLut& fromDay, const GradingLut& fromNight, const GradingLut& toDay,
    const GradingLut& toNight, float t, float day, float night)
{
    GradingLut out{};
    for (std::size_t z = 0; z < kLutSide; ++z) {
        for (std::size_t y = 0; y < kLutSide; ++y) {
            for (std::size_t x = 0; x < kLutSide; ++x) {
                const std::size_t texel = ((z * kLutSide + y) * kLutSide + x) * 4;
                const std::array<float, 3> identity{
                    static_cast<float>(x) / 15.0F, static_cast<float>(y) / 15.0F, static_cast<float>(z) / 15.0F};
                for (std::size_t c = 0; c < 3; ++c) {
                    const auto value = [&](const GradingLut& lut) {
                        return static_cast<float>(lut[texel + c]) / 255.0F - identity[c];
                    };
                    const float from = day * value(fromDay) + night * value(fromNight);
                    const float to = day * value(toDay) + night * value(toNight);
                    const float v = std::clamp(identity[c] + from + (to - from) * t, 0.0F, 1.0F);
                    // Rounding to nearest is inferred.
                    out[texel + c] = static_cast<std::uint8_t>(std::lround(v * 255.0F));
                }
                out[texel + 3] = 255;
            }
        }
    }
    return out;
}

PostProcessing PostProcessing::load(const Vfs& vfs, std::string_view track)
{
    const auto text = [&](const std::string& path) {
        std::optional<std::string> t = vfs.readText(path);
        if (!t) {
            throw DataError("{} not found", path);
        }
        return std::move(*t);
    };
    const std::string folder = std::format("media/dynamicpost/tracks/{}/", track);
    PostProcessing p;
    p.m_templates
        = parsePostTemplates(text(folder + "bloom_templates.xml"), text(folder + "colorsettings_templates.xml"),
            text(folder + "vignette_templates.xml"), text(folder + "fog_templates.xml"));
    p.m_zones = parsePostZones(
        text(std::format("media/tracks/{}/Ribbon_00/PostProcessingZones_safe.xml", track)), p.m_templates);
    const TrackPostSettings trackSettings
        = parseTrackPostSettings(text(std::format("media/tracks/{}/TrackSettings.xml", track)));
    p.m_default = trackSettings.settings;
    p.m_filmicNight = trackSettings.filmicNight;

    // colorgradinglookup%02d.dds when there is one (0x82E21770); Colorado has only the unnumbered file.
    p.m_defaultLuts.day = loadLut(vfs, std::format("media/tracks/{}/colorgradinglookup.dds", track));
    p.m_defaultLuts.night = loadLut(vfs, std::format("media/tracks/{}/colorgradinglookup_night.dds", track));
    for (const PostZone& zone : p.m_zones) {
        ZoneLuts luts;
        luts.day = loadLut(vfs, kGradingMapFolder + zone.gradingMap);
        luts.night = loadLut(vfs, kGradingMapFolder + zone.gradingMapNight);
        p.m_zoneLuts.push_back(luts);
    }
    p.m_settings = p.m_default;
    p.m_fog = defaultFog();
    p.m_lut = p.m_defaultLuts.day;
    logInfo("post-processing: {} zones, {} bloom, {} colour, {} vignette and {} fog templates", p.m_zones.size(),
        p.m_templates.bloom.size(), p.m_templates.colourSettings.size(), p.m_templates.vignette.size(),
        p.m_templates.fog.size());
    return p;
}

void PostProcessing::update(std::optional<glm::vec2> position, float dt, const TimeOfDayCurves& curves, float minutes)
{
    m_tracker.update(m_zones, position, dt);
    const float t = m_tracker.blend(m_zones);
    const int from = m_tracker.source();
    const int to = m_tracker.target();
    const auto zone = [&](int index) -> const PostZone* {
        return index == PostZoneTracker::kNone ? nullptr : &m_zones[static_cast<std::size_t>(index)];
    };
    const PostZone* a = zone(from);
    const PostZone* b = zone(to);

    PostSettings s = m_default;
    // DayNightPostProcess, sampled into the lighting block's +0x70 (0x8265AA88) and stored at track settings +0x268
    // (0x82E114D8), lerps the two (0x825E6D08); without it the day curve stays (the start value is untraced).
    const float night = multiplier(curves, "DayNightPostProcess", minutes, 0.0F);
    const FilmicCurve& day = m_default.filmic;
    s.filmic.white = lerp(day.white, m_filmicNight.white, night);
    s.filmic.shoulderStrength = lerp(day.shoulderStrength, m_filmicNight.shoulderStrength, night);
    s.filmic.linearStrength = lerp(day.linearStrength, m_filmicNight.linearStrength, night);
    s.filmic.linearAngle = lerp(day.linearAngle, m_filmicNight.linearAngle, night);
    s.filmic.toeStrength = lerp(day.toeStrength, m_filmicNight.toeStrength, night);
    s.filmic.toeNumerator = lerp(day.toeNumerator, m_filmicNight.toeNumerator, night);
    s.filmic.toeDenominator = lerp(day.toeDenominator, m_filmicNight.toeDenominator, night);
    s.filmic.exposure = lerp(day.exposure, m_filmicNight.exposure, night);

    const auto bloomOf = [&](const PostZone* z) {
        if (z == nullptr) {
            return PostTemplates::Bloom{{}, m_default.bloomScale, m_default.bloomCutoff, m_default.bloomPersist};
        }
        return m_templates.bloom[z->bloom];
    };
    const PostTemplates::Bloom bloomA = bloomOf(a);
    const PostTemplates::Bloom bloomB = bloomOf(b);
    s.bloomScale = lerp(bloomA.scale, bloomB.scale, t) * multiplier(curves, "BloomScaleMultiplier", minutes);
    s.bloomCutoff = lerp(bloomA.cutoff, bloomB.cutoff, t) * multiplier(curves, "BloomCutoffMultiplier", minutes);
    s.bloomPersist = lerp(bloomA.persist, bloomB.persist, t) * multiplier(curves, "BloomPersistMultiplier", minutes);
    s.bloomLowerGradient
        = m_default.bloomLowerGradient * multiplier(curves, "BloomLowerGradientMultiplier", minutes, 0.0F);
    s.bloomUpperGradient = m_default.bloomUpperGradient * multiplier(curves, "BloomUpperGradientMultiplier", minutes);

    const auto colourOf = [&](const PostZone* z) {
        if (z == nullptr) {
            return PostTemplates::ColourSettings{{}, m_default.desaturation, m_default.desatLuminosity,
                m_default.desatPostMod, m_default.exposureBias, m_default.brightness, m_default.contrast};
        }
        return m_templates.colourSettings[z->colourSettings];
    };
    const PostTemplates::ColourSettings colourA = colourOf(a);
    const PostTemplates::ColourSettings colourB = colourOf(b);
    s.desaturation
        = lerp(colourA.desaturation, colourB.desaturation, t) * multiplier(curves, "DesatAmountMultiplier", minutes);
    s.desatLuminosity
        = lerp(colourA.luminosity, colourB.luminosity, t) * multiplier3(curves, "DesatLuminosityMultiplier", minutes);
    s.desatPostMod = lerp(colourA.postMod, colourB.postMod, t) * multiplier3(curves, "DesatPostModMultiplier", minutes);
    s.exposureBias
        = lerp(colourA.exposureBias, colourB.exposureBias, t) * multiplier(curves, "ExposureBiasMultiplier", minutes);
    s.brightness
        = lerp(colourA.brightness, colourB.brightness, t) * multiplier(curves, "BrightnessMultiplier", minutes);
    s.contrast = lerp(colourA.contrast, colourB.contrast, t) * multiplier(curves, "ContrastMultiplier", minutes);

    const auto vignetteOf = [&](const PostZone* z, bool top) {
        if (z == nullptr) {
            return top ? m_default.vignetteTop : m_default.vignetteBottom;
        }
        return m_templates.vignette[top ? z->vignetteTop : z->vignetteBottom].settings;
    };
    // Alpha is minIntensity times VignetteAmount (0x8255E2FC) times a camera factor that is 0 only in the
    // driver's view (0x82905050, DriverCam in the table at 0x83336B78), which this game lacks.
    const float amount = multiplier(curves, "VignetteAmount", minutes);
    s.vignetteTop = lerp(vignetteOf(a, true), vignetteOf(b, true), t);
    s.vignetteTop.colour.a = s.vignetteTop.minIntensity * amount;
    s.vignetteBottom = lerp(vignetteOf(a, false), vignetteOf(b, false), t);
    s.vignetteBottom.colour.a = s.vignetteBottom.minIntensity * amount;
    m_settings = s;

    const auto fogOf
        = [&](const PostZone* z) { return z == nullptr ? defaultFog() : m_templates.fog[z->fog].settings; };
    m_fog = lerp(fogOf(a), fogOf(b), t);

    const ZoneLuts& lutsA = a == nullptr ? m_defaultLuts : m_zoneLuts[static_cast<std::size_t>(from)];
    const ZoneLuts& lutsB = b == nullptr ? m_defaultLuts : m_zoneLuts[static_cast<std::size_t>(to)];
    m_lut = blendGradingLuts(lutsA.day, lutsA.night, lutsB.day, lutsB.night, t,
        multiplier(curves, "DayColorGradeAmount", minutes), multiplier(curves, "NightColorGradeAmount", minutes, 0.0F));
}

render::PostFrame PostProcessing::frame(float aspect, bool reset) const
{
    const PostSettings& s = m_settings;
    render::PostFrame f;
    f.bloomScale = s.bloomScale;
    f.bloomCutoff = s.bloomCutoff;
    f.bloomPersist = s.bloomPersist;
    f.bloomLowerGradient = s.bloomLowerGradient;
    f.bloomUpperGradient = s.bloomUpperGradient;
    f.bloomWeights = s.bloomWeights;
    // Keys over 255 (0x82487C9C). ExposureShiftScale (0x825E5DD0) is 1 (0x82E48438) except in
    // renderscenarios/splitscreen_race.xml.
    f.luminanceKeys = s.adaptiveKeys / 255.0F;
    f.exposures = s.adaptiveExposures;
    // Filmic exposure plus the blended exposure bias (0x82487C9C adds renderer +0x1390 to +0x13FC).
    f.exposureBias = s.filmic.exposure + s.exposureBias;
    f.filmic = render::post::Filmic{s.filmic.white, s.filmic.shoulderStrength, s.filmic.linearStrength,
        s.filmic.linearAngle, s.filmic.toeStrength, s.filmic.toeNumerator, s.filmic.toeDenominator};
    // tonemapDelay (+0x80, renderer +0x13A0), applied once per frame whatever its length.
    f.adaptation = s.tonemapDelay;
    f.resetAdaptation = reset;
    // Grading is on (manager +0x24); the renderer flag at +0xC63 is untraced and taken as clear. The fade to a
    // colour (+0x16D0..+0x16DC) and photo mode filters (+0x16EC..+0x1724) are left out.
    f.colourTransform
        = render::post::colourTransform(s.contrast, s.brightness, s.desaturation, s.desatLuminosity, s.desatPostMod);
    f.gradingLut = m_lut;
    const auto vignette = [&](const VignetteSettings& v) {
        render::PostVignette out;
        out.transform = render::post::vignetteTransform(glm::radians(v.angleDegrees), v.scale, aspect);
        out.position = v.position;
        out.power = v.power;
        out.colour = v.colour;
        return out;
    };
    f.vignetteTop = vignette(s.vignetteTop);
    f.vignetteBottom = vignette(s.vignetteBottom);
    return f;
}

std::string_view PostProcessing::zoneName() const
{
    const int target = m_tracker.target();
    return target == PostZoneTracker::kNone ? std::string_view() : m_zones[static_cast<std::size_t>(target)].name;
}

} // namespace openhorizon
