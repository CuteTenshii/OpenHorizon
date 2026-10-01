#pragma once

#include "Presentation.h"
#include "SuperStacker.h"

#include "data/StringTable.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {
class Vfs;
struct InputState;
} // namespace openhorizon

/// What the front-end screens share: string tables, pad input, help bar.
namespace openhorizon::ui {

/// String tables by name and IDS_ id, as 0x82d6d6d8 looks them up; loaded on
/// first use, falling back to English when the language lacks a table.
class UiStrings {
public:
    UiStrings(const Vfs& vfs, std::string language);

    /// The id itself when missing, so the gap stays visible.
    std::string get(std::string_view table, std::string_view id);
    /// Some ids belong to downloadable content not on the disc.
    bool has(std::string_view table, std::string_view id);

private:
    const StringTable* table(std::string_view name);

    const Vfs& m_vfs;
    std::string m_language;
    std::unordered_map<std::string, std::optional<StringTable>> m_tables;
};

/// Front-end controls of input context 10 (table 0x82598a38). Presses are
/// edges: true on the frame the control goes down.
struct MenuInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool accept = false;
    bool back = false;
    bool start = false;
    /// The Back button.
    bool select = false;
    /// Given meaning by each screen's help bar.
    bool x = false;
    bool y = false;
    /// The sticks, -1 to 1, y up.
    glm::vec2 leftStick{0.0F};
    glm::vec2 rightStick{0.0F};
};

/// The stick threshold is the caller's: the game's was not traced.
class MenuInputReader {
public:
    explicit MenuInputReader(float stickThreshold)
        : m_threshold(stickThreshold)
    {
    }

    MenuInput read(const InputState& input);

private:
    float m_threshold;
    glm::vec2 m_previousStick{0.0F};
};

/// Index into the help bar's icon table (0x83333b98, 17 textures per
/// controller family; the pad row is used).
enum class HelpIcon : int {
    A = 0,
    B = 1,
    X = 2,
    Y = 3,
    Start = 4,
    LeftStick = 5,
    LeftTrigger = 6,
    Triggers = 7,
    LeftBumper = 8,
    RightBumper = 9,
    DpadUpDown = 10,
    RightTrigger = 11,
    RightStick = 12,
    Back = 13,
    LeftStickNoArrow = 14,
    RightStickNoArrow = 15,
    Dpad = 16,
};

std::string helpIconTexture(HelpIcon icon);

/// The rightmost point of the mesh, in `model.world`'s layer space, where
/// every clamped layer's u lies in [0, 1]: SUPER_STACKER slides textures
/// along meshes that reach far past what shows.
float visibleRight(const DrawModel& model);

/// In layer units: origin, text's right end, pill's right end.
struct HelpButtonExtent {
    float left = 0.0F;
    float textRight = 0.0F;
    float pillRight = 0.0F;
};

/// A scene's HELP_BUTTON_BAR (entries appended by 0x828ddca8), with the logo
/// replaced through MATERIAL_BUTTON_LOGO and each slot's SUPER_STACKER.
///
/// Verified: attach 0x8289a7b8 reads the marker's and text's x into +244 and
/// +248, update 0x828a78d8 measures the text into +252, layout 0x8289e100
/// places each slot at a running sum of +248 - +244 + +252 + 34 (0x83333b94).
/// Inferred: +252 is the stackers' text width (h_map's slots agree). Missing:
/// the bar's alignment (+212, not traced); the group stays left-aligned.
class HelpBar {
public:
    struct Entry {
        HelpIcon icon = HelpIcon::A;
        std::string text;
    };

    /// `bar` is the scene's COMPONENT_HELP_BUTTON_BAR, on the scene as loaded;
    /// -1 does nothing.
    void bind(const Presentation& scene, UiRenderer& renderer, int bar);
    void set(Presentation& scene, const std::vector<Entry>& entries);
    /// Fits each pill to its logo and text and packs the slots in use.
    void layout(Presentation& scene, UiRenderer& renderer) const;
    /// Shows or hides the whole bar with its ENABLED and DISABLED events.
    void enable(Presentation& scene, bool enabled) const;
    void apply(std::vector<DrawItem>& items) const;
    std::size_t slotCount() const { return m_slots.size(); }
    std::vector<HelpButtonExtent> extents(
        const Presentation& scene, UiRenderer& renderer, const std::vector<DrawItem>& items) const;

private:
    struct Slot {
        int contract = -1;
        /// The HELP_BUTTON component, which the bar moves.
        int element = -1;
        int text = -1;
        int material = -1;
        /// The model SUPER_STACKER stretches under the logo and text.
        int pill = -1;
        /// The text's x less the left marker's, as loaded.
        float textOffset = 0.0F;
    };
    int m_contract = -1;
    std::vector<Slot> m_slots;
    std::vector<std::string> m_icons;
    std::size_t m_used = 0;
    SuperStackers m_stackers;
};

/// The game draws text 96/72 larger than its size in ems: the style setup
/// (0x82430aa0, 0x824309b8) scales by 4/3 (0x82000ca4) over the build size
/// 14, and the editor widths (render data 1008, 0x82f2cbe0) run 1.30 to 1.38
/// times the measured ones. Inferred: the held value is the element's size
/// (factor 1, 0x825ecbd8). The HUD draws without this factor.
inline constexpr float kTextPointsToUnits = 4.0F / 3.0F;

void applyTextScale(std::vector<DrawItem>& items);

/// The boxwidth attribute times the scene's width (0x82f2a1e8); 0 when unset.
float textBoxWidth(const Presentation& scene, int element);
struct TextStyle {
    std::string font;
    float size = 0.0F;
};
TextStyle textStyle(const Presentation& scene, int element);

/// Breaks at spaces within `width`, keeping the text's own breaks; an
/// over-long word gets its own line. The wrap rule itself was not traced.
template <typename Measure> std::string wrapText(std::string_view text, float width, Measure measure)
{
    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view paragraph = text.substr(start, end - start);
        std::string line;
        std::size_t at = 0;
        while (at < paragraph.size()) {
            std::size_t next = paragraph.find(' ', at);
            if (next == std::string_view::npos) {
                next = paragraph.size();
            }
            const std::string_view word = paragraph.substr(at, next - at);
            const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
            if (!line.empty() && measure(candidate) > width) {
                out += line;
                out += '\n';
                line = std::string(word);
            } else {
                line = candidate;
            }
            at = next + 1;
        }
        out += line;
        if (end >= text.size()) {
            break;
        }
        out += '\n';
        start = end + 1;
    }
    return out;
}

} // namespace openhorizon::ui
