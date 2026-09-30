#pragma once

#include <cstdint>

namespace fh {

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

/// The dead zone a pad record starts with for both sticks. Verified: the
/// pad record's constructor (0x82CBE25C) stores 8689 (0x821547C4) as the
/// inner and 32767 (0x821547C8) as the outer edge of both sticks, and the
/// input manager uses the same pair for a pad it has no record for
/// (0x82CBC6E8).
inline constexpr StickDeadZone kPadDefaultStickDeadZone{8689.0F, 32767.0F};

/// The left stick's dead zone for a pad, from the profile's
/// ControllerAdvancedOptions/SteeringAxisDeadzoneInside and ...Outside.
///
/// Verified at 0x82569E20, which applies the controller options: it reads
/// both values from "<device>AdvancedOptions/" ("Controller" unless the
/// pad record's device byte at +6 is 1, "Wheel", or 2, "Bristol"); for a
/// controller it raises each to at least 0.23953979 (0x82017664, which is
/// 7849 / 32767, XInput's recommended left stick dead zone); it multiplies
/// both by 32767 (0x820139C0) and hands them to the setter at 0x82CB9A88,
/// which stores them as the pad record's left stick inner (+632) and outer
/// (+636) edges. ForzaProfile.sch defaults them to 0.24 and 0.95.
StickDeadZone controllerSteeringDeadZone(float inside, float outside);

/// Shapes a thumbstick the way the game's pad update does (0x82CB9CB8,
/// called for the left stick at 0x82CBAA14 with the XInput state's
/// sThumbLX and sThumbLY).
///
/// Verified: at or inside the inner edge (x^2 + y^2 <= inner^2, the sum
/// taken in integers) both axes are 0. Outside it, each raw axis is scaled
/// by (length - inner) / length and then by 1 / (outer - inner) (1.0 at
/// 0x82000D28), and each is clamped to [-1, 1] (0x82000D40, 0x82000D28) on
/// its own, so the dead zone is radial while the saturation is per axis:
/// a stick pushed straight along one axis is full from `outer` on, and
/// the raw range's -32768 end reaches -1 like 32767 reaches 1.
/// Inferred: `length` is the square root of x^2 + y^2. The function takes
/// it from VMX128 code objdump does not decode, but the scalar code after
/// it uses the value as the length ((length - inner) / length, and the
/// same difference over outer - inner as the stick's magnitude, capped
/// at 1).
///
/// Not the game's: the sum is taken in 64 bits, where the game's 32-bit
/// sum wraps negative for a corner no round stick gate reaches
/// (|x| = |y| = 32768); and a dead zone whose outer edge is not beyond its
/// inner edge, which the game would divide by zero with, gives full
/// deflection past the inner edge.
StickPosition shapeThumbstick(std::int16_t rawX, std::int16_t rawY, const StickDeadZone& deadZone);

} // namespace fh
