// The disc's cars driven on a flat or tilted plane, no world needed.
// openhorizon-cartest covers the performance figures.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "collision/SurfaceTable.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <optional>
#include <vector>

using Catch::Matchers::WithinAbs;

using openhorizon::test::gameDir;

namespace {

constexpr int kStepsPerSecond = 360;

/// The disc's database, loaded once for all tests.
struct Disc {
    openhorizon::Vfs vfs;
    std::optional<openhorizon::Database> db;

    static Disc& get(const char* dir)
    {
        static Disc disc;
        if (!disc.db) {
            disc.vfs.mountDisc(dir);
            disc.db.emplace(disc.vfs.readOrThrow("media/db/gamedb.slt"));
        }
        return disc;
    }

    openhorizon::CarSpec car(const char* media)
    {
        return openhorizon::loadCarSpec(*db, openhorizon::carIdByMediaName(*db, media), &vfs);
    }
};

/// A plane through the origin rising `grade` metres per metre towards -z.
class Slope final : public openhorizon::GroundQuery {
public:
    explicit Slope(float grade)
        : m_normal(glm::normalize(glm::vec3(0.0F, 1.0F, -grade)))
    {
    }

    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
        openhorizon::GroundHit& hit) const override
    {
        const float denominator = glm::dot(m_normal, direction);
        if (std::abs(denominator) < 1e-6F) {
            return false;
        }
        const float t = -glm::dot(m_normal, origin) / denominator;
        if (t < 0.0F || t > maxDistance) {
            return false;
        }
        hit.point = origin + direction * t;
        hit.normal = m_normal;
        hit.distance = t;
        hit.grip = 1.0F;
        return true;
    }

private:
    glm::vec3 m_normal;
};

void drive(openhorizon::CarPhysics& car, const openhorizon::GroundQuery& ground,
    const openhorizon::CarControls& controls, float seconds)
{
    const int steps = static_cast<int>(seconds * static_cast<float>(kStepsPerSecond));
    for (int i = 0; i < steps; ++i) {
        car.step(controls, ground);
    }
}

void accelerateTo(openhorizon::CarPhysics& car, const openhorizon::GroundQuery& ground, float kmh)
{
    openhorizon::CarControls controls;
    controls.throttle = 1.0F;
    for (int i = 0; i < kStepsPerSecond * 60 && car.speed() * 3.6F < kmh; ++i) {
        car.step(controls, ground);
    }
}

float yawRate(const openhorizon::CarPhysics& car)
{
    return glm::dot(car.angularVelocity(), car.up());
}

float bodySlip(const openhorizon::CarPhysics& car)
{
    const glm::vec3 v = car.velocity();
    return std::atan2(glm::dot(v, car.right()), glm::dot(v, car.forward()));
}

TEST_CASE("carLeftAloneOnFlatGroundStaysStill", "[car]")
{
    const char* dir = gameDir();
    openhorizon::FlatGround ground;
    openhorizon::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
    car.reset({0, 0, 0}, 0, ground);
    drive(car, ground, {}, 2.0F);
    const glm::vec3 start = car.position();
    drive(car, ground, {}, 30.0F);
    CHECK_THAT(glm::length(car.position() - start), WithinAbs(0.0, 0.01));
}

TEST_CASE("handbrakeHoldsACarOnASlope", "[car]")
{
    const char* dir = gameDir();
    // The tyre model has no static friction, so the car creeps a little on a steep slope.
    for (const float grade : {0.05F, 0.20F}) {
        const Slope ground(grade);
        openhorizon::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
        car.reset({0, 0, 0}, 0, ground);
        openhorizon::CarControls controls;
        controls.handbrake = 1.0F;
        drive(car, ground, controls, 2.0F);
        const glm::vec3 start = car.position();
        drive(car, ground, controls, 30.0F);
        CHECK(glm::length(car.position() - start) < 3.0F);
        CHECK(car.speed() < 0.1F);
    }
}

TEST_CASE("assistsRecoverFromPowerOversteer", "[car]")
{
    const char* dir = gameDir();
    openhorizon::FlatGround ground;
    openhorizon::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
    car.reset({0, 0, 0}, 0, ground);
    accelerateTo(car, ground, 70.0F);
    openhorizon::CarControls controls;
    controls.throttle = 1.0F;
    controls.steer = 1.0F;
    drive(car, ground, controls, 1.0F);
    controls.steer = 0.0F;
    drive(car, ground, controls, 5.0F);
    CHECK(std::abs(yawRate(car)) < 0.1F);
    CHECK(std::abs(bodySlip(car)) < 0.2F);
}

TEST_CASE("steeringLockIsTheSameBothWays", "[car]")
{
    const char* dir = gameDir();
    openhorizon::FlatGround ground;
    const openhorizon::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
    const auto lockAt = [&](float steer) {
        openhorizon::CarPhysics car(spec);
        car.reset({0, 0, 0}, 0, ground);
        openhorizon::CarControls controls;
        controls.steer = steer;
        drive(car, ground, controls, 1.0F);
        return std::pair{car.wheels()[0].steerAngle, car.wheels()[1].steerAngle};
    };
    const auto [leftFl, leftFr] = lockAt(-1.0F);
    const auto [rightFl, rightFr] = lockAt(1.0F);
    // The inner wheel turns more (Ackermann), and the sides mirror.
    CHECK_THAT(leftFl, WithinAbs(-rightFr, 1e-4));
    CHECK_THAT(leftFr, WithinAbs(-rightFl, 1e-4));
    CHECK(std::abs(leftFl) > std::abs(leftFr));
}

/// The steering angle at the middle of the front axle from the two
/// Ackermann angles: tan = 2 / (cot inner + cot outer).
float centreSteerAngle(const openhorizon::CarPhysics& car)
{
    const float a = car.wheels()[0].steerAngle;
    const float b = car.wheels()[1].steerAngle;
    if (std::abs(a) < 1e-6F || std::abs(b) < 1e-6F) {
        return 0.0F;
    }
    return std::atan(2.0F / (1.0F / std::tan(a) + 1.0F / std::tan(b)));
}

/// The centre steering angle in degrees after a second of `input` from `kmh`.
float steadySteerAngle(const openhorizon::CarSpec& spec, float kmh, float input)
{
    openhorizon::FlatGround ground;
    openhorizon::CarPhysics car(spec);
    car.reset({0, 0, 0}, 0, ground);
    if (kmh > 0.0F) {
        accelerateTo(car, ground, kmh);
    }
    openhorizon::CarControls controls;
    controls.steer = input;
    drive(car, ground, controls, 1.0F);
    constexpr float kRadToDeg = 57.2957795F;
    return centreSteerAngle(car) * kRadToDeg;
}

TEST_CASE("fullSteerAtRestReachesFullLock", "[car]")
{
    const char* dir = gameDir();
    // Below 2 m/s (0x82000CE0) CCarDynamics' pad steering (0x82DF9620)
    // asks for the stick's share of the full lock, SteerMaxAngle.
    for (const char* media : {"ALF_8C_08", "HON_CivicR_04", "FOR_GT40_66"}) {
        const openhorizon::CarSpec spec = Disc::get(dir).car(media);
        CHECK_THAT(steadySteerAngle(spec, 0.0F, 1.0F), WithinAbs(spec.steerMaxAngleDeg, 1e-3));
        CHECK_THAT(steadySteerAngle(spec, 0.0F, -1.0F), WithinAbs(-spec.steerMaxAngleDeg, 1e-3));
        CHECK_THAT(steadySteerAngle(spec, 0.0F, 0.5F), WithinAbs(0.5F * spec.steerMaxAngleDeg, 1e-3));
    }
}

TEST_CASE("fullSteerLockFallsWithSpeed", "[car]")
{
    const char* dir = gameDir();
    // The speed-sensitive lock (0x82DE7E70) narrows towards
    // SteerSpeedSensitiveMinMaxAngle as speed rises, the same both ways.
    const openhorizon::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
    float previous = spec.steerMaxAngleDeg;
    for (const float kmh : {20.0F, 40.0F, 60.0F, 100.0F, 150.0F}) {
        const float right = steadySteerAngle(spec, kmh, 1.0F);
        CHECK_THAT(steadySteerAngle(spec, kmh, -1.0F), WithinAbs(-right, 1e-3));
        CHECK(right < previous);
        CHECK(right >= spec.steerSpeedSensitiveMinAngleDeg - 1e-3F);
        previous = right;
    }
}

TEST_CASE("nearlyFullSteerAtSpeedGetsTheFindPeakLock", "[car]")
{
    const char* dir = gameDir();
    // At 0.976 input or more (0x8224ACA0) the pad steering (0x82DF9620) adds lock up to the
    // front tyres' peak. A pad reporting 31000 at full travel shapes to 0.9945.
    const openhorizon::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
    const float full = steadySteerAngle(spec, 60.0F, 1.0F);
    CHECK(steadySteerAngle(spec, 60.0F, 0.9945F) > 0.99F * full);
    CHECK(steadySteerAngle(spec, 60.0F, 0.939F) < 0.8F * full);
}

/// How far a car turned after driving into a wall.
struct WallHit {
    bool hit = false;
    float totalYaw = 0.0F;
    float finalYawRate = 0.0F;
};

/// Drives `spec` at `kmh` into a concrete wall (the plane x = 0) at `angle`
/// radians from head-on, lets go on contact and measures its turning over `seconds`.
WallHit driveIntoWall(const openhorizon::CarSpec& spec, float kmh, float angle, float seconds)
{
    Disc& disc = Disc::get(gameDir());
    const openhorizon::CollisionSettings settings = openhorizon::loadCollisionSettings(disc.vfs);
    const openhorizon::collision::SurfaceTable surfaces = openhorizon::collision::SurfaceTable::load(disc.vfs);
    const openhorizon::collision::CategoryPair* concrete = surfaces.pair("Car", "HardWorld");
    CHECK(concrete != nullptr);
    const std::vector<openhorizon::CarSpec::CollisionSphere> spheres = openhorizon::worldContactSpheres(spec);

    openhorizon::FlatGround ground;
    openhorizon::CarPhysics car(spec);
    // Far enough back to reach the speed first.
    car.reset({-400.0F, 0.0F, 0.0F}, std::numbers::pi_v<float> * 0.5F - angle, ground);
    accelerateTo(car, ground, kmh);
    WallHit result;
    std::vector<openhorizon::WorldContact> contacts;
    float yaw = std::atan2(car.forward().x, car.forward().z);
    const int steps = static_cast<int>(seconds * static_cast<float>(kStepsPerSecond));
    openhorizon::CarControls controls;
    controls.throttle = 1.0F;
    for (int i = 0; i < kStepsPerSecond * 60 && (!result.hit || i < steps); ++i) {
        if (!result.hit) {
            controls.throttle = car.speed() * 3.6F < kmh ? 1.0F : 0.0F;
        }
        car.step(controls, ground);
        contacts.clear();
        const glm::mat4 transform = car.transform();
        for (const openhorizon::CarSpec::CollisionSphere& sphere : spheres) {
            const glm::vec3 centre
                = glm::vec3(transform * glm::vec4(sphere.centre[0], sphere.centre[1], sphere.centre[2], 1.0F));
            if (centre.x + sphere.radius > 0.0F) {
                openhorizon::WorldContact& contact = contacts.emplace_back();
                contact.point = {0.0F, centre.y, centre.z};
                contact.normal = {-1.0F, 0.0F, 0.0F};
                contact.depth = centre.x + sphere.radius;
                contact.friction = concrete ? concrete->friction : 0.5F;
                contact.lowSpeedFriction = concrete ? concrete->lowSpeedFriction.value_or(contact.friction) : 0.5F;
            }
        }
        if (!contacts.empty() && !result.hit) {
            result.hit = true;
            controls = {};
            i = 0;
        }
        car.resolveWorldContacts(contacts, settings);
        if (result.hit) {
            const float next = std::atan2(car.forward().x, car.forward().z);
            result.totalYaw += std::remainder(next - yaw, 2.0F * std::numbers::pi_v<float>);
            yaw = next;
        } else {
            yaw = std::atan2(car.forward().x, car.forward().z);
        }
    }
    result.finalYawRate = yawRate(car);
    return result;
}

TEST_CASE("wallImpactDoesNotSpinTheCar", "[car]")
{
    const char* dir = gameDir();
    const openhorizon::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
    constexpr float kDegree = std::numbers::pi_v<float> / 180.0F;
    for (const float angle : {0.0F, 40.0F * kDegree}) {
        for (const float kmh : {60.0F, 120.0F}) {
            const WallHit hit = driveIntoWall(spec, kmh, angle, 5.0F);
            std::fprintf(stderr, "%.0f degrees at %.0f km/h: turned %.2f rad, yaw rate %.2f rad/s at the end\n",
                angle / kDegree, kmh, hit.totalYaw, hit.finalYawRate);
            CHECK(hit.hit);
            CHECK(std::abs(hit.totalYaw) < std::numbers::pi_v<float>);
            CHECK(std::abs(hit.finalYawRate) < 0.5F);
        }
    }
}

/// How deep `car`'s contact spheres reach into `other`'s maxdata.xml
/// BoundingBox, metres.
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

TEST_CASE("carDoesNotSinkIntoOrLaunchATruck", "[car]")
{
    const char* dir = gameDir();
    Disc& disc = Disc::get(dir);
    const openhorizon::CollisionSettings settings = openhorizon::loadCollisionSettings(disc.vfs);
    const openhorizon::CarSpec carSpec = disc.car("ALF_8C_08");
    const openhorizon::CarSpec truckSpec = disc.car("KEN_T440_12");
    const std::vector<openhorizon::CarSpec::CollisionSphere> carSpheres = openhorizon::carContactSpheres(carSpec);
    const std::vector<openhorizon::CarSpec::CollisionSphere> truckSpheres = openhorizon::carContactSpheres(truckSpec);
    const float halfPi = std::numbers::pi_v<float> * 0.5F;
    struct Approach {
        const char* name;
        float truckHeading;
    };
    // The car drives east (+x) at the truck standing at the origin.
    const std::array<Approach, 3> approaches{{
        {"tail", halfPi},
        {"nose", -halfPi},
        {"side", 0.0F},
    }};
    for (const Approach& approach : approaches) {
        for (const float kmh : {30.0F, 60.0F}) {
            openhorizon::FlatGround ground;
            openhorizon::CarPhysics car(carSpec);
            openhorizon::CarPhysics truck(truckSpec);
            car.reset({-300.0F, 0.0F, 0.0F}, halfPi, ground);
            truck.reset({0.0F, 0.0F, 0.0F}, approach.truckHeading, ground);
            accelerateTo(car, ground, kmh);
            openhorizon::CarControls throttle;
            std::vector<openhorizon::CarContact> contacts;
            float speedAtImpact = 0.0F;
            float truckSpeed = 0.0F;
            float truckLowestUp = 1.0F;
            float deepest = 0.0F;
            for (int i = 0; i < kStepsPerSecond * 60; ++i) {
                throttle.throttle = speedAtImpact == 0.0F && car.speed() * 3.6F < kmh ? 1.0F : 0.0F;
                car.step(speedAtImpact == 0.0F ? throttle : openhorizon::CarControls{}, ground);
                truck.step({}, ground);
                contacts.clear();
                openhorizon::findCarContacts(car, carSpheres, openhorizon::carContactIsBox(carSpec), truck,
                    truckSpheres, openhorizon::carContactIsBox(truckSpec), contacts);
                if (!contacts.empty() && speedAtImpact == 0.0F) {
                    speedAtImpact = car.speed();
                    i = kStepsPerSecond * 57;
                }
                openhorizon::resolveCarContacts(car, truck, contacts, settings.playerTorqueScale,
                    settings.trafficTorqueScale, settings.carCarRestitution);
                truckSpeed = std::max(truckSpeed, truck.speed());
                truckLowestUp = std::min(truckLowestUp, truck.up().y);
                deepest = std::max(deepest, boxPenetration(car, truck));
            }
            // The fastest a fully elastic hit could push the truck.
            const float pushLimit = 2.0F * car.mass() * speedAtImpact / (car.mass() + truck.mass());
            std::fprintf(stderr,
                "%s at %.0f km/h: hit at %.1f m/s, truck up to %.2f m/s (limit %.2f), lowest up %.2f, %.2f m into "
                "the truck at most, %.2f m after 3 s\n",
                approach.name, kmh, speedAtImpact, truckSpeed, pushLimit, truckLowestUp, deepest,
                boxPenetration(car, truck));
            CHECK(speedAtImpact > 0.0F);
            CHECK(truckLowestUp > 0.7F);
            CHECK(truckSpeed <= pushLimit + 0.1F);
            // A bumper may reach into the shallow dips between neighbouring spheres.
            CHECK(deepest < 0.1F);
            CHECK(boxPenetration(car, truck) < 0.1F);
        }
    }
}

} // namespace
