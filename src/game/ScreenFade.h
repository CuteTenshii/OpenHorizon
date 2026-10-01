#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string_view>

namespace openhorizon {

class Vfs;

/// A CFadeIn behaviour's <FadeOptions> in media/gamemodes/state_definitions.xml, or a cutscene camera's
/// <FadeInOptions> and <FadeOutOptions>.
struct FadeOptions {
    float duration = 0.0F;
    float hold = 0.0F;
    /// CurveType "S" rather than "Linear" (table at 0x82084730).
    bool sCurve = false;
    glm::vec3 colour{0.0F};
};

/// `stateId` is e.g. "fade_in" or "fade_out_half_sec"; nothing when the file or the state lacks a fade.
std::optional<FadeOptions> loadStateFade(const Vfs& vfs, std::string_view stateId);

/// Verified (0x8297BE60, 0x82902E08): opaque up to the shorter of duration and hold, clear from the longer, a
/// ramp between shaped by the curve (0x829004A0: S is (1 - cos(pi x)) / 2).
float fadeInOpacity(const FadeOptions& fade, float elapsed);

} // namespace openhorizon
