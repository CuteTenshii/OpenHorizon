#include "PadInput.h"

#include <algorithm>
#include <cmath>

namespace fh {

namespace {

/// 0x820139C0: the profile's dead zones are fractions of this.
constexpr float kThumbRange = 32767.0F;
/// 0x82017664: 7849 / 32767, the least either steering edge may be for a
/// controller.
constexpr float kControllerMinSteeringDeadZone = 7849.0F / kThumbRange;

} // namespace

StickDeadZone controllerSteeringDeadZone(float inside, float outside)
{
    return {std::max(inside, kControllerMinSteeringDeadZone) * kThumbRange,
        std::max(outside, kControllerMinSteeringDeadZone) * kThumbRange};
}

StickPosition shapeThumbstick(std::int16_t rawX, std::int16_t rawY, const StickDeadZone& deadZone)
{
    const std::int64_t x = rawX;
    const std::int64_t y = rawY;
    const auto lengthSquared = static_cast<float>(x * x + y * y);
    if (lengthSquared <= deadZone.inner * deadZone.inner) {
        return {};
    }
    const float length = std::sqrt(lengthSquared);
    const float span = deadZone.outer - deadZone.inner;
    if (span <= 0.0F) {
        return {static_cast<float>((x > 0) - (x < 0)), static_cast<float>((y > 0) - (y < 0))};
    }
    const float scale = (length - deadZone.inner) / length / span;
    return {
        std::clamp(static_cast<float>(x) * scale, -1.0F, 1.0F), std::clamp(static_cast<float>(y) * scale, -1.0F, 1.0F)};
}

} // namespace fh
