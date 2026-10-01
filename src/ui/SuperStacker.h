#pragma once

#include <vector>

namespace openhorizon::ui {

class Presentation;
class UiRenderer;

/// Design units at kTextPointsToUnits, animated values first, before the
/// element's own x scale.
float textElementWidth(const Presentation& scene, UiRenderer& renderer, int element);

/// Every SUPER_STACKER contract of a scene: lines elements up and stretches
/// the brush-stroke bars under them so they grow with the text.
///
/// Verified: attach 0x828cf6a8 and its defaults, layout (widths 0x828ca920,
/// placement 0x828cfab8, from slot 5, 0x828d1e08); only CUI4TextElement has a
/// width (0x828cf7bc, RTTI 0x83324d40, 0x83324d9c). Inferred: the layout runs
/// when a stacked text changes; running it every frame gives the same result.
class SuperStackers {
public:
    /// Captures the stackers (below `root` unless -1) with their current values,
    /// which every layout starts from. Call on the scene as loaded or reset.
    void bind(const Presentation& scene, UiRenderer& renderer, int root = -1);
    /// Writes only x positions, x scales and texture offsets.
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
        /// The image whose texture the scaler slides.
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

} // namespace openhorizon::ui
