// Car physics checks that need no world: the real cars from the disc driven
// on a flat or tilted plane, for the behaviours players notice first (a car
// holding still, recovering from a slide, steering the same both ways).
// They need the disc and skip without FH1_GAME_DIR; fh1cartest covers the
// performance figures.

#include "TestHarness.h"

#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

constexpr int kStepsPerSecond = 360;

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

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

TEST(carLeftAloneOnFlatGroundStaysStill)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::FlatGround ground;
    fh::CarPhysics car(Disc::get(dir).car("ALF_8C_08"));
    car.reset({0, 0, 0}, 0, ground);
    drive(car, ground, {}, 2.0F);
    const glm::vec3 start = car.position();
    drive(car, ground, {}, 30.0F);
    CHECK_NEAR(glm::length(car.position() - start), 0.0, 0.01);
}

TEST(handbrakeHoldsACarOnASlope)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
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

TEST(assistsRecoverFromPowerOversteer)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
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

TEST(steeringLockIsTheSameBothWays)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
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
    CHECK_NEAR(leftFl, -rightFr, 1e-4);
    CHECK_NEAR(leftFr, -rightFl, 1e-4);
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

TEST(fullSteerAtRestReachesFullLock)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    // Below 2 m/s (0x82000CE0) CCarDynamics' pad steering (0x82DF9620)
    // asks for the stick's share of the full lock, SteerMaxAngle.
    for (const char* media : {"ALF_8C_08", "HON_CivicR_04", "FOR_GT40_66"}) {
        const fh::CarSpec spec = Disc::get(dir).car(media);
        CHECK_NEAR(steadySteerAngle(spec, 0.0F, 1.0F), spec.steerMaxAngleDeg, 1e-3);
        CHECK_NEAR(steadySteerAngle(spec, 0.0F, -1.0F), -spec.steerMaxAngleDeg, 1e-3);
        CHECK_NEAR(steadySteerAngle(spec, 0.0F, 0.5F), 0.5F * spec.steerMaxAngleDeg, 1e-3);
    }
}

TEST(fullSteerLockFallsWithSpeed)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    // The speed-sensitive lock (0x82DE7E70) narrows towards
    // SteerSpeedSensitiveMinMaxAngle as speed rises, the same both ways.
    const fh::CarSpec spec = Disc::get(dir).car("ALF_8C_08");
    float previous = spec.steerMaxAngleDeg;
    for (const float kmh : {20.0F, 40.0F, 60.0F, 100.0F, 150.0F}) {
        const float right = steadySteerAngle(spec, kmh, 1.0F);
        CHECK_NEAR(steadySteerAngle(spec, kmh, -1.0F), -right, 1e-3);
        CHECK(right < previous);
        CHECK(right >= spec.steerSpeedSensitiveMinAngleDeg - 1e-3F);
        previous = right;
    }
}

TEST(nearlyFullSteerAtSpeedGetsTheFindPeakLock)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
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

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
