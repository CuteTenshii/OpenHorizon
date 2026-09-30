// The pad's thumbstick shaping against the game's (PadInput.h): the radial
// dead zone, the profile's steering dead zones and full deflection at the
// raw values real pads reach. No disc needed.

#include "TestHarness.h"

#include "platform/PadInput.h"

#include <cstdint>

namespace {

using fh::controllerSteeringDeadZone;
using fh::shapeThumbstick;
using fh::StickDeadZone;

/// ForzaProfile.sch's ControllerAdvancedOptions defaults.
constexpr float kDefaultInside = 0.24F;
constexpr float kDefaultOutside = 0.95F;

/// CCarDynamics' pad steering (0x82DF9620) counts the stick as held at
/// full lock from this input on (0.976 at 0x8224ACA0); below it the car
/// never adds the lock the find-peak step gives.
constexpr float kCarFullInput = 0.976F;

StickDeadZone profileDefault()
{
    return controllerSteeringDeadZone(kDefaultInside, kDefaultOutside);
}

TEST(profileSteeringDeadZoneIsScaledToRawUnits)
{
    const StickDeadZone zone = profileDefault();
    CHECK_NEAR(zone.inner, 0.24F * 32767.0F, 1e-2);
    CHECK_NEAR(zone.outer, 0.95F * 32767.0F, 1e-2);
}

TEST(controllerSteeringDeadZoneIsAtLeastXInputs)
{
    // The game raises both edges to 7849 / 32767 for a controller.
    const StickDeadZone zone = controllerSteeringDeadZone(0.0F, 0.1F);
    CHECK_NEAR(zone.inner, 7849.0F, 1e-2);
    CHECK_NEAR(zone.outer, 7849.0F, 1e-2);
}

TEST(fullDeflectionReachesFullInput)
{
    const StickDeadZone zone = profileDefault();
    // What pads report at full travel along one axis varies; many stop
    // short of the raw range's ends.
    for (const std::int16_t raw : {std::int16_t{31129}, std::int16_t{32000}, std::int16_t{32767}}) {
        CHECK_NEAR(shapeThumbstick(raw, 0, zone).x, 1.0F, 1e-6);
        CHECK_NEAR(shapeThumbstick(static_cast<std::int16_t>(-raw), 0, zone).x, -1.0F, 1e-6);
        CHECK_NEAR(shapeThumbstick(0, raw, zone).y, 1.0F, 1e-6);
    }
    CHECK_NEAR(shapeThumbstick(-32768, 0, zone).x, -1.0F, 1e-6);
    CHECK_NEAR(shapeThumbstick(0, -32768, zone).y, -1.0F, 1e-6);
}

TEST(nearlyFullDeflectionCountsAsFullLockForTheCar)
{
    const StickDeadZone zone = profileDefault();
    const float expected = (31000.0F - zone.inner) / (zone.outer - zone.inner);
    const float left = shapeThumbstick(-31000, 0, zone).x;
    const float right = shapeThumbstick(31000, 0, zone).x;
    CHECK_NEAR(right, expected, 1e-5);
    CHECK_NEAR(left, -expected, 1e-5);
    CHECK(right >= kCarFullInput);
    CHECK(-left >= kCarFullInput);
}

TEST(deadZoneIsRadial)
{
    const StickDeadZone zone = profileDefault();
    CHECK_EQ(shapeThumbstick(7864, 0, zone).x, 0.0F);
    CHECK(shapeThumbstick(7865, 0, zone).x > 0.0F);
    // Each axis alone is past the old per-axis 12 %, but the stick is
    // inside the circle.
    const fh::StickPosition inside = shapeThumbstick(7000, -3000, zone);
    CHECK_EQ(inside.x, 0.0F);
    CHECK_EQ(inside.y, 0.0F);
    // Out of the circle both axes keep their share of the direction.
    const fh::StickPosition diagonal = shapeThumbstick(20000, -20000, zone);
    CHECK_NEAR(diagonal.x, -diagonal.y, 1e-6);
    const float length = 28284.271F;
    CHECK_NEAR(diagonal.x, 20000.0F * (length - zone.inner) / length / (zone.outer - zone.inner), 1e-4);
}

TEST(halfwayIsLinearPastTheInnerEdge)
{
    const StickDeadZone zone = profileDefault();
    const float middle = (zone.inner + zone.outer) * 0.5F;
    CHECK_NEAR(shapeThumbstick(static_cast<std::int16_t>(middle), 0, zone).x, 0.5F, 1e-4);
}

TEST(padDefaultDeadZone)
{
    CHECK_EQ(shapeThumbstick(8689, 0, fh::kPadDefaultStickDeadZone).x, 0.0F);
    CHECK_NEAR(shapeThumbstick(32767, 0, fh::kPadDefaultStickDeadZone).x, 1.0F, 1e-6);
    CHECK_NEAR(shapeThumbstick(-32768, 0, fh::kPadDefaultStickDeadZone).x, -1.0F, 1e-6);
}

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
