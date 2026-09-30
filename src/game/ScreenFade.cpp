#include "ScreenFade.h"

#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <numbers>
#include <string>

namespace fh {

std::optional<FadeOptions> loadStateFade(const Vfs& vfs, std::string_view stateId)
{
    const std::optional<std::string> text = vfs.readText("media/gamemodes/state_definitions.xml");
    pugi::xml_document doc;
    if (!text || !doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    const std::string query = std::format("//State[@id='{}']/Behaviour[@id='CFadeIn']/FadeOptions", stateId);
    const pugi::xpath_node node = doc.select_node(query.c_str());
    if (!node) {
        return std::nullopt;
    }
    const pugi::xml_node options = node.node();
    FadeOptions fade;
    fade.duration = options.attribute("Duration").as_float();
    fade.hold = options.attribute("HoldDuration").as_float();
    fade.sCurve = std::string_view(options.attribute("CurveType").as_string()) == "S";
    fade.colour = glm::vec3(options.attribute("Color.x").as_float(), options.attribute("Color.y").as_float(),
        options.attribute("Color.z").as_float());
    return fade;
}

float fadeInOpacity(const FadeOptions& fade, float elapsed)
{
    const float t = std::min(elapsed, fade.duration);
    if (t >= fade.duration) {
        return 0.0F;
    }
    const float from = std::min(fade.duration, fade.hold);
    const float to = std::max(fade.duration, fade.hold);
    float x = 0.0F;
    if (t <= from) {
        x = 1.0F;
    } else if (t < to) {
        x = 1.0F - (t - from) / (to - from);
    }
    x = std::clamp(x, 0.0F, 1.0F);
    return fade.sCurve ? (1.0F - std::cos(std::numbers::pi_v<float> * x)) * 0.5F : x;
}

} // namespace fh
