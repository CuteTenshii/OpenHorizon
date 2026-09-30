#include "ParticleEmitters.h"

#include "ParticleSystem.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <cmath>
#include <format>
#include <optional>

namespace openhorizon::particles {
namespace {

/// The threshold and replacement of 0x82dc5114 (0x82249240, 0x82063cf0).
constexpr float kDegenerateAxis = 1e-5F;

float valueOf(const pugi::xml_node& parent, const char* child)
{
    return parent.child(child).attribute("value").as_float(0.0F);
}

std::string stringOf(const pugi::xml_node& parent, const char* child)
{
    return parent.child(child).attribute("value").as_string();
}

glm::vec3 vectorOf(const pugi::xml_node& node)
{
    return {node.attribute("v.x").as_float(0.0F), node.attribute("v.y").as_float(0.0F),
        node.attribute("v.z").as_float(0.0F)};
}

glm::mat3 axesOf(const pugi::xml_node& parent)
{
    const pugi::xml_node orientation = parent.child("Orientation");
    if (!orientation) {
        return glm::mat3(1.0F);
    }
    return {vectorOf(orientation.child("XAxis")), vectorOf(orientation.child("YAxis")),
        vectorOf(orientation.child("ZAxis"))};
}

/// The file's position with z negated, as 0x82dc4fe8-0x82dc5020 stores it.
glm::vec3 positionOf(const pugi::xml_node& parent)
{
    glm::vec3 p = vectorOf(parent.child("Position"));
    p.z = -p.z;
    return p;
}

EmitterShape shapeOf(std::string_view type)
{
    if (iequals(type, "plane")) {
        return EmitterShape::Plane;
    }
    if (iequals(type, "box")) {
        return EmitterShape::Box;
    }
    return EmitterShape::SuperSpray;
}

PlacedEmitter readEmitter(const pugi::xml_node& node, bool simple)
{
    PlacedEmitter e;
    if (simple) {
        e.gameplayId = stringOf(node, "GameplayID");
    }
    e.name = stringOf(node, "Name");
    e.effect = stringOf(node, "Effect");
    e.type = shapeOf(stringOf(node, "Type"));
    e.position = positionOf(node);
    e.axes = axesOf(node);
    e.angles = placementAngles(e.axes, e.type);
    switch (e.type) {
    case EmitterShape::SuperSpray: {
        const pugi::xml_node spray = node.child("Spray");
        e.spray = {spray.attribute("offAxisSpread").as_float(0.0F), spray.attribute("planeSpread").as_float(0.0F)};
        break;
    }
    case EmitterShape::Plane:
        e.width = valueOf(node, "Width");
        e.length = valueOf(node, "Length");
        break;
    case EmitterShape::Box:
        e.size = vectorOf(node.child("Size"));
        break;
    }
    return e;
}

EmitterTriggerZone readZone(const pugi::xml_node& node)
{
    EmitterTriggerZone z;
    z.name = stringOf(node, "Name");
    z.type = stringOf(node, "Type");
    z.position = positionOf(node);
    z.axes = axesOf(node);
    z.radius = valueOf(node, "Radius");
    z.size = vectorOf(node.child("Size"));
    z.retriggerDelay = valueOf(node, "RetriggerDelay");
    for (const pugi::xml_node emitter : node.children("Emitter")) {
        z.emitters.emplace_back(emitter.attribute("name").as_string());
    }
    return z;
}

} // namespace

glm::vec3 placementAngles(const glm::mat3& axes, EmitterShape type)
{
    glm::vec3 z = axes[2];
    if (type == EmitterShape::SuperSpray && std::fabs(z.x) < kDegenerateAxis && std::fabs(z.z) < kDegenerateAxis) {
        z.z = kDegenerateAxis;
    }
    const float yaw = std::atan2(z.x, z.z);
    const float pitch = std::atan2(-z.y, std::sqrt(z.x * z.x + z.z * z.z));
    // Undo the yaw, then the pitch, of the X axis: what is left is the roll
    // about Z, rollPitchYaw's first rotation.
    const glm::vec3 x
        = transformRow(transformRow(axes[0], glm::transpose(rotationY(yaw))), glm::transpose(rotationX(pitch)));
    const float roll = std::atan2(x.y, x.x);
    return {pitch, yaw, roll};
}

std::string ParticleEmitterSet::pathOf(std::string_view track, int ribbon)
{
    return std::format("media/tracks/{}/Ribbon_{:02}/ParticleEmitters.xml", track, ribbon);
}

ParticleEmitterSet ParticleEmitterSet::load(const Vfs& vfs, std::string_view track, int ribbon)
{
    const std::string path = pathOf(track, ribbon);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        return {};
    }
    return parse(*text, path);
}

ParticleEmitterSet ParticleEmitterSet::parse(std::string_view xml, std::string_view file)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    const pugi::xml_node root = doc.first_child();
    if (!result || !root || !iequals(root.name(), "ParticleEmitters")) {
        throw DataError("{}: not a ParticleEmitters file", file);
    }
    ParticleEmitterSet set;
    for (const pugi::xml_node node : root.children()) {
        const std::string_view name = node.name();
        if (iequals(name, "SimpleEmitter")) {
            set.m_simple.push_back(readEmitter(node, true));
        } else if (iequals(name, "TriggerableEmitter")) {
            set.m_triggerable.push_back(readEmitter(node, false));
        } else if (iequals(name, "TriggerZone")) {
            set.m_zones.push_back(readZone(node));
        }
    }
    return set;
}

const PlacedEmitter* ParticleEmitterSet::findSimple(std::string_view gameplayId) const
{
    for (const PlacedEmitter& e : m_simple) {
        if (e.gameplayId == gameplayId) {
            return &e;
        }
    }
    return nullptr;
}

std::map<std::string, std::size_t> ParticleEmitterSet::countByEffect() const
{
    std::map<std::string, std::size_t> counts;
    for (const PlacedEmitter& e : m_simple) {
        ++counts[e.effect];
    }
    for (const PlacedEmitter& e : m_triggerable) {
        ++counts[e.effect];
    }
    return counts;
}

} // namespace openhorizon::particles
