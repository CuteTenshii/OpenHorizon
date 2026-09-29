#pragma once

#include "Presentation.h"

#include "data/StringTable.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh {
class Vfs;
struct InputState;
} // namespace fh

/// Pieces the game's front-end screens share: the string tables by name,
/// the pad input a screen reads, and the help button bar most scenes carry.
namespace fh::ui {

/// The game's string tables, looked up by table name and string id as the
/// executable does (0x82d6d6d8 takes a table name such as "PauseMenu" and a
/// hashed IDS_ id). Tables load on first use, the chosen language first and
/// English when that language lacks the table.
class UiStrings {
public:
    UiStrings(const Vfs& vfs, std::string language);

    /// The text of `id` in `table`, or the id itself when it is missing so
    /// the gap stays visible.
    std::string get(std::string_view table, std::string_view id);
    /// Whether `table` has `id`. Some ids the game references belong to
    /// downloadable content whose tables are not on the disc.
    bool has(std::string_view table, std::string_view id);

private:
    const StringTable* table(std::string_view name);

    const Vfs& m_vfs;
    std::string m_language;
    std::unordered_map<std::string, std::optional<StringTable>> m_tables;
};

/// What a menu screen reads from the pad in one frame. The game's front end
/// binds its navigation in input context 10 (the table at 0x82598a38): up,
/// down, left and right on the D-pad and on the left stick, A to accept, B
/// to go back, Start and Back. Presses are edges: true on the frame the
/// control goes down.
struct MenuInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool accept = false;
    bool back = false;
    bool start = false;
    /// The Back button (the one with the left-pointing arrow).
    bool select = false;
    /// X and Y, which screens give their own meaning in their help bars.
    bool x = false;
    bool y = false;
    /// The sticks, -1 to 1, y up.
    glm::vec2 leftStick{0.0F};
    glm::vec2 rightStick{0.0F};
};

/// Turns the engine's pad state into MenuInput. Direction presses come from
/// the D-pad edges and from the left stick crossing `stickThreshold`; the
/// threshold the game's front end uses for its stick directions was not
/// traced, so the caller chooses it.
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

/// The pad buttons the help bar shows, as the index into the icon table the
/// help bar reads (0x83333b98, one row of 17 textures per controller
/// family; the pad row is the one used here).
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

/// The game path of a help icon's texture, from the icon table.
std::string helpIconTexture(HelpIcon icon);

/// A scene's HELP_BUTTON_BAR: a row of HELP_BUTTON slots, each with a
/// logo material and a text. The game appends entries in order (0x828ddca8
/// pushes each onto the bar's list) and the bar shows them in its slots
/// left to right, firing SHOWN on the slots in use and HIDDEN on the rest.
/// The logo is the material's texture, which the bar replaces with the
/// icon's (MATERIAL_BUTTON_LOGO).
class HelpBar {
public:
    struct Entry {
        HelpIcon icon = HelpIcon::A;
        std::string text;
    };

    /// Binds the bar whose contract sits on `bar` (the scene's
    /// COMPONENT_HELP_BUTTON_BAR). Does nothing when `bar` is -1.
    void bind(const Presentation& scene, int bar);
    void set(Presentation& scene, const std::vector<Entry>& entries);
    /// Shows or hides the whole bar with its ENABLED and DISABLED events.
    void enable(Presentation& scene, bool enabled) const;
    /// Points the logos of the slots in use at their icons' textures.
    void apply(std::vector<DrawItem>& items) const;
    std::size_t slotCount() const { return m_slots.size(); }

private:
    struct Slot {
        int contract = -1;
        int text = -1;
        int material = -1;
    };
    int m_contract = -1;
    std::vector<Slot> m_slots;
    std::vector<std::string> m_icons;
};

/// How much larger the game draws a text element than its size taken as
/// ems of the vector font. The render data stores, next to each text's
/// size, the width its authored string had in the editor (the f32 that
/// render data 1008 adds to text records, read at 0x82f2cbe0); over the
/// scenes' sample strings that width is 1.30 to 1.38 times the string
/// measured at its size in ems ("BUTTON" in horizon_e 36: 220 against 164;
/// "BUTTON TEXT" in horizon_b 14: 112 against 81; "GAME PAUSED" at 13: 118
/// against 91; "22" in horizon_b 15: 22 against 17; "TITLETEXTGOESHERE" in
/// horizon_e 36: 554 against 414; "0" in horizon_e 54: 54 against 40;
/// "PLEASE WAIT" in horizon_e 16: 156 against 119; the loading tip's
/// placeholder in horizon_a 16: 493 against 360), which is the 96/72 of a
/// point size drawn in pixels. The runtime's own mapping from size to glyph
/// scale was not traced (the text setup at 0x82f2a1e8 passes the size
/// through a per-font factor that returns 1, 0x825ecbd8). The HUD draws
/// through UiRenderer without this factor.
inline constexpr float kTextPointsToUnits = 4.0F / 3.0F;

/// Scales the size of every text item by kTextPointsToUnits.
void applyTextScale(std::vector<DrawItem>& items);

/// The width of a text element's box in design units: its boxwidth
/// attribute, a fraction of the scene's width (the text setup at 0x82f2a1e8
/// multiplies the render data's copy by the screen width). 0 when unset.
float textBoxWidth(const Presentation& scene, int element);
/// The font and size a text element is authored with.
struct TextStyle {
    std::string font;
    float size = 0.0F;
};
TextStyle textStyle(const Presentation& scene, int element);

/// Breaks `text` into lines no wider than `width` design units when drawn
/// with `measure`, at spaces, keeping the text's own line breaks. A word
/// wider than the line keeps a line to itself. The Anark runtime wraps text
/// whose wordwrap attribute is set to its boxwidth; the wrap rule itself
/// (break at spaces) was not traced.
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

} // namespace fh::ui
