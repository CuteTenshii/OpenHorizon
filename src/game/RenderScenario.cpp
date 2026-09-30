#include "RenderScenario.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <format>
#include <string>

namespace openhorizon {

RenderScenario RenderScenario::parse(std::string_view xml)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(xml.data(), xml.size());
    if (!parsed) {
        throw DataError("render scenario: {}", parsed.description());
    }
    const pugi::xml_node root = doc.child("DynamicRenderSettings");
    if (!root) {
        throw DataError("render scenario is not a DynamicRenderSettings document");
    }
    RenderScenario scenario;
    if (const pugi::xml_attribute value = root.child("EnvMapFrequencyScale").attribute("value")) {
        scenario.envMapFrequencyScale = value.as_float();
    }
    return scenario;
}

RenderScenario RenderScenario::load(const Vfs& vfs, std::string_view name)
{
    const std::string path = std::format("media/renderscenarios/{}.xml", name);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} is missing", path);
    }
    return parse(*text);
}

RenderScenario RenderScenario::stacked(const Vfs& vfs, std::string_view top)
{
    RenderScenario scenario = load(vfs, top);
    if (!scenario.envMapFrequencyScale) {
        scenario.envMapFrequencyScale = load(vfs, kGlobal).envMapFrequencyScale;
    }
    return scenario;
}

} // namespace openhorizon
