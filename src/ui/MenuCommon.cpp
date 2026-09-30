#include "MenuCommon.h"

#include "AnarkFormats.h"
#include "SuperStacker.h"

#include "base/Error.h"
#include "base/Log.h"
#include "platform/Input.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <limits>

namespace fh::ui {

namespace {

/// The pad row of the help bar's icon table (0x83333b98). The other two
/// rows of the table hold the same paths.
constexpr std::array<const char*, 17> kHelpIcons{
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\A_Button.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\B_Button.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\X_Button.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\Y_Button.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\Start_Button.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\L_Thumbstick.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\LTrigger.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\Triggers.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\LBumper.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\RBumper.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\D_Pad_UpDown.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\RTrigger.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\RThumbstick.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\Back_Button.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\L_Thumbstick_NoArrow.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\RThumbstick_NoArrow.tga",
    "GAME:\\Media\\UI\\Textures\\Horizon\\Controller\\D_Pad.tga",
};

/// What the help bar's layout (0x8289e100) adds after each button's text
/// before the next button: the float at 0x83333b94.
constexpr float kHelpButtonGap = 34.0F;

} // namespace

UiStrings::UiStrings(const Vfs& vfs, std::string language)
    : m_vfs(vfs)
    , m_language(std::move(language))
{
}

const StringTable* UiStrings::table(std::string_view name)
{
    const std::string key = toLower(name);
    auto it = m_tables.find(key);
    if (it == m_tables.end()) {
        std::optional<StringTable> loaded;
        for (const std::string& language : {m_language, std::string("en")}) {
            const std::string path = std::format("media/stringtables/{}/{}.str", language, key);
            if (const std::optional<Bytes> data = m_vfs.read(path)) {
                try {
                    loaded = StringTable::parse(*data, path);
                    break;
                } catch (const DataError& e) {
                    logWarning("ui: {}", e.what());
                }
            }
        }
        it = m_tables.emplace(key, std::move(loaded)).first;
    }
    return it->second ? &*it->second : nullptr;
}

std::string UiStrings::get(std::string_view tableName, std::string_view id)
{
    if (const StringTable* strings = table(tableName)) {
        if (std::optional<std::string> text = strings->get(stringIdHash(id))) {
            return *text;
        }
    }
    return std::string(id);
}

bool UiStrings::has(std::string_view tableName, std::string_view id)
{
    const StringTable* strings = table(tableName);
    return strings && strings->get(stringIdHash(id)).has_value();
}

MenuInput MenuInputReader::read(const InputState& input)
{
    MenuInput out;
    out.leftStick = {input.axis(Axis::LeftX), input.axis(Axis::LeftY)};
    out.rightStick = {input.axis(Axis::RightX), input.axis(Axis::RightY)};
    const auto crossed
        = [&](float now, float before, float sign) { return now * sign >= m_threshold && before * sign < m_threshold; };
    out.up = input.pressed(Button::DpadUp) || crossed(out.leftStick.y, m_previousStick.y, 1.0F);
    out.down = input.pressed(Button::DpadDown) || crossed(out.leftStick.y, m_previousStick.y, -1.0F);
    out.left = input.pressed(Button::DpadLeft) || crossed(out.leftStick.x, m_previousStick.x, -1.0F);
    out.right = input.pressed(Button::DpadRight) || crossed(out.leftStick.x, m_previousStick.x, 1.0F);
    out.accept = input.pressed(Button::A);
    out.back = input.pressed(Button::B);
    out.x = input.pressed(Button::X);
    out.y = input.pressed(Button::Y);
    out.start = input.pressed(Button::Start);
    out.select = input.pressed(Button::Back);
    m_previousStick = out.leftStick;
    return out;
}

void applyTextScale(std::vector<DrawItem>& items)
{
    for (DrawItem& item : items) {
        if (item.isText) {
            item.text.size *= kTextPointsToUnits;
            item.text.tracking *= kTextPointsToUnits;
            item.text.leading *= kTextPointsToUnits;
        }
    }
}

float textBoxWidth(const Presentation& scene, int element)
{
    if (element < 0) {
        return 0.0F;
    }
    const BgfFile& bgf = scene.files().bgf;
    const std::uint32_t boxWidth = anarkHash("boxwidth");
    for (const Attribute& attribute : bgf.elements[static_cast<std::size_t>(element)].attributes) {
        if (attribute.hash == boxWidth) {
            return attribute.asFloat() * static_cast<float>(bgf.width);
        }
    }
    return 0.0F;
}

TextStyle textStyle(const Presentation& scene, int element)
{
    TextStyle style;
    if (element < 0) {
        return style;
    }
    const std::uint32_t id = scene.files().bgf.elements[static_cast<std::size_t>(element)].id;
    for (const FbfRecord& record : scene.files().fbf.records) {
        if (record.id == id && record.kind == FbfKind::Text) {
            style.font = record.text.font;
            style.size = static_cast<float>(record.text.size);
        }
    }
    return style;
}

std::string helpIconTexture(HelpIcon icon)
{
    return texturePathForScene(kHelpIcons[static_cast<std::size_t>(icon)]);
}

void HelpBar::bind(const Presentation& scene, UiRenderer& renderer, int bar)
{
    m_slots.clear();
    m_icons.clear();
    m_used = 0;
    const int contract = scene.contract(bar, "HELP_BUTTON_BAR");
    m_contract = contract;
    const int buttons = scene.contractParameter(contract, "GROUP_BUTTONS");
    if (buttons < 0) {
        m_stackers = {};
        return;
    }
    m_stackers.bind(scene, renderer, bar);
    for (const int child : scene.children(buttons)) {
        const int slotContract = scene.contract(child, "HELP_BUTTON");
        if (slotContract < 0) {
            continue;
        }
        Slot slot;
        slot.contract = slotContract;
        slot.element = child;
        slot.text = scene.contractParameter(slotContract, "TEXT_BUTTON_TEXT");
        slot.material = scene.contractParameter(slotContract, "MATERIAL_BUTTON_LOGO");
        const int marker = scene.contractParameter(slotContract, "GROUP_LEFT_MARKER");
        if (marker >= 0 && slot.text >= 0) {
            slot.textOffset = scene.attribute(slot.text, attr::PositionX).value_or(0.0F)
                - scene.attribute(marker, attr::PositionX).value_or(0.0F);
        }
        for (const int part : scene.children(marker)) {
            if (const int stacker = scene.contract(part, "SUPER_STACKER"); stacker >= 0) {
                slot.pill = scene.contractParameter(stacker, "SCALER_0");
                break;
            }
        }
        m_slots.push_back(slot);
    }
}

void HelpBar::set(Presentation& scene, const std::vector<Entry>& entries)
{
    m_icons.assign(m_slots.size(), std::string());
    m_used = std::min(entries.size(), m_slots.size());
    for (std::size_t i = 0; i < m_slots.size(); ++i) {
        const Slot& slot = m_slots[i];
        const bool used = i < entries.size();
        scene.fireEvent(slot.contract, used ? "SHOWN" : "HIDDEN");
        if (used) {
            if (slot.text >= 0) {
                scene.setText(slot.text, entries[i].text);
            }
            m_icons[i] = helpIconTexture(entries[i].icon);
        }
    }
    if (entries.size() > m_slots.size()) {
        logWarning("ui: {} help buttons for a bar of {}", entries.size(), m_slots.size());
    }
}

void HelpBar::layout(Presentation& scene, UiRenderer& renderer) const
{
    m_stackers.layout(scene, renderer);
    float x = 0.0F;
    for (std::size_t i = 0; i < m_used; ++i) {
        const Slot& slot = m_slots[i];
        scene.setAttribute(slot.element, attr::PositionX, x);
        const float width = slot.text >= 0
            ? textElementWidth(scene, renderer, slot.text) * scene.attribute(slot.text, attr::ScaleX).value_or(1.0F)
            : 0.0F;
        x += slot.textOffset + width + kHelpButtonGap;
    }
}

void HelpBar::enable(Presentation& scene, bool enabled) const
{
    if (m_contract >= 0) {
        scene.fireEvent(m_contract, enabled ? "ENABLED" : "DISABLED");
    }
}

void HelpBar::apply(std::vector<DrawItem>& items) const
{
    for (DrawItem& item : items) {
        if (item.isText || item.model.layers.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < m_slots.size() && i < m_icons.size(); ++i) {
            if (m_icons[i].empty() || item.material != m_slots[i].material) {
                continue;
            }
            item.model.layers.front().texture = m_icons[i];
            item.model.layers.front().imageName = m_icons[i];
        }
    }
}

std::vector<HelpButtonExtent> HelpBar::extents(
    const Presentation& scene, UiRenderer& renderer, const std::vector<DrawItem>& items) const
{
    std::vector<HelpButtonExtent> out;
    for (std::size_t i = 0; i < m_used; ++i) {
        const Slot& slot = m_slots[i];
        if (slot.text < 0) {
            continue;
        }
        HelpButtonExtent extent;
        extent.left = scene.worldTransform(slot.element)[3].x;
        const float width = textElementWidth(scene, renderer, slot.text);
        const auto align = static_cast<int>(scene.attribute(slot.text, attr::HorzAlign).value_or(0.0F));
        const float start = align == 1 ? -0.5F * width : align == 2 ? -width : 0.0F;
        extent.textRight = (scene.worldTransform(slot.text) * glm::vec4(start + width, 0.0F, 0.0F, 1.0F)).x;
        extent.pillRight = -std::numeric_limits<float>::infinity();
        for (const DrawItem& item : items) {
            if (!item.isText && item.model.mesh && slot.pill >= 0 && item.element == slot.pill) {
                extent.pillRight = visibleRight(item.model);
            }
        }
        out.push_back(extent);
    }
    return out;
}

float visibleRight(const DrawModel& model)
{
    struct Point {
        float x = 0.0F;
        std::vector<float> u;
    };
    std::vector<Point> points;
    points.reserve(model.mesh->vertices.size());
    for (const MeshVertex& vertex : model.mesh->vertices) {
        Point point;
        point.x = (model.world * glm::vec4(vertex.position, 1.0F)).x;
        for (const DrawModel::Layer& layer : model.layers) {
            if (layer.wrapU == TextureWrap::Clamp) {
                point.u.push_back(glm::dot(layer.uvRow0, glm::vec3(vertex.uv, 1.0F)));
            }
        }
        points.push_back(std::move(point));
    }
    float right = -std::numeric_limits<float>::infinity();
    const auto inside
        = [](const Point& p) { return std::ranges::all_of(p.u, [](float u) { return u >= 0.0F && u <= 1.0F; }); };
    const std::vector<std::uint16_t>& indices = model.mesh->indices;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        for (int corner = 0; corner < 3; ++corner) {
            const Point& a = points[indices[t + static_cast<std::size_t>(corner)]];
            const Point& b = points[indices[t + static_cast<std::size_t>((corner + 1) % 3)]];
            if (inside(a)) {
                right = std::max(right, a.x);
            }
            // Where the edge crosses a layer's u = 0 or u = 1.
            for (std::size_t layer = 0; layer < a.u.size(); ++layer) {
                for (const float bound : {0.0F, 1.0F}) {
                    const float da = a.u[layer] - bound;
                    const float db = b.u[layer] - bound;
                    if (da * db >= 0.0F) {
                        continue;
                    }
                    const float f = da / (da - db);
                    Point crossing;
                    crossing.x = a.x + (b.x - a.x) * f;
                    for (std::size_t other = 0; other < a.u.size(); ++other) {
                        crossing.u.push_back(other == layer ? bound : a.u[other] + (b.u[other] - a.u[other]) * f);
                    }
                    if (inside(crossing)) {
                        right = std::max(right, crossing.x);
                    }
                }
            }
        }
    }
    return right;
}

} // namespace fh::ui
