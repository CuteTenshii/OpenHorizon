#include "SuperStacker.h"

#include "MenuCommon.h"
#include "Presentation.h"
#include "UiRenderer.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace fh::ui {

float textElementWidth(const Presentation& scene, UiRenderer& renderer, int element)
{
    const FbfRecord* record = scene.record(element);
    if (!record) {
        return 0.0F;
    }
    const float size = scene.attribute(element, attr::Size).value_or(static_cast<float>(record->text.size));
    const float tracking = scene.attribute(element, attr::Tracking).value_or(record->text.tracking);
    return renderer.measure(
        record->text.font, scene.text(element), size * kTextPointsToUnits, tracking * kTextPointsToUnits);
}

void SuperStackers::bind(const Presentation& scene, UiRenderer& renderer, int root)
{
    m_stackers.clear();
    const auto below = [&](int element) {
        for (int at = element; at >= 0; at = scene.parent(at)) {
            if (at == root) {
                return true;
            }
        }
        return false;
    };
    for (int owner = 0; owner < static_cast<int>(scene.elementCount()); ++owner) {
        const int contract = scene.contract(owner, "SUPER_STACKER");
        if (contract < 0 || (root >= 0 && !below(owner))) {
            continue;
        }
        const auto integer = [&](std::string_view name, int fallback) {
            const std::optional<float> value = scene.contractNumber(contract, name);
            return value ? static_cast<int>(*value) : fallback;
        };
        Stacker stacker;
        // The defaults are the attach's (0x828cf6a8) when a scene leaves a
        // parameter out: left alignment, text positioned, a scaler over the
        // first element only.
        stacker.alignment = integer("ALIGNMENT", 1);
        stacker.positionText = scene.contractFlag(contract, "POSITION_TEXT").value_or(true);
        const int count = integer("NUM_ELEMENTS", 0);
        for (int i = 0; i < count; ++i) {
            Element stacked;
            stacked.element = scene.contractParameter(contract, std::format("ELEMENT_{}", i));
            // A path that names nothing keeps its place in the numbering the
            // scalers refer to, with no width.
            if (stacked.element < 0) {
                stacker.elements.push_back(stacked);
                continue;
            }
            stacked.x0 = scene.attribute(stacked.element, attr::PositionX).value_or(0.0F);
            if (const FbfRecord* record = scene.record(stacked.element); record && record->kind == FbfKind::Text) {
                stacked.text = true;
                // The width the editor stored, else the text's width now.
                const float authored = record->text.authoredWidth >= 0.0F
                    ? record->text.authoredWidth
                    : textElementWidth(scene, renderer, stacked.element);
                stacked.trim = authored * scene.attribute(stacked.element, attr::ScaleX).value_or(1.0F);
            }
            stacker.elements.push_back(stacked);
        }
        const int scalers = integer("NUM_SCALERS", 0);
        for (int i = 0; i < scalers; ++i) {
            Scaler scaler;
            scaler.element = scene.contractParameter(contract, std::format("SCALER_{}", i));
            const std::optional<float> size = scene.contractNumber(contract, std::format("SCALER_{}_SIZE", i));
            // A scaler without a size cannot convert widths to scale; the
            // attach's default for it was not traced, so such scalers stay
            // as authored.
            if (scaler.element < 0 || !size || *size == 0.0F) {
                continue;
            }
            scaler.size = *size;
            scaler.first = std::clamp(integer(std::format("SCALER_{}_FIRST_ELEMENT", i), 0), 0,
                std::max(static_cast<int>(stacker.elements.size()) - 1, 0));
            // -1 runs to the last element.
            scaler.last = integer(std::format("SCALER_{}_LAST_ELEMENT", i), 0);
            if (scaler.last != -1) {
                scaler.last = std::clamp(
                    scaler.last, scaler.first, std::max(static_cast<int>(stacker.elements.size()) - 1, scaler.first));
            }
            scaler.mode = integer(std::format("SCALER_{}_MODE", i), 0);
            const std::string uvMagic = std::format("SCALER_{}_UVMAGIC", i);
            if (scene.contractFlag(contract, uvMagic).value_or(false) || integer(uvMagic, 0) != 0) {
                scaler.mode = 1;
            }
            scaler.x0 = scene.attribute(scaler.element, attr::PositionX).value_or(0.0F);
            scaler.scale0 = scene.attribute(scaler.element, attr::ScaleX).value_or(1.0F);
            for (const int material : scene.children(scaler.element)) {
                if (scene.kind(material) != ElementKind::Material) {
                    continue;
                }
                for (const int image : scene.children(material)) {
                    if (scene.kind(image) == ElementKind::Image) {
                        scaler.image = image;
                        break;
                    }
                }
                break;
            }
            if (scaler.image >= 0) {
                scaler.u0 = scene.attribute(scaler.image, attr::PositionU).value_or(0.0F);
                scaler.uScale0 = scene.attribute(scaler.image, attr::ScaleU).value_or(1.0F);
            }
            stacker.scalers.push_back(scaler);
        }
        if (!stacker.elements.empty()) {
            m_stackers.push_back(std::move(stacker));
        }
    }
}

/// Each text counts for the width it has grown by since it was authored,
/// the elements move by the widths before them and by the alignment, and
/// each scaler stretches its model under a run of elements.
void SuperStackers::layout(Presentation& scene, UiRenderer& renderer) const
{
    for (const Stacker& stacker : m_stackers) {
        const std::size_t count = stacker.elements.size();
        std::vector<float> widths(count, 0.0F);
        std::vector<float> starts(count + 1, 0.0F);
        for (std::size_t i = 0; i < count; ++i) {
            const Element& stacked = stacker.elements[i];
            if (stacked.text) {
                const float scale = scene.attribute(stacked.element, attr::ScaleX).value_or(1.0F);
                widths[i] = textElementWidth(scene, renderer, stacked.element) * scale - stacked.trim;
            }
            starts[i + 1] = starts[i] + widths[i];
        }
        const float total = starts[count];
        const float offset = stacker.alignment == 0 ? -0.5F * total : stacker.alignment == 2 ? -total : 0.0F;
        for (std::size_t i = 0; i < count; ++i) {
            const Element& stacked = stacker.elements[i];
            if (stacked.element < 0) {
                continue;
            }
            float x = stacked.x0 + starts[i] + offset;
            if (stacked.text) {
                if (!stacker.positionText) {
                    continue;
                }
                const auto align = static_cast<int>(scene.attribute(stacked.element, attr::HorzAlign).value_or(0.0F));
                x += align == 1 ? 0.5F * widths[i] : align == 2 ? widths[i] : 0.0F;
            }
            scene.setAttribute(stacked.element, attr::PositionX, x);
        }
        for (const Scaler& scaler : stacker.scalers) {
            const float start = starts[static_cast<std::size_t>(scaler.first)];
            const float end = scaler.last < 0 ? total : starts[static_cast<std::size_t>(scaler.last) + 1];
            const float span = end - start;
            if (scaler.mode == 1) {
                // The model keeps its size and slides along with its texture
                // going the other way, so more of the texture's length shows.
                const float uScale
                    = scaler.image >= 0 ? scene.attribute(scaler.image, attr::ScaleU).value_or(1.0F) : 1.0F;
                const float shift = span * uScale / (scaler.scale0 * scaler.size);
                float x = scaler.x0 + start + offset;
                float u = 0.0F;
                if (scaler.u0 >= 0.0F) {
                    u = std::clamp(scaler.u0 - shift, 0.0F, 1.0F);
                } else {
                    u = std::clamp(scaler.u0 + shift, -1.0F, 0.0F);
                    x += span;
                }
                if (scaler.image >= 0) {
                    scene.setAttribute(scaler.image, attr::PositionU, u);
                }
                scene.setAttribute(scaler.element, attr::PositionX, x);
                continue;
            }
            const float scale = scaler.scale0 + span / scaler.size;
            scene.setAttribute(scaler.element, attr::PositionX, scaler.x0 + start + offset + 0.5F * span);
            scene.setAttribute(scaler.element, attr::ScaleX, scale);
            if (scaler.mode == 2 && scaler.image >= 0) {
                scene.setAttribute(scaler.image, attr::ScaleU, scaler.uScale0 * scale);
                scene.setAttribute(scaler.image, attr::PositionU, scaler.u0 * scale);
            }
        }
    }
}

} // namespace fh::ui
