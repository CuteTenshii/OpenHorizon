#pragma once

#include <vector>

namespace fh::ui {

class Presentation;
class UiRenderer;

/// Width of a text element as it draws now, in design units: its current
/// text, size and tracking (animated values first) at kTextPointsToUnits,
/// before the element's own x scale.
float textElementWidth(const Presentation& scene, UiRenderer& renderer, int element);

/// Every SUPER_STACKER contract of a scene: the Anark component that lines
/// elements up one after another and stretches models (the brush-stroke
/// bars) under them, so a text's bar grows with the text. The HUD and the
/// pause menu's entries (925_pause_menu: Text then MessageIndicator, with
/// Crisp Tape, Smudge Tape, Dirt Tile and Clipon under them) use it.
///
/// Verified from the executable: what the attach (0x828cf6a8) captures and
/// its defaults, and the layout (widths 0x828ca920, placement 0x828cfab8,
/// both called from the contract's slot 5, 0x828d1e08), and that only text
/// elements have a width: the attach keeps an element in the width slot
/// only when it casts from CUI4Element to CUI4TextElement (0x828cf7bc, the
/// RTTI at 0x83324d40 and 0x83324d9c), so a stacked component, such as the
/// activation prompt's Title, moves by the texts before it but pushes
/// nothing along. Inferred: that the layout runs whenever a stacked text
/// changes; what makes the game run it was not traced, and running it every
/// frame from the captured values gives the same result.
class SuperStackers {
public:
    /// Captures every SUPER_STACKER in `scene` (only those below `root`
    /// when it is not -1) with the values its elements have now, which
    /// every layout starts from again. Call on the scene as loaded (or
    /// reset), before any layout.
    void bind(const Presentation& scene, UiRenderer& renderer, int root = -1);
    /// Places the stacked elements and stretches the scalers for the texts
    /// the scene shows now. Writes only x positions, x scales and texture
    /// offsets.
    void layout(Presentation& scene, UiRenderer& renderer) const;

    bool empty() const { return m_stackers.empty(); }

private:
    struct Element {
        int element = -1;
        bool text = false;
        float x0 = 0.0F;
        /// The width the text was authored at, times its scale.
        float trim = 0.0F;
    };
    struct Scaler {
        int element = -1;
        /// The model's first image, whose texture the scaler slides.
        int image = -1;
        float size = 0.0F;
        int first = 0;
        int last = 0;
        int mode = 0;
        float x0 = 0.0F;
        float scale0 = 1.0F;
        float u0 = 0.0F;
        float uScale0 = 1.0F;
    };
    struct Stacker {
        int alignment = 1;
        bool positionText = true;
        std::vector<Element> elements;
        std::vector<Scaler> scalers;
    };
    std::vector<Stacker> m_stackers;
};

} // namespace fh::ui
