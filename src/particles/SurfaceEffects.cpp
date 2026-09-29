#include "SurfaceEffects.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

namespace fh::particles {
namespace {

constexpr std::string_view kSurfaceTypesPath = "media/physics/surfaceTypes.xml";

constexpr std::array<std::string_view, kTyreChannelCount> kChannelNames
    = {"Smoke", "Dust", "Grass", "Dirt", "Leaf1", "Leaf2", "Leaf3", "Leaf4", "Leaf5", "Leaf6", "GravelBits", "Litter",
        "DustClose", "DustBumper", "SmokeRim0", "SmokeRim1", "SmokeRim2", "SmokeRim3", "Dirt2", "Dust2", "DustClose2",
        "DustBumper2", "Dust3", "DustClose3", "DustBumper3"};

} // namespace

std::string_view tyreChannelName(TyreChannel channel)
{
    return kChannelNames[static_cast<std::size_t>(channel)];
}

SurfaceEffectTable SurfaceEffectTable::parse(std::string_view xml)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result result = doc.load_buffer(xml.data(), xml.size());
    const pugi::xml_node root = doc.child("Root");
    const pugi::xml_node list = root.child("SurfaceTypesList");
    if (!result || !list) {
        throw DataError("{}: no SurfaceTypesList", kSurfaceTypesPath);
    }
    const pugi::xml_node types = root.child("SurfaceTypes");

    SurfaceEffectTable table;
    for (const pugi::xml_node& entry : list.children("SurfaceType")) {
        SurfaceEffects s;
        s.surfaceName = entry.attribute("value").as_string();
        const pugi::xml_node element = types.child(s.surfaceName.c_str());
        // The smashable entries keep SmokeType directly under the element,
        // without SkidData; the loader only reads SkidData (0x82F16830), so
        // those weights are not the tyre table's and stay zero.
        const pugi::xml_node skid = element.child("SkidData");
        if (skid) {
            s.hasSkidData = true;
            s.minIntensity = skid.child("MinIntensity").attribute("value").as_float(0.0F);
            const pugi::xml_node smoke = skid.child("SmokeType");
            for (std::size_t i = 0; i < kTyreChannelCount; ++i) {
                s.weights[i] = smoke.attribute(std::string(kChannelNames[i]).c_str()).as_float(0.0F);
            }
        }
        table.m_surfaces.push_back(std::move(s));
    }
    return table;
}

SurfaceEffectTable SurfaceEffectTable::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kSurfaceTypesPath);
    if (!text) {
        throw DataError("{} is missing", kSurfaceTypesPath);
    }
    return parse(*text);
}

std::optional<std::size_t> SurfaceEffectTable::idOf(std::string_view name) const
{
    for (std::size_t i = 0; i < m_surfaces.size(); ++i) {
        if (m_surfaces[i].surfaceName == name) {
            return i;
        }
    }
    return std::nullopt;
}

} // namespace fh::particles
