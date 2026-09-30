#include "SpectatorConfig.h"

#include "base/Bytes.h"

#include <pugixml.hpp>

namespace openhorizon::spectators {

namespace {

template <typename Item> int findByName(const std::vector<Item>& items, std::string_view name)
{
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (iequals(items[i].name, name)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool parseBool(const pugi::xml_attribute& attribute, bool fallback)
{
    if (!attribute) {
        return fallback;
    }
    const std::string_view value = attribute.as_string();
    return iequals(value, "true") || value == "1";
}

std::vector<SpectatorConfig::WeightedAnimation> weightedChildren(const pugi::xml_node& node, const char* tag)
{
    std::vector<SpectatorConfig::WeightedAnimation> out;
    for (const pugi::xml_node child : node.children(tag)) {
        out.push_back({child.attribute("name").as_string(), child.attribute("weight").as_float(1.0F)});
    }
    return out;
}

} // namespace

SpectatorConfig SpectatorConfig::parse(std::string_view text, const std::string& source)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text.data(), text.size());
    if (!parsed) {
        throw DataError("{}: {}", source, parsed.description());
    }
    const pugi::xml_node root = doc.child("spectators");
    if (!root) {
        throw DataError("{}: no <spectators> element", source);
    }

    SpectatorConfig config;
    const pugi::xml_node models = root.child("models");
    config.version = models.attribute("Version").as_int();
    config.spriteAngles = models.attribute("numSpriteAngles").as_int();
    config.spritesBothSides = parseBool(models.attribute("bSpritesBothSides"), false);
    for (const pugi::xml_node model : models.children("model")) {
        config.models.push_back({model.attribute("name").as_string(),
            model.child("texture").attribute("name").as_string(), model.attribute("spritesPerTex").as_int()});
    }
    for (const pugi::xml_node set : root.children("modelset")) {
        ModelSet modelSet {set.attribute("name").as_string(), {}};
        for (const pugi::xml_node model : set.children("model")) {
            modelSet.models.emplace_back(model.attribute("name").as_string());
        }
        config.modelSets.push_back(std::move(modelSet));
    }
    for (const pugi::xml_node node : root.children("skeleton")) {
        Skeleton skeleton;
        skeleton.name = node.attribute("name").as_string();
        skeleton.suffix = node.attribute("suffix").as_string();
        skeleton.subdir = node.attribute("subdir").as_string();
        skeleton.modelSet = node.child("modelset").attribute("name").as_string();
        for (const pugi::xml_node animation : node.children("animation")) {
            skeleton.animations.push_back(
                {animation.attribute("name").as_string(), animation.attribute("filename").as_string()});
        }
        config.skeletons.push_back(std::move(skeleton));
    }
    for (const pugi::xml_node node : root.children("crowdclass")) {
        CrowdClass crowdClass;
        crowdClass.id = node.attribute("id").as_int();
        crowdClass.name = node.attribute("name").as_string();
        crowdClass.skeleton = node.child("skeleton").attribute("name").as_string();
        crowdClass.modelSet = node.child("modelset").attribute("name").as_string();
        crowdClass.idles = weightedChildren(node, "idle");
        crowdClass.cheers = weightedChildren(node, "cheer");
        if (const pugi::xml_attribute randomSize = node.attribute("randomsize")) {
            crowdClass.randomSize = parseBool(randomSize, true);
        }
        crowdClass.offset = {node.attribute("offsetX").as_float(), node.attribute("offsetY").as_float(),
            node.attribute("offsetZ").as_float()};
        crowdClass.billboardOffsetY = node.attribute("BBoffsetY").as_float();
        config.classes.push_back(std::move(crowdClass));
    }
    return config;
}

int SpectatorConfig::findModel(std::string_view name) const
{
    return findByName(models, name);
}

int SpectatorConfig::findModelSet(std::string_view name) const
{
    return findByName(modelSets, name);
}

int SpectatorConfig::findSkeleton(std::string_view name) const
{
    return findByName(skeletons, name);
}

} // namespace openhorizon::spectators
