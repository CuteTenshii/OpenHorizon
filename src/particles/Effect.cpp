#include "Effect.h"

#include "base/Error.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

namespace fh::particles {
namespace {

/// 0x82000D44, the factor the loader multiplies degree fields by.
constexpr float kDegreesToRadians = std::numbers::pi_v<float> / 180.0F;

float floatAttr(const pugi::xml_node& node, const char* name, float fallback)
{
    const pugi::xml_attribute a = node.attribute(name);
    return a ? a.as_float(fallback) : fallback;
}

int intAttr(const pugi::xml_node& node, const char* name, int fallback)
{
    const pugi::xml_attribute a = node.attribute(name);
    return a ? a.as_int(fallback) : fallback;
}

bool boolAttr(const pugi::xml_node& node, const char* name, bool fallback)
{
    const pugi::xml_attribute a = node.attribute(name);
    return a ? a.as_bool(fallback) : fallback;
}

/// The loader's value/variation pair reader (0x82DC1C38).
Varied varied(const pugi::xml_node& node, Varied fallback = {})
{
    if (!node) {
        return fallback;
    }
    return {floatAttr(node, "value", fallback.value), floatAttr(node, "variation", fallback.variation)};
}

/// The four-float reader of light_influence and friends (0x82DC1B98).
glm::vec4 vec4Node(const pugi::xml_node& node, glm::vec4 fallback)
{
    if (!node) {
        return fallback;
    }
    return {floatAttr(node, "v.x", fallback.x), floatAttr(node, "v.y", fallback.y), floatAttr(node, "v.z", fallback.z),
        floatAttr(node, "v.w", fallback.w)};
}

glm::vec4 colourNode(const pugi::xml_node& node)
{
    return {
        floatAttr(node, "r", 0.0F), floatAttr(node, "g", 0.0F), floatAttr(node, "b", 0.0F), floatAttr(node, "a", 0.0F)};
}

std::string lower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void parseBehaviours(const pugi::xml_node& behaviour, EffectDefinition& effect)
{
    const pugi::xml_node inherit = behaviour.child("MotionInheritance");
    effect.motionInheritance.influence = intAttr(inherit, "influence", 0);
    effect.motionInheritance.multiplier = floatAttr(inherit, "multiplier", 0.0F);
    effect.motionInheritance.offset = floatAttr(inherit, "offset", 0.0F);
    effect.motionInheritance.variation = floatAttr(inherit, "variation", 0.0F);

    // The loader walks Behavior's children and builds a behaviour for
    // each of these names (0x82DCB684), keeping file order.
    for (const pugi::xml_node& child : behaviour.children()) {
        const std::string_view tag = child.name();
        Behaviour b;
        if (tag == "Gravity") {
            b.kind = Behaviour::Kind::Gravity;
            b.gravity.strength = floatAttr(child, "strength", 10.0F);
            b.gravity.decay = floatAttr(child, "decay", 0.0F);
        } else if (tag == "Friction") {
            b.kind = Behaviour::Kind::Friction;
            b.friction.strength = floatAttr(child, "strength", 0.0F);
            b.friction.startFrom = floatAttr(child, "startFrom", 0.0F);
            b.friction.velocityKept = floatAttr(child, "velocityKept", 0.0F);
        } else if (tag == "Wind") {
            b.kind = Behaviour::Kind::Wind;
            b.wind.direction = {floatAttr(child, "direction.x", 0.0F), floatAttr(child, "direction.y", 0.0F),
                floatAttr(child, "direction.z", 0.0F), floatAttr(child, "direction.w", 0.0F)};
            b.wind.strength = floatAttr(child, "strength", 0.0F);
            b.wind.decay = floatAttr(child, "decay", 0.0F);
            b.wind.turbulence = floatAttr(child, "turbulence", 0.0F);
            b.wind.frequency = floatAttr(child, "frequency", 0.0F);
            b.wind.scale = floatAttr(child, "scale", 0.0F);
        } else if (tag == "BubbleMotion") {
            b.kind = Behaviour::Kind::BubbleMotion;
            Varied* slots[3] = {&b.bubble.amplitude, &b.bubble.period, &b.bubble.phase};
            int index = 0;
            for (const pugi::xml_node& amp : child.children("Amplitude")) {
                if (index < 3) {
                    *slots[index++] = varied(amp);
                }
            }
        } else if (tag == "WheelAttractor") {
            b.kind = Behaviour::Kind::WheelAttractor;
            b.wheelAttractor.sideDriftScale = floatAttr(child, "SideDriftScale", 0.0F);
            b.wheelAttractor.backDriftScale = floatAttr(child, "BackDriftScale", 0.0F);
            b.wheelAttractor.downDriftScale = floatAttr(child, "DownDriftScale", 0.0F);
            b.wheelAttractor.velInhMinDist = floatAttr(child, "VelInhMinDist", 0.0F);
            b.wheelAttractor.velInhDistScale = floatAttr(child, "VelInhDistScale", 0.0F);
            b.wheelAttractor.velInhMin = floatAttr(child, "VelInhMin", 0.0F);
        } else if (tag == "Collision") {
            b.kind = Behaviour::Kind::Collision;
            b.collision.plane = {floatAttr(child, "PlaneX", 0.0F), floatAttr(child, "PlaneY", 1.0F),
                floatAttr(child, "PlaneZ", 0.0F), floatAttr(child, "PlaneD", 0.0F)};
            b.collision.bounce = varied(child.child("Bounce"));
        } else {
            continue;
        }
        effect.behaviours.push_back(b);
    }

    const pugi::xml_node spawn = behaviour.child("ParticleSpawn");
    const std::string_view spawnType = spawn.attribute("type").as_string("None");
    if (spawnType == "Collision") {
        effect.spawnType = SpawnType::Collision;
    } else if (spawnType == "Death") {
        effect.spawnType = SpawnType::Death;
    } else if (spawnType == "Trails") {
        effect.spawnType = SpawnType::Trails;
    }
    effect.spawnEffect = spawn.attribute("effect").as_string();
}

void parseRender(const pugi::xml_node& renderNode, int version, EffectDefinition& effect)
{
    const pugi::xml_node type = renderNode.child("ParticleType");
    RenderSettings& r = effect.render;
    const std::string_view value = type.attribute("value").as_string();
    if (value == "Quad") {
        r.type = ParticleType::Quad;
    } else if (value == "BillboardedQuad") {
        r.type = ParticleType::BillboardedQuad;
    } else {
        r.type = ParticleType::Modeled;
    }
    r.dynamicLighting = boolAttr(type, "dynamic_lighting", false);
    r.zSort = boolAttr(type, "zsort", false);
    r.maxScreenSize = floatAttr(type, "max_screen_size", r.maxScreenSize);
    if (version >= 10) {
        r.forceEdgeOnFadeOff = boolAttr(type, "forceEdgeOnFadeOff", false);
    }
    if (version >= 14) {
        r.facingMatchesVelocity = boolAttr(type, "facingMatchesVelocity", false);
    }
    if (r.dynamicLighting) {
        if (version >= 8) {
            r.lightInfluence = vec4Node(type.child("light_influence"), r.lightInfluence);
        } else {
            // Older files have one number, which lands in the w component
            // (0x82DC7CAC).
            r.lightInfluence.w = floatAttr(type, "light_influence", r.lightInfluence.w);
        }
        if (version >= 9) {
            r.headLightPower = floatAttr(type.child("HeadLightPower"), "value", r.headLightPower);
        }
        if (version >= 13) {
            r.specularLightInfluence = vec4Node(type.child("specular_light_influence"), r.specularLightInfluence);
        }
    } else {
        // Unlit particles get no lighting blend at all (0x82DC7D48).
        r.lightInfluence.w = 0.0F;
    }
    r.texture = type.child("Texture").attribute("value").as_string();
    if (version >= 4) {
        r.texture2 = type.child("Texture2").attribute("value").as_string();
    }

    const pugi::xml_node material = type.child("Material");
    Material& m = r.material;
    m.blend = std::string_view(material.child("BlendType").attribute("value").as_string()) == "additive"
        ? BlendType::Additive
        : BlendType::Alpha;
    const glm::vec4 start = colourNode(material.child("RGBStart"));
    const glm::vec4 end = colourNode(material.child("RGBEnd"));
    m.nearFade = floatAttr(material.child("NearFade"), "nearFade", 0.0F);
    glm::vec4 mid = (start + end) * 0.5F;
    if (version >= 7) {
        m.fadeInTo = floatAttr(material.child("FadeInTo"), "fadeInTo", m.fadeInTo);
        m.fadeOutFrom = floatAttr(material.child("FadeOutFrom"), "fadeOutFrom", m.fadeOutFrom);
        mid = colourNode(material.child("RGBMid"));
    }
    const auto squared = [](glm::vec4 c) { return glm::vec4(c.r * c.r, c.g * c.g, c.b * c.b, c.a); };
    m.rgbStart = squared(start);
    m.rgbMid = squared(mid);
    m.rgbEnd = squared(end);
}

} // namespace

std::string EffectDefinition::shaderName() const
{
    std::string shader = "particles";
    if (soft()) {
        shader += "_soft";
    }
    if (animated()) {
        shader += "_animated";
    }
    if (rowOnly()) {
        shader += "_rowonly";
    }
    if (gradient()) {
        shader += "_gradient";
    }
    if (shaderFlags == 0 || shaderFlags == shader::kSoft) {
        shader += "_common";
    }
    return shader;
}

EffectDefinition parseEffect(std::string_view xml, std::string fileName)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    const pugi::xml_node root = doc.child("Effect");
    if (!result || !root) {
        throw DataError("{}: not an effect file", fileName);
    }

    EffectDefinition e;
    e.name = std::move(fileName);
    e.version = intAttr(root, "Version", 0);
    e.budget = static_cast<std::uint32_t>(std::max(0, intAttr(root, "budget", 0)));
    e.lowerBudget
        = e.version >= 2 ? static_cast<std::uint32_t>(std::max(0, intAttr(root, "lowerbudget", 0))) : e.budget / 2;
    e.lowerBudget = std::min(e.lowerBudget, e.budget);
    if (e.version >= 11) {
        const pugi::xml_node tod = root.child("TimeOfDayData");
        e.timeOfDay[0] = intAttr(tod, "startH", 0);
        e.timeOfDay[1] = intAttr(tod, "startM", 0);
        e.timeOfDay[2] = intAttr(tod, "endH", 24);
        e.timeOfDay[3] = intAttr(tod, "endM", 0);
    }

    const pugi::xml_node props = root.child("EmitterProperties");
    const std::string_view shape = props.attribute("type").as_string("SuperSpray");
    const pugi::xml_node sizeNode = props.child("Size");
    if (shape == "Box") {
        e.shape = EmitterShape::Box;
        e.size.box = {floatAttr(sizeNode, "length", 0.0F), floatAttr(sizeNode, "height", 0.0F),
            floatAttr(sizeNode, "width", 0.0F)};
    } else if (shape == "Plane") {
        e.shape = EmitterShape::Plane;
        e.size.box = {floatAttr(sizeNode, "length", 0.0F), 0.0F, floatAttr(sizeNode, "width", 0.0F)};
    } else {
        e.shape = EmitterShape::SuperSpray;
        const Varied axis = varied(props.child("OffAxis"));
        const Varied plane = varied(props.child("OffPlane"));
        e.offAxis = {axis.value * kDegreesToRadians, axis.variation * kDegreesToRadians};
        e.offPlane = {plane.value * kDegreesToRadians, plane.variation * kDegreesToRadians};
    }

    const pugi::xml_node quantity = props.child("Quantity");
    e.quantity = {floatAttr(quantity, "value", 0.0F), floatAttr(quantity, "var", 0.0F)};
    e.velocity = varied(props.child("Velocity"), {0.5F, 0.0F});
    const pugi::xml_node emitTime = props.child("EmitTime");
    e.emitStart = floatAttr(emitTime, "start", 0.0F);
    e.emitEnd = floatAttr(emitTime, "end", 0.0F);
    e.emitInfinite = boolAttr(emitTime, "infinite", false);
    e.displayUntil = props.child("DisplayUntil").attribute("value").as_uint(0xFFFFFFFFU);
    e.life = varied(props.child("Life"));

    if (e.version >= 3) {
        e.density = floatAttr(props.child("Density"), "value", 0.0F);
    }
    if (e.version >= 4) {
        const std::string type = lower(props.child("ParticleShaderType").attribute("value").as_string());
        if (type.find("animated") != std::string::npos) {
            e.shaderFlags |= shader::kAnimated;
        }
        if (type.find("gradient") != std::string::npos) {
            e.shaderFlags |= shader::kGradient;
        }
        if (type.find("rowonly") != std::string::npos) {
            e.shaderFlags |= shader::kRowOnly;
        }
        if (type.find("soft") != std::string::npos) {
            e.shaderFlags |= shader::kSoft;
        } else {
            // Density only matters to soft particles; the loader clears it
            // for the others (0x82DCAB6C), which also switches the vertex
            // shader's depth output off.
            e.density = 0.0F;
        }
    }
    if (e.animated()) {
        const pugi::xml_node anim = props.child("Animation");
        e.animationRate = floatAttr(anim, "rate", 1.0F);
        e.framesX = std::max(1, intAttr(anim, "numFramesX", 1));
        e.framesY = std::max(1, intAttr(anim, "numFramesY", 1));
    }
    if (e.gradient()) {
        e.gradientRows = std::max(1, intAttr(props.child("Gradient"), "numGradientRows", 1));
    }
    if (e.version >= 5) {
        e.renderInMirror = boolAttr(props.child("RenderInMirror"), "value", false);
    }

    SizeCurve& s = e.size;
    s.value = {floatAttr(sizeNode, "valueX", 0.25F), floatAttr(sizeNode, "valueY", 0.25F)};
    s.variation = {floatAttr(sizeNode, "variationX", 0.0F), floatAttr(sizeNode, "variationY", 0.0F)};
    s.growFor = {floatAttr(sizeNode, "growforX", 0.0F), floatAttr(sizeNode, "growforY", 0.0F)};
    s.shrinkFor = {floatAttr(sizeNode, "shrinkforX", 0.0F), floatAttr(sizeNode, "shrinkforY", 0.0F)};
    s.maxValue = {floatAttr(sizeNode, "maxValueX", 0.25F), floatAttr(sizeNode, "maxValueY", 0.25F)};
    s.endValue = {floatAttr(sizeNode, "endValueX", 0.0F), floatAttr(sizeNode, "endValueY", 0.0F)};
    s.growFrom = floatAttr(sizeNode, "growFrom", 0.0F);
    s.shrinkTo = floatAttr(sizeNode, "shrinkTo", 1.0F);

    e.spinTime = varied(props.child("SpinTime"));
    const Varied phase = varied(props.child("SpinPhase"));
    e.spinPhase = {phase.value * kDegreesToRadians, phase.variation};

    const pugi::xml_node axis = props.child("SpinAxis");
    const std::string_view axisType = axis.attribute("type").as_string("Random");
    if (axisType == "DirectionOfTravel") {
        e.spinAxis = SpinAxis::DirectionOfTravel;
    } else if (axisType == "PerpToDirOfTravel") {
        e.spinAxis = SpinAxis::PerpToDirOfTravel;
    } else if (axisType == "UserDefined") {
        e.spinAxis = SpinAxis::UserDefined;
        e.spinAxisUser = {floatAttr(axis, "x", 0.0F), floatAttr(axis, "y", 0.0F), floatAttr(axis, "z", 0.0F)};
    } else {
        e.spinAxis = SpinAxis::Random;
    }
    e.spinAlternate = boolAttr(axis, "alternate", false);
    e.lockToCamera = boolAttr(props.child("LockTo"), "camera", false);
    e.surfaceEffectId = intAttr(props.child("SurfaceEffectId"), "type", 0);

    parseBehaviours(root.child("Behavior"), e);
    parseRender(root.child("Render"), e.version, e);
    return e;
}

CompositeDefinition parseComposite(std::string_view xml, std::string name)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    const pugi::xml_node root = doc.child("Composite");
    if (!result || !root) {
        throw DataError("{}: not a composite effect file", name);
    }
    CompositeDefinition c;
    c.name = std::move(name);
    c.repeatPeriod = floatAttr(root.child("RepeatPeriod"), "time", 0.0F);
    for (const pugi::xml_node& member : root.children("Effect")) {
        CompositeMember m;
        m.emitterName = member.child("EmitterName").attribute("name").as_string();
        m.fileName = member.child("FileName").attribute("name").as_string();
        m.positionOffset = vec4Node(member.child("PosOffset"), glm::vec4(0.0F));
        m.startOffset = floatAttr(member.child("StartOffset"), "time", 0.0F);
        m.finishOffset = floatAttr(member.child("FinishOffset"), "time", 0.0F);
        c.members.push_back(std::move(m));
    }
    return c;
}

std::vector<SystemEntry> parseSystemList(std::string_view xml)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    const pugi::xml_node root = doc.child("Track");
    if (!result || !root) {
        throw DataError("{}: not an effect list", kDefaultTrackPath);
    }
    std::vector<SystemEntry> out;
    for (const pugi::xml_node& entry : root.children("SystemEffect")) {
        out.push_back({entry.attribute("file").as_string(), entry.attribute("system_name").as_string()});
    }
    return out;
}

std::string effectPath(std::string_view file)
{
    return std::string(kEffectsFolder) + "/" + lower(file) + ".xml";
}

bool isCompositeName(std::string_view file)
{
    return lower(file).starts_with("_composite_");
}

} // namespace fh::particles
