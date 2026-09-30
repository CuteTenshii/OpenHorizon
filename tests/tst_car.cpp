// Car physics checks that need no world: the real cars from the disc driven
// on a flat or tilted plane, for the behaviours players notice first (a car
// holding still, recovering from a slide, steering the same both ways).
// They need the disc and skip without FH1_GAME_DIR; fh1cartest covers the
// performance figures.

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

using fh::test::gameDir;

namespace {

constexpr int kStepsPerSecond = 360;

/// The disc's database, loaded once for all tests.
struct Disc {
    fh::Vfs vfs;
    std::optional<fh::Database> db;

    static Disc& get(const char* dir)
    {
        static Disc disc;
        if (!disc.db) {
            disc.vfs.mountDisc(dir);
            disc.db.emplace(disc.vfs.readOrThrow("media/db/gamedb.slt"));
        }
        return disc;
    }

    fh::CarSpec car(const char* media) { return fh::loadCarSpec(*db, fh::carIdByMediaName(*db, media), &vfs); }
};

/// A plane through the origin rising `grade` metres per metre towards -z.
class Slope final : public fh::GroundQuery {
public:
    explicit Slope(float grade)
        : m_normal(glm::normalize(glm::vec3(0.0F, 1.0F, -grade)))
    {
    }

    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, fh::GroundHit& hit) const override
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

void drive(fh::CarPhysics& car, const fh::GroundQuery& ground, const fh::CarControls& controls, float seconds)
{
    const int steps = static_cast<int>(seconds * static_cast<float>(kStepsPerSecond));
    for (int i = 0; i < steps; ++i) {
        car.step(controls, ground);
    }
}

void accelerateTo(fh::CarPhysics& car, const fh::GroundQuery& ground, float kmh)
{
    fh::CarControls controls;
    controls.throttle = 1.0F;
    for (int i = 0; i < kStepsPerSecond * 60 && car.speed() * 3.6F < kmh; ++i) {
        car.step(controls, ground);
    }
}

float yawRate(const fh::CarPhysics& car)
{
    return glm::dot(car.angularVelocity(), car.up());
}

float bodySlip(const fh::CarPhysics& car)
{
    const glm::vec3 v = car.velocity();
    return std::atan2(glm::dot(v, car.right()), glm::dot(v, car.forward()));
}

TEST_CASE("carLeftAloneOnFlatGroundStaysStill", "[car]")
{
    const char* dir = gameDir();
    fh::FlatGround ground;
    fh::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
    car.reset({0, 0, 0}, 0, ground);
    drive(car, ground, {}, 2.0F);
    const glm::vec3 start = car.position();
    drive(car, ground, {}, 30.0F);
    CHECK_THAT(glm::length(car.position() - start), WithinAbs(0.0, 0.01));
}

TEST_CASE("handbrakeHoldsACarOnASlope", "[car]")
{
    const char* dir = gameDir();
    // The game holds a stopped car on its handbrake. The tyre model has no
    // static friction at walking speed, so it still creeps a little on a
    // steep slope; this bounds that creep.
    for (const float grade : {0.05F, 0.20F}) {
        const Slope ground(grade);
        fh::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
        car.reset({0, 0, 0}, 0, ground);
        fh::CarControls controls;
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
    // A second of full lock at full throttle, then the wheel straight: with
    // the default assists the car must come back in line, not keep turning.
    fh::FlatGround ground;
    fh::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
    car.reset({0, 0, 0}, 0, ground);
    accelerateTo(car, ground, 70.0F);
    fh::CarControls controls;
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
    fh::FlatGround ground;
    const fh::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
    const auto lockAt = [&](float steer) {
        fh::CarPhysics car(spec);
        car.reset({0, 0, 0}, 0, ground);
        fh::CarControls controls;
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

/// The steering angle at the middle of the front axle, from the two front
/// wheels' Ackermann angles (CarPhysics turns the inner wheel tighter
/// about the same turning centre): tan = 2 / (cot inner + cot outer).
float centreSteerAngle(const fh::CarPhysics& car)
{
    const float a = car.wheels()[0].steerAngle;
    const float b = car.wheels()[1].steerAngle;
    if (std::abs(a) < 1e-6F || std::abs(b) < 1e-6F) {
        return 0.0F;
    }
    return std::atan(2.0F / (1.0F / std::tan(a) + 1.0F / std::tan(b)));
}

/// Holds `input` on the stick for a second from a start at `kmh` and
/// returns the centre steering angle in degrees.
float steadySteerAngle(const fh::CarSpec& spec, float kmh, float input)
{
    fh::FlatGround ground;
    fh::CarPhysics car(spec);
    car.reset({0, 0, 0}, 0, ground);
    if (kmh > 0.0F) {
        accelerateTo(car, ground, kmh);
    }
    fh::CarControls controls;
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
        const fh::CarSpec spec = Disc::get(dir).car(media);
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
    const fh::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
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
    // With the stick at 0.976 of full input or more (0x8224ACA0) and the
    // wheels within one step's turn of the lock, the pad steering
    // (0x82DF9620) adds lock while the front tyres are short of their
    // peak; otherwise the car gets only the stick's share of the lock. A
    // pad reporting 31000 at full travel gives 0.9945 through the game's
    // shaping and gets that extra lock; the 0.939 the old 12 % per-axis
    // dead zone gave it does not, and steers markedly less at speed.
    const fh::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
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

/// Drives `spec` at `kmh` into a concrete wall (the plane x = 0, facing
/// -x) that it meets at `angle` radians from head-on, lets go of the
/// controls on contact and measures its turning over `seconds`. The body's
/// contacts are found as the game's are, from worldContactSpheres(), and
/// resolved by CarPhysics::resolveWorldContacts() after each step.
WallHit driveIntoWall(const fh::CarSpec& spec, float kmh, float angle, float seconds)
{
    Disc& disc = Disc::get(gameDir());
    const fh::CollisionSettings settings = fh::loadCollisionSettings(disc.vfs);
    const fh::collision::SurfaceTable surfaces = fh::collision::SurfaceTable::load(disc.vfs);
    const fh::collision::CategoryPair* concrete = surfaces.pair("Car", "HardWorld");
    CHECK(concrete != nullptr);
    const std::vector<fh::CarSpec::CollisionSphere> spheres = fh::worldContactSpheres(spec);

    fh::FlatGround ground;
    fh::CarPhysics car(spec);
    // Far enough back to reach the speed first.
    car.reset({-400.0F, 0.0F, 0.0F}, std::numbers::pi_v<float> * 0.5F - angle, ground);
    accelerateTo(car, ground, kmh);
    WallHit result;
    std::vector<fh::WorldContact> contacts;
    float yaw = std::atan2(car.forward().x, car.forward().z);
    const int steps = static_cast<int>(seconds * static_cast<float>(kStepsPerSecond));
    fh::CarControls controls;
    controls.throttle = 1.0F;
    for (int i = 0; i < kStepsPerSecond * 60 && (!result.hit || i < steps); ++i) {
        if (!result.hit) {
            // Keeps the speed on the way in, as a player holding the
            // throttle does, then lets go at the wall.
            controls.throttle = car.speed() * 3.6F < kmh ? 1.0F : 0.0F;
        }
        car.step(controls, ground);
        contacts.clear();
        const glm::mat4 transform = car.transform();
        for (const fh::CarSpec::CollisionSphere& sphere : spheres) {
            const glm::vec3 centre
                = glm::vec3(transform * glm::vec4(sphere.centre[0], sphere.centre[1], sphere.centre[2], 1.0F));
            if (centre.x + sphere.radius > 0.0F) {
                fh::WorldContact& contact = contacts.emplace_back();
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
    // A player drove into a wall and the car spun round about eight times.
    // Head-on and at 40 degrees, at town and open-road speeds, a car
    // glances off or stops against the wall; it does not turn round, and it
    // has stopped turning a few seconds later.
    const fh::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
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

/// How deep `car`'s contact spheres reach into `other`'s bounding box (its
/// maxdata.xml BoundingBox), metres: the overlap a player sees, whatever
/// shapes stand in for the bodies.
float boxPenetration(const fh::CarPhysics& car, const fh::CarPhysics& other)
{
    const glm::mat4 toOther = glm::inverse(other.transform()) * car.transform();
    const glm::vec3 lo(other.spec().boundsMin[0], other.spec().boundsMin[1], other.spec().boundsMin[2]);
    const glm::vec3 hi(other.spec().boundsMax[0], other.spec().boundsMax[1], other.spec().boundsMax[2]);
    float deepest = 0.0F;
    for (const fh::CarSpec::CollisionSphere& sphere : fh::carContactSpheres(car.spec())) {
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
    // Players drove into a traffic box truck (KEN_T440_12) and ended up
    // inside it, or sent it flying upside down. The default car drives at a
    // truck standing still, into its tail, its nose and its side, at 30 and
    // 60 km/h, with the contacts, torque scales and restitution the game
    // uses between the player and traffic.
    Disc& disc = Disc::get(dir);
    const fh::CollisionSettings settings = fh::loadCollisionSettings(disc.vfs);
    const fh::CarSpec carSpec = disc.car("ALF_8C_08");
    const fh::CarSpec truckSpec = disc.car("KEN_T440_12");
    const std::vector<fh::CarSpec::CollisionSphere> carSpheres = fh::carContactSpheres(carSpec);
    const std::vector<fh::CarSpec::CollisionSphere> truckSpheres = fh::carContactSpheres(truckSpec);
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
            fh::FlatGround ground;
            fh::CarPhysics car(carSpec);
            fh::CarPhysics truck(truckSpec);
            car.reset({-300.0F, 0.0F, 0.0F}, halfPi, ground);
            truck.reset({0.0F, 0.0F, 0.0F}, approach.truckHeading, ground);
            accelerateTo(car, ground, kmh);
            fh::CarControls throttle;
            std::vector<fh::CarContact> contacts;
            float speedAtImpact = 0.0F;
            float truckSpeed = 0.0F;
            float truckLowestUp = 1.0F;
            float deepest = 0.0F;
            for (int i = 0; i < kStepsPerSecond * 60; ++i) {
                throttle.throttle = speedAtImpact == 0.0F && car.speed() * 3.6F < kmh ? 1.0F : 0.0F;
                car.step(speedAtImpact == 0.0F ? throttle : fh::CarControls{}, ground);
                truck.step({}, ground);
                contacts.clear();
                fh::findCarContacts(car, carSpheres, fh::carContactIsBox(carSpec), truck, truckSpheres,
                    fh::carContactIsBox(truckSpec), contacts);
                if (!contacts.empty() && speedAtImpact == 0.0F) {
                    speedAtImpact = car.speed();
                    i = kStepsPerSecond * 57;
                }
                fh::resolveCarContacts(car, truck, contacts, settings.playerTorqueScale, settings.trafficTorqueScale,
                    settings.carCarRestitution);
                truckSpeed = std::max(truckSpeed, truck.speed());
                truckLowestUp = std::min(truckLowestUp, truck.up().y);
                deepest = std::max(deepest, boxPenetration(car, truck));
            }
            // Momentum: the truck cannot come away faster than the whole
            // car's momentum, shared by both, would push it even with a
            // fully elastic hit.
            const float pushLimit = 2.0F * car.mass() * speedAtImpact / (car.mass() + truck.mass());
            std::fprintf(stderr,
                "%s at %.0f km/h: hit at %.1f m/s, truck up to %.2f m/s (limit %.2f), lowest up %.2f, %.2f m into "
                "the truck at most, %.2f m after 3 s\n",
                approach.name, kmh, speedAtImpact, truckSpeed, pushLimit, truckLowestUp, deepest,
                boxPenetration(car, truck));
            CHECK(speedAtImpact > 0.0F);
            CHECK(truckLowestUp > 0.7F);
            CHECK(truckSpeed <= pushLimit + 0.1F);
            // The spheres standing in for the bodies leave shallow dips
            // between neighbours; a bumper may reach that far, no further.
            CHECK(deepest < 0.1F);
            CHECK(boxPenetration(car, truck) < 0.1F);
        }
    }
}

} // namespace
