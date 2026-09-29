#include "MenuCommon.h"

#include "AnarkFormats.h"

#include "base/Error.h"
#include "base/Log.h"
#include "platform/Input.h"
#include "vfs/Vfs.h"

#include <array>
#include <format>

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

std::string helpIconTexture(HelpIcon icon)
{
    return texturePathForScene(kHelpIcons[static_cast<std::size_t>(icon)]);
}

void HelpBar::bind(const Presentation& scene, int bar)
{
    m_slots.clear();
    m_icons.clear();
    const int contract = scene.contract(bar, "HELP_BUTTON_BAR");
    m_contract = contract;
    const int buttons = scene.contractParameter(contract, "GROUP_BUTTONS");
    if (buttons < 0) {
        return;
    }
    for (const int child : scene.children(buttons)) {
        const int slotContract = scene.contract(child, "HELP_BUTTON");
        if (slotContract < 0) {
            continue;
        }
        Slot slot;
        slot.contract = slotContract;
        slot.text = scene.contractParameter(slotContract, "TEXT_BUTTON_TEXT");
        slot.material = scene.contractParameter(slotContract, "MATERIAL_BUTTON_LOGO");
        m_slots.push_back(slot);
    }
}

void HelpBar::set(Presentation& scene, const std::vector<Entry>& entries)
{
    m_icons.assign(m_slots.size(), std::string());
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

} // namespace fh::ui
