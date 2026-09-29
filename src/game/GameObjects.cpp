#include "GameObjects.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <cmath>
#include <format>
#include <utility>

namespace fh {

namespace {

glm::vec3 readVector(const pugi::xml_node& node)
{
    return {node.attribute("x").as_float(), node.attribute("y").as_float(), node.attribute("z").as_float()};
}

} // namespace

float GameObject::heading() const
{
    const glm::vec3 forward = axes[2];
    return std::atan2(forward.x, forward.z);
}

GameObjects GameObjects::load(const Vfs& vfs, std::string_view track)
{
    const std::string path = std::format("media/tracks/{}/Ribbon_00/GameObjs.xml", track);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} not found", path);
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text->data(), text->size());
    if (!parsed) {
        throw DataError("{}: {}", path, parsed.description());
    }
    GameObjects objects;
    // Each object is its own element (Obj0, Obj1, ...), so walk the children
    // rather than select by name.
    for (const pugi::xml_node node : doc.child("GameObjs").children()) {
        GameObject object;
        object.id = node.attribute("GameplayID").as_string();
        object.position = readVector(node.child("Pos"));
        const pugi::xml_node orientation = node.child("Orientation");
        if (orientation) {
            object.axes[0] = readVector(orientation.child("XAxis"));
            object.axes[1] = readVector(orientation.child("YAxis"));
            object.axes[2] = readVector(orientation.child("ZAxis"));
        }
        objects.m_objects.push_back(std::move(object));
    }
    return objects;
}

GameObjects GameObjects::fromObjects(std::vector<GameObject> objects)
{
    GameObjects result;
    result.m_objects = std::move(objects);
    return result;
}

const GameObject* GameObjects::find(std::string_view id) const
{
    for (const GameObject& object : m_objects) {
        if (iequals(object.id, id)) {
            return &object;
        }
    }
    return nullptr;
}

} // namespace fh
