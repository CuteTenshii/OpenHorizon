#pragma once

#include <glm/glm.hpp>

namespace openhorizon::render::post {

/// The combine's filmic curve (0x821C8ABC, 0x821BD8C4), divided by f(W):
///     f(x) = (x (A x + C B) + D E) / (x (A x + B) + D F) - E / F
/// TrackSettings.xml's filmicTone gives W, A..F in field order (verified).
/// C B, D F, D E and E / F are precomputed on the CPU (0x82431C60).
struct Filmic {
    float white = 1.0F;
    float a = 0.0F;
    float b = 0.0F;
    float c = 0.0F;
    float d = 0.0F;
    float e = 0.0F;
    float f = 1.0F;
};

float filmic(const Filmic& curve, float x);

/// The log2 exposure of the combine and hot extract (0x821BD8C4,
/// 0x821BD58C), piecewise over four keys. Keys are the settings' / 255
/// (0x82487C9C).
float exposureLog2(float luminance, const glm::vec4& keys, const glm::vec4& exposures, float bias);

/// The combine's colorTransform (0x82432A64-0x82433200): contrast, brightness,
/// saturation, then per-channel scale, as M * vec4(colour, 1). Verified
/// helpers: 0x824153E0, 0x82415388, 0x82415E70.
glm::mat4 colourTransform(
    float contrast, float brightness, float saturation, const glm::vec3& weights, const glm::vec3& postMod);

/// The combine's vignetteTransform (0x82433360-0x824333F4): a 2x2 matrix applied as
/// (u x + v y, u z + v w) to the offset from the vignette centre.
glm::vec4 vignetteTransform(float angleRadians, const glm::vec2& scale, float aspect);

} // namespace openhorizon::render::post
