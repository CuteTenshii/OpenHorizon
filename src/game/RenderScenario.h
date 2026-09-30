#pragma once

#include <optional>
#include <string_view>

namespace openhorizon {
class Vfs;
}

namespace openhorizon {

/// The values of a render scenario (`media/renderscenarios/<name>.xml`, a
/// DynamicRenderSettings document) this game uses.
///
/// Verified in the executable: the in-game scenario is Horizon_Race, or
/// PhotoMode in photo mode (0x824BF2E0, chosen by the mode at +612), pushed
/// over Global. Each setting is a record of the table ending at
/// 0x8224F894 whose accessor answers its slot in the settings block (the
/// renderer reads EnvMapFrequencyScale from slot 30, 0x82424060, its record
/// answering 30 at 0x82E3FF98). A scenario that leaves a setting out keeps
/// the one below it in the stack.
struct RenderScenario {
    /// How often the environment cube's faces are redrawn (see
    /// render::envcube::facesPerFrame()).
    std::optional<float> envMapFrequencyScale;

    /// The in-game scenario, as the executable names it.
    static constexpr std::string_view kInGame = "Horizon_Race";
    /// The scenario below every other.
    static constexpr std::string_view kGlobal = "Global";

    /// Parses the XML text of a scenario. Throws DataError when it is not a
    /// DynamicRenderSettings document.
    static RenderScenario parse(std::string_view xml);
    /// Reads `media/renderscenarios/<name>.xml`; throws DataError if it is
    /// missing or malformed.
    static RenderScenario load(const Vfs& vfs, std::string_view name);
    /// The settings of `top` pushed over Global: each value from the first
    /// of the two that sets it.
    static RenderScenario stacked(const Vfs& vfs, std::string_view top);
};

} // namespace openhorizon
