#pragma once

#include <optional>
#include <string_view>

namespace openhorizon {
class Vfs;
}

namespace openhorizon {

/// The values used from a render scenario (`media/renderscenarios/<name>.xml`).
/// Verified: in game Horizon_Race (PhotoMode in photo mode, 0x824BF2E0) is pushed over Global; a setting left out
/// keeps the one below. Settings table ends at 0x8224F894; EnvMapFrequencyScale is slot 30 (0x82424060, 0x82E3FF98).
struct RenderScenario {
    /// See render::envcube::facesPerFrame().
    std::optional<float> envMapFrequencyScale;

    static constexpr std::string_view kInGame = "Horizon_Race";
    /// The scenario below every other.
    static constexpr std::string_view kGlobal = "Global";

    /// Throws DataError when it is not a DynamicRenderSettings document.
    static RenderScenario parse(std::string_view xml);
    /// Throws DataError if the file is missing or malformed.
    static RenderScenario load(const Vfs& vfs, std::string_view name);
    /// `top` pushed over Global.
    static RenderScenario stacked(const Vfs& vfs, std::string_view top);
};

} // namespace openhorizon
