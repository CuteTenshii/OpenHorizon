#include "SurfaceTable.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cstdlib>

namespace fh::collision {

namespace {

/// The `value` attribute of the element at `path` below `node`, if present
/// and numeric.
std::optional<float> valueAt(const pugi::xml_node& node, const char* path)
{
    const pugi::xml_node element = node.first_element_by_path(path);
    if (!element) {
        return std::nullopt;
    }
    const pugi::xml_attribute value = element.attribute("value");
    if (!value) {
        return std::nullopt;
    }
    const char* text = value.value();
    char* end = nullptr;
    const float number = std::strtof(text, &end);
    if (end == text) {
        return std::nullopt;
    }
    return number;
}

void readFloat(const pugi::xml_node& node, const char* path, float& into)
{
    if (const std::optional<float> v = valueAt(node, path)) {
        into = *v;
    }
}

void readBool(const pugi::xml_node& node, const char* path, bool& into)
{
    const pugi::xml_node element = node.first_element_by_path(path);
    if (element && element.attribute("value")) {
        into = iequals(element.attribute("value").value(), "true");
    }
}

std::uint8_t colourChannel(const pugi::xml_node& colour, const char* name, std::uint8_t fallback)
{
    const std::optional<float> v = valueAt(colour, name);
    return v ? static_cast<std::uint8_t>(std::clamp(*v, 0.0F, 255.0F)) : fallback;
}

void readParameters(const pugi::xml_node& element, SurfaceType& surface)
{
    const pugi::xml_node friction = element.child("Friction");
    surface.hasParameters = static_cast<bool>(friction);
    readFloat(element, "Friction/FrictionScale", surface.frictionScale);
    readFloat(element, "Friction/HeatTireScale", surface.heatTireScale);
    readFloat(element, "Friction/OffRoadness", surface.offRoadness);
    readFloat(element, "Friction/OffRoadWetFrictionScale", surface.offRoadWetFrictionScale);
    readFloat(element, "Friction/OffRoadDryPeakSA", surface.offRoadDryPeakSlipAngle);
    readFloat(element, "Friction/OffRoadWetPeakSA", surface.offRoadWetPeakSlipAngle);
    readFloat(element, "Friction/RearGripMultiplier", surface.rearGripMultiplier);
    readFloat(element, "Friction/MinimumArcadeGripValue", surface.minimumArcadeGrip);
    readFloat(element, "Friction/HandbrakeGripMultiplier", surface.handbrakeGripMultiplier);
    readFloat(element, "Friction/Offroad/ChaseCamSpringMultiplier", surface.chaseCamSpringMultiplier);
    readFloat(element, "Friction/VelDepFriction/VelPeak", surface.velocityFriction.velPeak);
    readFloat(element, "Friction/VelDepFriction/LateralCoeff", surface.velocityFriction.lateralCoeff);
    readFloat(element, "Friction/VelDepFriction/LongCoeff", surface.velocityFriction.longitudinalCoeff);
    readFloat(element, "Aerodynamics/LinearDrag", surface.linearDrag);
    readFloat(element, "Aerodynamics/AngularDrag", surface.angularDrag);

    const pugi::xml_node bumpiness = element.child("Bumpiness");
    if (const pugi::xml_node noise = bumpiness.child("NoiseType"); noise && noise.attribute("value")) {
        surface.bumpiness.noiseType = noise.attribute("value").value();
    }
    readFloat(bumpiness, "Frequency", surface.bumpiness.frequency);
    readFloat(bumpiness, "ClampMin", surface.bumpiness.clampMin);
    readFloat(bumpiness, "ClampMax", surface.bumpiness.clampMax);
    readFloat(bumpiness, "Scale", surface.bumpiness.scale);
    readFloat(bumpiness, "Offset", surface.bumpiness.offset);
    readFloat(bumpiness, "Rumble", surface.bumpiness.rumble);

    readBool(element, "Springiness/IsSpringy", surface.springiness.springy);
    readFloat(element, "Springiness/SpringK", surface.springiness.springK);
    readFloat(element, "Springiness/SpringCompD", surface.springiness.compressionDamping);
    readFloat(element, "Springiness/SpringRebD", surface.springiness.reboundDamping);

    readFloat(element, "CarDamageScalar", surface.carDamageScalar);
    readBool(element, "IsPenaltySurface", surface.penalty);
    readBool(element, "ShouldSpark", surface.sparks);

    if (const pugi::xml_node colour = element.child("DebugColor")) {
        surface.debugColour = {colourChannel(colour, "ColorRed", 128), colourChannel(colour, "ColorGreen", 128),
            colourChannel(colour, "ColorBlue", 128), colourChannel(colour, "ColorAlpha", 255)};
    }
}

} // namespace

SurfaceTable SurfaceTable::parse(std::string_view xml, const std::string& source)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        throw DataError("{}: not XML: {}", source, parsed.description());
    }
    const pugi::xml_node root = doc.child("Root");
    const pugi::xml_node list = root.child("SurfaceTypesList");
    if (!list) {
        throw DataError("{}: no <SurfaceTypesList>", source);
    }

    SurfaceTable table;
    for (const pugi::xml_node category : root.child("SurfaceCategories").children()) {
        if (category.type() == pugi::node_element) {
            table.m_categories.emplace_back(category.name());
        }
    }
    for (const pugi::xml_node entry : list.children("SurfaceType")) {
        SurfaceType surface;
        surface.name = entry.attribute("value").value();
        table.m_surfaces.push_back(std::move(surface));
    }

    const pugi::xml_node types = root.child("SurfaceTypes");
    for (SurfaceType& surface : table.m_surfaces) {
        const pugi::xml_node element = types.child(surface.name.c_str());
        if (!element) {
            continue;
        }
        const std::string_view category = element.attribute("Category").value();
        const auto found = std::find(table.m_categories.begin(), table.m_categories.end(), category);
        if (found != table.m_categories.end()) {
            surface.category = static_cast<std::uint8_t>(found - table.m_categories.begin());
        }
        readParameters(element, surface);
    }

    for (const pugi::xml_node node : root.child("CategoryToCategoryParams").children("Node")) {
        CategoryPair pair;
        pair.category0 = node.attribute("Category0").value();
        pair.category1 = node.attribute("Category1").value();
        pair.friction = node.attribute("FrictionCoefficient").as_float(pair.friction);
        pair.elasticity = node.attribute("Elasticity").as_float(pair.elasticity);
        if (const pugi::xml_attribute low = node.attribute("LowSpeedFrictionCoefficient")) {
            pair.lowSpeedFriction = low.as_float();
        }
        table.m_pairs.push_back(std::move(pair));
    }
    return table;
}

SurfaceTable SurfaceTable::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kSurfaceTypesPath);
    if (!text) {
        throw DataError("{}: not found", kSurfaceTypesPath);
    }
    return parse(*text);
}

std::string SurfaceTable::nameOf(std::size_t id) const
{
    if (const SurfaceType* surface = find(id)) {
        return surface->name;
    }
    return std::format("surface{}", id);
}

std::string_view SurfaceTable::categoryOf(std::size_t id) const
{
    const SurfaceType* surface = find(id);
    if (surface == nullptr || surface->category >= m_categories.size()) {
        return {};
    }
    return m_categories[surface->category];
}

std::optional<std::size_t> SurfaceTable::idOf(std::string_view name) const
{
    for (std::size_t i = 0; i < m_surfaces.size(); ++i) {
        if (iequals(m_surfaces[i].name, name)) {
            return i;
        }
    }
    return std::nullopt;
}

const CategoryPair* SurfaceTable::pair(std::string_view category0, std::string_view category1) const
{
    const CategoryPair* fallback = nullptr;
    for (const CategoryPair& p : m_pairs) {
        if ((p.category0 == category0 && p.category1 == category1)
            || (p.category0 == category1 && p.category1 == category0)) {
            return &p;
        }
        if (p.category0 == "Default" && p.category1 == "Default") {
            fallback = &p;
        }
    }
    return fallback;
}

} // namespace fh::collision
