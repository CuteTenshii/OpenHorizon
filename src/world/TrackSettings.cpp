#include "TrackSettings.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <format>
#include <optional>
#include <string>

namespace fh::world {

namespace {

float attribute(const pugi::xml_node& root, const char* element, const char* name)
{
    const pugi::xml_attribute value = root.child(element).attribute(name);
    if (!value) {
        throw DataError("TrackSettings.xml has no {} {}", element, name);
    }
    return value.as_float();
}

glm::vec4 colour(const pugi::xml_node& root, const char* element)
{
    return {attribute(root, element, "r"), attribute(root, element, "g"), attribute(root, element, "b"),
        attribute(root, element, "a")};
}

} // namespace

TrackSettings TrackSettings::parse(std::string_view xml)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        throw DataError("TrackSettings.xml: {}", parsed.description());
    }
    const pugi::xml_node root = doc.child("TrackSettings");
    if (!root) {
        throw DataError("TrackSettings.xml is not a TrackSettings document");
    }
    TrackSettings s;
    s.skyScale = {attribute(root, "skyScale", "value"), attribute(root, "cubeSkySaturation", "value"),
        attribute(root, "cubeSkyScale", "value"), attribute(root, "cubeReflGroundScale", "value")};
    s.roadSpecular2 = {attribute(root, "RoadSpecular", "RoadGlossiness"),
        attribute(root, "RoadSpecular", "RoadGlossiness2"), attribute(root, "RoadSpecular", "RoadLightmapDarkPoint"),
        attribute(root, "RoadSpecular", "RoadLightmapLightPoint")};
    s.darkColor = colour(root, "DarkColor");
    s.lightColor = colour(root, "LightColor");
    s.trackLightMap = {attribute(root, "TrackLightMapParams", "DarkPoint"),
        attribute(root, "TrackLightMapParams", "LightPoint"), 0.0F, 0.0F};
    s.dynamicCubemap = attribute(root, "Performance", "DynamicCubemap") != 0.0F;
    return s;
}

TrackSettings TrackSettings::load(const Vfs& vfs, std::string_view track)
{
    const std::string path = std::format("media/tracks/{}/TrackSettings.xml", track);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} is missing", path);
    }
    return parse(*text);
}

} // namespace fh::world
