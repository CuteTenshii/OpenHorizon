#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string_view>

namespace fh {

class Vfs;

/// The options of a screen fade: a CFadeIn behaviour's <FadeOptions> in
/// media/gamemodes/state_definitions.xml, or a cutscene camera's
/// <FadeInOptions> and <FadeOutOptions>.
struct FadeOptions {
    float duration = 0.0F;
    float hold = 0.0F;
    /// CurveType "S" rather than "Linear": the curve names map through the
    /// table at 0x82084730, Linear 0 and S 1.
    bool sCurve = false;
    glm::vec3 colour{0.0F};
};

/// The FadeOptions of the CFadeIn behaviour of the state `stateId` of
/// media/gamemodes/state_definitions.xml ("fade_in", "fade_out_half_sec"),
/// or nothing when the file or the state lacks one.
std::optional<FadeOptions> loadStateFade(const Vfs& vfs, std::string_view stateId);

/// How opaque a fade from the colour is `elapsed` seconds after it began.
/// Verified: the fader's update (0x8297BE60) clamps its clock to the
/// duration and evaluates the fade at 0x82902E08: opaque up to the shorter
/// of the duration and the hold, clear from the longer on, a straight ramp
/// between, shaped by the curve (0x829004A0: S is (1 - cos(pi x)) / 2).
float fadeInOpacity(const FadeOptions& fade, float elapsed);

} // namespace fh
