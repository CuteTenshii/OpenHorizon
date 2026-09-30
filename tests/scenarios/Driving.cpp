// Scenarios of driving and collisions: turning from a standstill, the chase
// camera, and cars driven into a wall and into a parked truck.

#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>

#include "car/CarPhysics.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <format>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openhorizon::test {
namespace {

TEST_CASE_METHOD(GameScenario, "gentleLeftTurnFromStandstillDoesNotSpin", "[scenario][driving]")
{
    // A player on a keyboard pulls away at full throttle and eases left: the
    // car must turn, not spin, with the assists on (the default).
    openhorizon::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    // The countdown holds the car; wait for the start.
    session.runUntil(30.0F, [&] { return !session.game().race() || session.game().race()->started(); });
    session.run(3.0F, {.throttle = 1.0F, .steer = -0.3F});
    session.capture("left_turn");
    const openhorizon::CarPhysics& car = session.game().playerCar();
    const float yawRate = glm::dot(car.angularVelocity(), car.up());
    const glm::vec3 velocity = car.velocity();
    const float bodySlip = glm::length(velocity) > 1.0F
        ? std::abs(std::atan2(glm::dot(velocity, car.right()), glm::dot(velocity, car.forward())))
        : 0.0F;
    CHECK(std::abs(yawRate) < 1.0F);
    CHECK(bodySlip < 0.5F);
}

TEST_CASE_METHOD(GameScenario, "chaseCameraStaysBehindInATurn", "[scenario][driving]")
{
    openhorizon::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.runUntil(30.0F, [&] { return !session.game().race() || session.game().race()->started(); });
    session.run(4.0F, {.throttle = 0.6F});
    session.run(1.5F, {.throttle = 0.4F, .steer = -0.3F});
    session.capture("camera_turn");
    const openhorizon::CarPhysics& car = session.game().playerCar();
    const openhorizon::render::Camera& camera = session.game().camera();
    // The chase camera looks at the car from behind: its view heading stays
    // close to the car's heading through a gentle turn.
    const float view = headingOf(camera.target - camera.position);
    CHECK(angleBetween(view, headingOf(car.forward())) < 0.6F);
}

/// What a car did after driving into a wall: the speed it hit at and how
/// far it turned about its vertical axis over the seconds after.
struct WallHit {
    bool hit = false;
    float speed = 0.0F;
    float totalYaw = 0.0F;
    float finalYawRate = 0.0F;
};

/// Drives the default car from `start` at `target` at up to `speed` m/s
/// until it hits something, then lets go of every control for `seconds`
/// and measures how far it turns.
WallHit driveIntoWall(glm::vec2 start, glm::vec2 target, float speed, float seconds, const std::string& name)
{
    openhorizon::GameOptions options;
    options.spawnAt = start;
    options.spawnHeading = std::atan2(target.x - start.x, target.y - start.y);
    options.startTime = 12.0F * 3600.0F;
    Session session(options);
    WallHit result;
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    float previousSpeed = 0.0F;
    for (float t = 0.0F; t < 20.0F; t += kStep) {
        const float now = session.game().playerCar().forwardSpeed();
        // A collision takes more speed in one frame than the brakes can.
        if (previousSpeed - now > 3.0F) {
            result.hit = true;
            result.speed = previousSpeed;
            break;
        }
        previousSpeed = now;
        const openhorizon::CarPhysics& car = session.game().playerCar();
        session.run(kStep, {.throttle = now < speed ? 1.0F : 0.2F, .steer = steerTowards(car, target)});
    }
    if (!result.hit) {
        return result;
    }
    float yaw = headingOf(session.game().playerCar().forward());
    for (float t = 0.0F; t < seconds; t += kStep) {
        session.run(kStep, {});
        const float next = headingOf(session.game().playerCar().forward());
        result.totalYaw += std::remainder(next - yaw, 2.0F * std::numbers::pi_v<float>);
        yaw = next;
    }
    const openhorizon::CarPhysics& car = session.game().playerCar();
    result.finalYawRate = glm::dot(car.angularVelocity(), car.up());
    session.capture(name);
    std::fprintf(stderr, "%s: hit at %.1f m/s, turned %.2f rad after, yaw rate %.2f rad/s at the end\n", name.c_str(),
        result.speed, result.totalYaw, result.finalYawRate);
    return result;
}

// A player drove into a wall and the car spun round about eight times. The
// west face of a building south-west of the festival (colorado, x -3898,
// z 1022 to 1070) stands at the end of a flat parking lot; the default car
// drives across the lot into it head-on and at 40 degrees, at the speeds
// the lot's length allows.
TEST_CASE_METHOD(GameScenario, "wallImpactDoesNotSpinTheCar", "[scenario][driving]")
{
    struct Approach {
        const char* name;
        glm::vec2 target;
        float angle;
        float runUp;
        float speed;
    };
    constexpr float kDegree = std::numbers::pi_v<float> / 180.0F;
    const std::array<Approach, 4> approaches{{
        {"wall_head_on_60", {-3898.0F, 1046.0F}, 0.0F, 68.0F, 60.0F / 3.6F},
        {"wall_head_on_100", {-3898.0F, 1046.0F}, 0.0F, 68.0F, 100.0F / 3.6F},
        {"wall_40_degrees_60", {-3898.0F, 1062.0F}, 40.0F * kDegree, 55.0F, 60.0F / 3.6F},
        {"wall_40_degrees_80", {-3898.0F, 1062.0F}, 40.0F * kDegree, 55.0F, 80.0F / 3.6F},
    }};
    for (const Approach& approach : approaches) {
        const float heading = std::numbers::pi_v<float> * 0.5F - approach.angle;
        const glm::vec2 direction(std::sin(heading), std::cos(heading));
        const WallHit hit = driveIntoWall(
            approach.target - direction * approach.runUp, approach.target, approach.speed, 5.0F, approach.name);
        CHECK(hit.hit);
        // A car glances off or stops against the wall; it does not turn
        // round, and it has stopped turning a few seconds later.
        CHECK(std::abs(hit.totalYaw) < std::numbers::pi_v<float>);
        CHECK(std::abs(hit.finalYawRate) < 0.5F);
    }
}

/// How deep `car`'s contact spheres reach into `other`'s bounding box (its
/// maxdata.xml BoundingBox), metres: the overlap a player sees, whatever
/// shapes stand in for the bodies.
float boxPenetration(const openhorizon::CarPhysics& car, const openhorizon::CarPhysics& other)
{
    const glm::mat4 toOther = glm::inverse(other.transform()) * car.transform();
    const glm::vec3 lo(other.spec().boundsMin[0], other.spec().boundsMin[1], other.spec().boundsMin[2]);
    const glm::vec3 hi(other.spec().boundsMax[0], other.spec().boundsMax[1], other.spec().boundsMax[2]);
    float deepest = 0.0F;
    for (const openhorizon::CarSpec::CollisionSphere& sphere : openhorizon::carContactSpheres(car.spec())) {
        const glm::vec3 centre(toOther * glm::vec4(sphere.centre[0], sphere.centre[1], sphere.centre[2], 1.0F));
        const glm::vec3 nearest = glm::clamp(centre, lo, hi);
        const float outside = glm::length(centre - nearest);
        const glm::vec3 toFaces = glm::min(centre - lo, hi - centre);
        const float inside = std::min({toFaces.x, toFaces.y, toFaces.z});
        deepest = std::max(deepest, outside > 0.0F ? sphere.radius - outside : sphere.radius + inside);
    }
    return deepest;
}

// Players drove into a traffic box truck (KEN_T440_12) and ended up inside
// it, or sent it flying upside down. On the flat parking lot in front of
// the building above, the default car drives at a truck standing still,
// into its tail and into its side, at 30 and 60 km/h.
TEST_CASE_METHOD(GameScenario, "carDoesNotSinkIntoOrLaunchATruck", "[scenario][driving]")
{
    const glm::vec2 truckAt(-3920.0F, 1046.0F);
    const float halfPi = std::numbers::pi_v<float> * 0.5F;
    struct Approach {
        const char* name;
        float truckHeading;
        float speed;
    };
    const std::array<Approach, 4> approaches{{
        {"truck_tail_30", halfPi, 30.0F / 3.6F},
        {"truck_tail_60", halfPi, 60.0F / 3.6F},
        {"truck_side_30", 0.0F, 30.0F / 3.6F},
        {"truck_side_60", 0.0F, 60.0F / 3.6F},
    }};
    for (const Approach& approach : approaches) {
        openhorizon::GameOptions options;
        options.spawnAt = truckAt - glm::vec2(48.0F, 0.0F);
        options.spawnHeading = halfPi;
        options.startTime = 12.0F * 3600.0F;
        options.parkedCar = "KEN_T440_12";
        options.parkedAt = truckAt;
        options.parkedHeading = approach.truckHeading;
        Session session(options);
        CHECK(session.waitForCar());
        session.run(1.0F, {});
        const openhorizon::CarPhysics* truck = session.game().parkedCar();
        CHECK(truck != nullptr);
        if (!truck) {
            continue;
        }
        const openhorizon::CarPhysics& car = session.game().playerCar();
        float speedAtImpact = 0.0F;
        for (float t = 0.0F; t < 20.0F && speedAtImpact == 0.0F; t += kStep) {
            const float speed = car.speed();
            session.run(kStep, {.throttle = speed < approach.speed ? 1.0F : 0.0F, .steer = steerTowards(car, truckAt)});
            // A collision takes more speed in one frame than the brakes can.
            if (speed - car.speed() > 1.5F) {
                speedAtImpact = speed;
            }
        }
        CHECK(speedAtImpact > 0.0F);
        float truckSpeed = 0.0F;
        float truckLowestUp = 1.0F;
        float deepest = boxPenetration(car, *truck);
        for (float t = 0.0F; t < 3.0F; t += kStep) {
            session.run(kStep, {});
            truckSpeed = std::max(truckSpeed, truck->speed());
            truckLowestUp = std::min(truckLowestUp, truck->up().y);
            deepest = std::max(deepest, boxPenetration(car, *truck));
        }
        session.capture(approach.name);
        // Momentum: the truck cannot come away faster than the whole car's
        // momentum, shared by both, would push it even in a fully elastic
        // hit.
        const float pushLimit = 2.0F * car.mass() * speedAtImpact / (car.mass() + truck->mass());
        std::fprintf(stderr,
            "%s: hit at %.1f m/s, truck up to %.2f m/s (limit %.2f), lowest up %.2f, %.2f m into the truck at most, "
            "%.2f m after 3 s\n",
            approach.name, speedAtImpact, truckSpeed, pushLimit, truckLowestUp, deepest, boxPenetration(car, *truck));
        CHECK(truckLowestUp > 0.7F);
        CHECK(truckSpeed <= pushLimit + 0.1F);
        // The spheres standing in for the bodies leave shallow dips between
        // neighbours; a bumper may reach that far, no further.
        CHECK(deepest < 0.1F);
        CHECK(boxPenetration(car, *truck) < 0.1F);
    }
}

} // namespace
} // namespace openhorizon::test
