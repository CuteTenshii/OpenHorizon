#pragma once

#include <glm/glm.hpp>

namespace fh::render::post {

/// The filmic tone curve of the final combine (pixel shader 0x821C8ABC and
/// the vertex shader 0x821BD8C4 that evaluates it at the white point):
///
///     f(x) = (x (A x + C B) + D E) / (x (A x + B) + D F) - E / F
///
/// applied to exposed linear colour and divided by f(W). The constants
/// arrive as filmicParams1 = (W, A, B, C), filmicParams2 = (D, E, F, log2
/// exposure bias) and filmicParamsE = (C B, D F, D E, E / F), the last
/// computed on the CPU at 0x82431C60 (verified). TrackSettings.xml's
/// filmicTone names them white, shoulderStrength, linearStrength,
/// linearAngle, toeStrength, toeNumerator and toeDenominator in that order
/// (the settings block keeps them at +0xC0..+0xD8 in file order).
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

/// The log2 exposure the combine and the hot extract apply (vertex shaders
/// 0x821BD8C4 and 0x821BD58C): the adapted luminance picks the segment of
/// the four keys it falls in (below the second key the first segment, at
/// or above the third the last), the exposure is interpolated along it with
/// the fraction clamped to [0, 1], and `bias` is added. The keys are the
/// settings block's divided by 255 (0x82487C9C).
float exposureLog2(float luminance, const glm::vec4& keys, const glm::vec4& exposures, float bias);

/// The colour matrix the combine applies to tonemapped linear colour
/// (colorTransform, built at 0x82432A64..0x82433200 and uploaded as three
/// columns of a 4x3 matrix): contrast, then brightness, then saturation
/// towards the luminance `weights`, then a per-channel scale, in that order
/// on the colour. For column vectors: result = M * vec4(colour, 1).
///
/// Verified from the helpers the builder calls: contrast k (0x824153E0)
/// maps c to (c - 0.5) (1 + k) + 0.5; brightness b (0x82415388) adds b;
/// saturation s (0x82415E70) gives s c + (1 - s) dot(c, weights).
glm::mat4 colourTransform(
    float contrast, float brightness, float saturation, const glm::vec3& weights, const glm::vec3& postMod);

/// The combine's vignetteTransform (0x82433360..0x824333F4): a 2x2 matrix
/// (x, y, z, w) applied as (u x + v y, u z + v w) to the pixel's offset
/// from the vignette centre, from the angle in radians, the scale and the
/// picture's width over height.
glm::vec4 vignetteTransform(float angleRadians, const glm::vec2& scale, float aspect);

} // namespace fh::render::post
