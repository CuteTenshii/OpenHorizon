#pragma once

#include <cstdint>

namespace openhorizon {

/// A thumbstick's dead zone in the pad's raw units (XInput thumb values,
/// -32768 to 32767): nothing inside `inner`, full deflection from `outer` on.
struct StickDeadZone {
    float inner = 0.0F;
    float outer = 0.0F;
};

/// A shaped thumbstick, each axis from -1 to 1, x right and y as the raw
/// axis it came from.
struct StickPosition {
    float x = 0.0F;
    float y = 0.0F;
};

/// Both sticks' default dead zone. Verified: pad record constructor
/// 0x82CBE25C (8689 at 0x821547C4, 32767 at 0x821547C8), also 0x82CBC6E8.
inline constexpr StickDeadZone kPadDefaultStickDeadZone{8689.0F, 32767.0F};

/// The left stick's dead zone from the profile's
/// ControllerAdvancedOptions/SteeringAxisDeadzoneInside and ...Outside.
/// Verified at 0x82569E20: each at least 7849 / 32767 (0x82017664), times
/// 32767 (0x820139C0), set by 0x82CB9A88. ForzaProfile.sch defaults them to
/// 0.24 and 0.95.
StickDeadZone controllerSteeringDeadZone(float inside, float outside);

/// Shapes a thumbstick as the game's pad update does (0x82CB9CB8, left stick
/// at 0x82CBAA14): radial dead zone, scale 1 / (outer - inner) (1.0 at
/// 0x82000D28), per-axis clamp to [-1, 1] (0x82000D40). Verified; `length`
/// being the Euclidean length is inferred (VMX128 code).
/// Unlike the game, sums in 64 bits and survives outer <= inner.
StickPosition shapeThumbstick(std::int16_t rawX, std::int16_t rawY, const StickDeadZone& deadZone);

} // namespace openhorizon
