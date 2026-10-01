#include "CloudDefs.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace openhorizon::render {

namespace {

constexpr const char* kCloudDefsPath = "media/realtimesky/clouddefs.xml";

/// The layer names the readers compare against (0x8224D380-0x8224D3B0), in
/// the order of CloudDefinitions::Layer.
constexpr std::array<const char*, 4> kLayerNames{"closeCloudFront", "closeCloudBack", "farCloudFront", "farCloudBack"};

/// The time-of-day speeds and turns are per hour (0x82000D7C holds 1/3600).
constexpr float kPerHour = 1.0F / 3600.0F;
/// The far cloud object's field 416 (constructor 0x82E37EE8).
constexpr float kFarMapScrollScale = 0.4F;

std::optional<CloudDefinitions::Layer> layerNamed(std::string_view name)
{
    for (std::size_t i = 0; i < kLayerNames.size(); ++i) {
        if (kLayerNames[i] == name) {
            return static_cast<CloudDefinitions::Layer>(i);
        }
    }
    return std::nullopt;
}

/// Wraps as the updates do, keeping exactly 1.
float wrapUnit(float value)
{
    while (value < 0.0F) {
        value += 1.0F;
    }
    while (value > 1.0F) {
        value -= 1.0F;
    }
    return value;
}

void scrollFarLayer(CloudMotion::FarLayer& layer, const FarCloudLayerParameters& p, float seconds)
{
    const glm::vec2 step = glm::vec2(p.speedX, p.speedZ) * kPerHour * seconds;
    layer.mask1 = glm::vec2(wrapUnit(layer.mask1.x + step.x), wrapUnit(layer.mask1.y + step.y));
    layer.mask2 = glm::vec2(wrapUnit(layer.mask2.x + step.x), wrapUnit(layer.mask2.y + step.y));
    const glm::vec2 mapStep = step * kFarMapScrollScale;
    layer.map = glm::vec2(wrapUnit(layer.map.x + mapStep.x), wrapUnit(layer.map.y + mapStep.y));
}

void sampleFar(const CloudCurveLookup& curve, const char* prefix, FarCloudLayerParameters& p)
{
    const auto scalar = [&](const char* name, float& out) {
        if (const std::optional<glm::vec3> v = curve(std::format("{}{}", prefix, name))) {
            out = v->x;
        }
    };
    const auto colour = [&](const char* name, glm::vec3& out) {
        if (const std::optional<glm::vec3> v = curve(std::format("{}{}", prefix, name))) {
            out = *v;
        }
    };
    scalar("SpeedX", p.speedX);
    scalar("SpeedZ", p.speedZ);
    scalar("Coverage", p.coverage);
    colour("DenseColour", p.denseColour);
    colour("ThinColour", p.thinColour);
    scalar("DenseIntensity", p.denseIntensity);
    scalar("ThinIntensity", p.thinIntensity);
    scalar("CentreAlpha", p.centreAlpha);
    scalar("EdgeAlpha", p.edgeAlpha);
    scalar("AlphaPower", p.alphaPower);
}

void sampleClose(const CloudCurveLookup& curve, CloseCloudParameters& p)
{
    const auto scalar = [&](const char* name, float& out) {
        if (const std::optional<glm::vec3> v = curve(std::format("CloseCloud{}", name))) {
            out = v->x;
        }
    };
    const auto colour = [&](const char* name, glm::vec3& out) {
        if (const std::optional<glm::vec3> v = curve(std::format("CloseCloud{}", name))) {
            out = *v;
        }
    };
    colour("DenseColour", p.denseColour);
    colour("ThinColour", p.thinColour);
    colour("DenseColourBack", p.denseColourBack);
    colour("ThinColourBack", p.thinColourBack);
    colour("TopColour", p.topColour);
    colour("BottomColour", p.bottomColour);
    colour("BottomColourBack", p.bottomColourBack);
    colour("BackColour", p.backColour);
    scalar("DenseColourIntensity", p.denseColourIntensity);
    scalar("ThinColourIntensity", p.thinColourIntensity);
    scalar("DenseColourBackIntensity", p.denseColourBackIntensity);
    scalar("ThinColourBackIntensity", p.thinColourBackIntensity);
    scalar("TopPower", p.topPower);
    scalar("TopIntensity", p.topIntensity);
    scalar("BottomPower", p.bottomPower);
    scalar("BottomIntensity", p.bottomIntensity);
    scalar("BottomPowerBack", p.bottomPowerBack);
    scalar("BottomIntensityBack", p.bottomIntensityBack);
    scalar("BackPower", p.backPower);
    scalar("BackIntensity", p.backIntensity);
    scalar("TopMaskPower", p.topMaskPower);
    scalar("BottomMaskPower", p.bottomMaskPower);
    scalar("FrontAlpha", p.frontAlpha);
    scalar("BackAlpha", p.backAlpha);
    scalar("TopAlpha", p.topAlpha);
    scalar("BottomAlpha", p.bottomAlpha);
    scalar("AlphaCurve", p.alphaCurve);
    scalar("FrontRevs", p.frontRevs);
    scalar("BackRevs", p.backRevs);
}

std::string realtimeSkyPath(std::string_view stem, std::string_view suffix, int index)
{
    return std::format("media/realtimesky/{}{}{}.xds", stem, suffix, index);
}

} // namespace

bool CloudDefinitions::Set::valid() const
{
    return std::all_of(textures.begin(), textures.end(), [](int index) { return index >= 0; });
}

CloudDefinitions CloudDefinitions::parse(std::string_view xml)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        throw DataError("{}: {}", kCloudDefsPath, parsed.description());
    }
    const pugi::xml_node root = doc.child("cloudDefinitions");
    if (!root) {
        throw DataError("{} is not a cloudDefinitions document", kCloudDefsPath);
    }
    CloudDefinitions defs;
    for (const pugi::xml_node node : root.children("cloudTransition")) {
        Transition transition;
        transition.name = node.attribute("name").as_string();
        for (const pugi::xml_node layer : node.children("layer")) {
            const std::string_view name = layer.attribute("name").as_string();
            const std::optional<Layer> which = layerNamed(name);
            if (!which) {
                throw DataError("{}: transition {} names unknown layer {}", kCloudDefsPath, transition.name, name);
            }
            transition.layers.push_back({*which, layer.attribute("time").as_float()});
        }
        defs.transitions.push_back(std::move(transition));
    }
    for (const pugi::xml_node node : root.children("cloudSet")) {
        Set set;
        set.name = node.attribute("name").as_string();
        set.overcast = node.attribute("overcast").as_bool();
        for (std::size_t i = 0; i < kLayerNames.size(); ++i) {
            set.textures[i] = node.attribute(kLayerNames[i]).as_int(-1);
        }
        defs.sets.push_back(std::move(set));
    }
    return defs;
}

CloudDefinitions CloudDefinitions::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kCloudDefsPath);
    if (!text) {
        throw DataError("{} is missing", kCloudDefsPath);
    }
    return parse(*text);
}

const CloudDefinitions::Set* CloudDefinitions::findSet(std::string_view name) const
{
    const auto it = std::find_if(sets.begin(), sets.end(), [name](const Set& s) { return s.name == name; });
    return it == sets.end() ? nullptr : &*it;
}

std::string farCloudMapPath(std::string_view suffix, int index)
{
    return realtimeSkyPath("FarCloudMap", suffix, index);
}

std::string farCloudMaskPath()
{
    return "media/realtimesky/FarCloudMask.xds";
}

std::string closeCloudAlphaDensityPath(std::string_view suffix, int index)
{
    return realtimeSkyPath("CloseCloudAlphaDensity", suffix, index);
}

std::string closeCloudTopBottomAmbientPath(std::string_view suffix, int index)
{
    return realtimeSkyPath("CloseCloudTopBottomAmbient", suffix, index);
}

std::string closeCloudAmbientPath(std::string_view suffix, int index)
{
    return realtimeSkyPath("CloseCloudAmbient", suffix, index);
}

CloudParameters sampleCloudParameters(const CloudCurveLookup& curve)
{
    CloudParameters p;
    sampleFar(curve, "FarCloud", p.farFront);
    sampleFar(curve, "FarCloudBack", p.farBack);
    sampleClose(curve, p.close);
    return p;
}

void CloudMotion::advance(const CloudParameters& parameters, float seconds)
{
    scrollFarLayer(farFront, parameters.farFront, seconds);
    scrollFarLayer(farBack, parameters.farBack, seconds);
    closeFront = wrapUnit(closeFront + parameters.close.frontRevs * kPerHour * seconds);
    closeBack = wrapUnit(closeBack + parameters.close.backRevs * kPerHour * seconds);
}

float cloudMinSkyHeight(float fogSkyHeightFadeStart)
{
    // 0x824740F8.
    constexpr float kHighest = -1000.0F;
    constexpr double kRing = 15000.0;
    constexpr float kDome = 300.0F;
    const double angle = std::atan2(static_cast<double>(std::min(fogSkyHeightFadeStart, kHighest)), kRing);
    return static_cast<float>(std::sin(static_cast<float>(angle))) * kDome + 1.0F;
}

CloudDome<FarCloudVertex> buildFarCloudDome()
{
    // Constants of 0x82E35D08 (0x8203E6C4, 0x8224D5B4, 0x8224D5C0,
    // 0x8224D438, 0x8224D43C, 0x8224D2B8).
    constexpr float kRadius = 10000.0F;
    constexpr float kCentreBelow = 9700.0F;
    constexpr double kCutBelowHorizon = 0.10471967061360676;
    constexpr float kHalfPi = 1.5707950592041016F;
    constexpr float kTwoPi = 6.283180236816406F;
    constexpr float kTop = 300.0F;
    constexpr std::uint32_t kRings = 15;
    constexpr std::uint32_t kAround = 100;

    // The larger root, or the smaller when that is behind the camera.
    const auto dx = static_cast<float>(std::cos(kCutBelowHorizon));
    const auto dy = static_cast<float>(-std::sin(kCutBelowHorizon));
    const float a = dx * dx + dy * dy;
    const float b = 2.0F * dy * kCentreBelow;
    const float c = kCentreBelow * kCentreBelow - kRadius * kRadius;
    const float root = std::sqrt(b * b - 4.0F * a * c);
    float t = (root - b) / (2.0F * a);
    if (t < 0.0F) {
        t = (-b - root) / (2.0F * a);
    }
    // The angle at the sphere's centre between the camera and that point.
    const float spread
        = std::acos((kCentreBelow * kCentreBelow + kRadius * kRadius - t * t) / (2.0F * kCentreBelow * kRadius));
    const float lowest = kHalfPi - spread;
    const float span = kHalfPi - lowest;

    CloudDome<FarCloudVertex> dome;
    dome.vertices.reserve(kRings * kAround + 1);
    dome.indices.reserve((kRings - 1) * kAround * 6 + kAround * 3);
    for (std::uint32_t ring = 0; ring < kRings; ++ring) {
        const float along = static_cast<float>(ring) * (1.0F / 15.0F);
        const float latitude = lowest + span * along;
        const float weight = 1.0F - along;
        const std::uint32_t start = ring * kAround;
        for (std::uint32_t k = 0; k < kAround; ++k) {
            const float azimuth = static_cast<float>(k) * 0.01F * kTwoPi;
            FarCloudVertex v;
            v.position = glm::vec3(std::cos(azimuth) * std::cos(latitude) * kRadius,
                std::sin(latitude) * kRadius - kCentreBelow, std::sin(azimuth) * std::cos(latitude) * kRadius);
            v.texcoord = glm::vec3(std::sin(azimuth) * weight * 0.5F, std::cos(azimuth) * weight * 0.5F, weight);
            dome.vertices.push_back(v);
            if (ring + 1 < kRings) {
                const std::uint32_t here = start + k;
                const std::uint32_t next = start + (k + 1) % kAround;
                dome.indices.insert(
                    dome.indices.end(), {here, next, next + kAround, here, next + kAround, here + kAround});
            }
        }
    }
    const std::uint32_t lastRing = (kRings - 1) * kAround;
    const auto top = static_cast<std::uint32_t>(dome.vertices.size());
    dome.vertices.push_back({glm::vec3(0.0F, kTop, 0.0F), glm::vec3(0.0F)});
    for (std::uint32_t k = 1; k <= kAround; ++k) {
        dome.indices.insert(dome.indices.end(), {lastRing + k - 1, lastRing + k % kAround, top});
    }
    return dome;
}

CloudDome<CloseCloudVertex> buildCloseCloudDome()
{
    // Constants of 0x82E35830 (0x82006160, 0x8224D438, 0x82013CF8).
    constexpr float kInward = -300.0F;
    constexpr std::uint32_t kAround = 100;
    constexpr std::uint32_t kRow = kAround + 1;
    constexpr int kFirstRing = -1;
    constexpr int kLastRing = 14;
    constexpr float kStep = 1.0F / 15.0F;
    constexpr float kHalfPi = 1.5707950592041016F;
    constexpr float kTwoPi = 6.283180236816406F;

    CloudDome<CloseCloudVertex> dome;
    dome.vertices.reserve(static_cast<std::size_t>(kLastRing - kFirstRing + 1) * kRow + 1);
    std::uint32_t start = 0;
    for (int ring = kFirstRing; ring <= kLastRing; ++ring) {
        const float elevation = static_cast<float>(ring) * kStep * kHalfPi;
        const float height = static_cast<float>(ring + 1) * kStep;
        for (std::uint32_t k = 0; k < kAround; ++k) {
            const float u = static_cast<float>(k) * 0.01F;
            const float azimuth = u * kTwoPi;
            const glm::vec3 normal(-std::cos(azimuth) * std::cos(elevation), -std::sin(elevation),
                -std::sin(azimuth) * std::cos(elevation));
            dome.vertices.push_back({glm::vec4(normal * kInward, height), glm::vec4(normal, u)});
            if (ring < kLastRing) {
                const std::uint32_t here = start + k;
                dome.indices.insert(
                    dome.indices.end(), {here, here + 1, here + kRow + 1, here, here + kRow + 1, here + kRow});
            }
        }
        // The seam: the ring's first vertex again, at the far end of u.
        CloseCloudVertex seam = dome.vertices[start];
        seam.normal.w = 1.0F;
        dome.vertices.push_back(seam);
        start += kRow;
    }
    const std::uint32_t lastRing = start - kRow;
    const auto top = static_cast<std::uint32_t>(dome.vertices.size());
    dome.vertices.push_back({glm::vec4(-0.0F, 300.0F, -0.0F, 1.0F), glm::vec4(0.0F, -1.0F, 0.0F, 0.5F)});
    for (std::uint32_t k = 0; k < kAround; ++k) {
        dome.indices.insert(dome.indices.end(), {lastRing + k, lastRing + k + 1, top});
    }
    return dome;
}

} // namespace openhorizon::render
