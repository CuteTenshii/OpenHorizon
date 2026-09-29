// Runs each car through the game's own performance tests on flat ground and
// compares the results with the figures the game recorded in gamedb
// (Data_Car.Sim*), to check the car model against the original.

#include "base/Log.h"
#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr float kMph60 = 26.8224F;
constexpr float kMph100 = 44.704F;
constexpr float kQuarterMile = 402.336F;
constexpr float kStep = 1.0F / fh::CarPhysics::kStepRate;

struct Result {
    float to60 = NAN;
    float to100 = NAN;
    float quarterTime = NAN;
    float quarterSpeed = NAN;
    float topSpeed = NAN;
    float brake60 = NAN;
    float lateralG = NAN;
};

/// The assists the game's own tests ran with: the test car set-up at
/// 0x82DFD810 turns ABS, traction control and stability management on, and
/// the test steps mark the car scripted (+0x1828), which skips traction
/// control (0x82DF8630).
void assistsLikeGameTests(fh::CarPhysics& car)
{
    car.assists().abs = true;
    car.assists().tractionControl = false;
    car.assists().stabilityControl = true;
}

void accelerationRun(const fh::CarSpec& spec, Result& result)
{
    const fh::FlatGround ground;
    fh::CarPhysics car(spec);
    assistsLikeGameTests(car);
    car.reset({0.0F, 0.0F, 0.0F}, 0.0F, ground);
    fh::CarControls idle;
    for (int i = 0; i < 360; ++i) {
        car.step(idle, ground);
    }
    const glm::vec3 start = car.position();
    fh::CarControls controls;
    controls.throttle = 1.0F;
    float time = 0.0F;
    float lastSpeed = 0.0F;
    float stalledFor = 0.0F;
    while (time < 180.0F) {
        car.step(controls, ground);
        time += kStep;
        const float speed = car.forwardSpeed();
        const float distance = glm::length(car.position() - start);
        if (std::isnan(result.to60) && speed >= kMph60) {
            result.to60 = time;
        }
        if (std::isnan(result.to100) && speed >= kMph100) {
            result.to100 = time;
        }
        if (std::isnan(result.quarterTime) && distance >= kQuarterMile) {
            result.quarterTime = time;
            result.quarterSpeed = speed;
        }
        // Top speed: acceleration below 0.02 m/s² for two seconds.
        if ((speed - lastSpeed) / kStep < 0.02F) {
            stalledFor += kStep;
            if (stalledFor > 2.0F && time > 20.0F) {
                break;
            }
        } else {
            stalledFor = 0.0F;
        }
        lastSpeed = speed;
    }
    result.topSpeed = car.forwardSpeed();
}

void brakingRun(const fh::CarSpec& spec, Result& result)
{
    const fh::FlatGround ground;
    fh::CarPhysics car(spec);
    assistsLikeGameTests(car);
    car.reset({0.0F, 0.0F, 0.0F}, 0.0F, ground);
    fh::CarControls controls;
    controls.throttle = 1.0F;
    float time = 0.0F;
    while (car.forwardSpeed() < kMph60 && time < 60.0F) {
        car.step(controls, ground);
        time += kStep;
    }
    const glm::vec3 start = car.position();
    controls.throttle = 0.0F;
    controls.brake = 1.0F;
    while (car.speed() > 0.05F && time < 120.0F) {
        car.step(controls, ground);
        time += kStep;
    }
    result.brake60 = glm::length(car.position() - start);
}

/// Holds 60 mph on a widening circle and records the highest lateral
/// acceleration the car sustains for a second.
void skidpadRun(const fh::CarSpec& spec, Result& result)
{
    const fh::FlatGround ground;
    fh::CarPhysics car(spec);
    assistsLikeGameTests(car);
    car.assists().speedSensitiveSteering = false;
    car.reset({0.0F, 0.0F, 0.0F}, 0.0F, ground);
    fh::CarControls controls;
    float time = 0.0F;
    while (car.forwardSpeed() < kMph60 && time < 60.0F) {
        controls.throttle = 1.0F;
        car.step(controls, ground);
        time += kStep;
    }
    float best = 0.0F;
    float windowSum = 0.0F;
    int windowCount = 0;
    const int windowSteps = static_cast<int>(fh::CarPhysics::kStepRate);
    for (float steer = 0.05F; steer <= 1.0F; steer += 0.025F) {
        // Settle at this steering input, then average one second.
        for (int i = 0; i < windowSteps * 3; ++i) {
            const float speedError = kMph60 - car.forwardSpeed();
            controls.throttle = std::clamp(speedError * 0.5F + 0.3F, 0.0F, 1.0F);
            controls.brake = speedError < -1.0F ? 0.2F : 0.0F;
            controls.steer = steer;
            car.step(controls, ground);
            if (i >= windowSteps * 2) {
                const glm::vec3 v = car.velocity();
                const float yawRate = glm::dot(car.angularVelocity(), glm::vec3(0.0F, 1.0F, 0.0F));
                windowSum += std::abs(yawRate * glm::length(glm::vec2(v.x, v.z)));
                ++windowCount;
            }
        }
        if (windowCount > 0) {
            const float speedNow = car.forwardSpeed();
            const float lateral = windowSum / static_cast<float>(windowCount) / 9.80665F;
            if (speedNow > kMph60 * 0.9F) {
                best = std::max(best, lateral);
            }
        }
        windowSum = 0.0F;
        windowCount = 0;
    }
    result.lateralG = best;
}

float relativeError(float ours, float reference)
{
    if (std::isnan(ours) || reference <= 0.0F) {
        return NAN;
    }
    return (ours - reference) / reference;
}

void trace(const fh::CarSpec& spec)
{
    const fh::FlatGround ground;
    fh::CarPhysics car(spec);
    car.reset({0.0F, 0.0F, 0.0F}, 0.0F, ground);
    fh::CarControls controls;
    for (int i = 0; i < 360 * 12; ++i) {
        controls.throttle = i >= 360 ? 1.0F : 0.0F;
        car.step(controls, ground);
        if (i % 18 == 0 && i >= 340 && i < 1400) {
            const auto& w = car.wheels();
            std::printf("t %5.2f v %6.2f y %6.3f rpm %6.0f gear %d | load %5.0f %5.0f %5.0f %5.0f | spin %6.1f %6.1f "
                        "| slip %6.3f %6.3f | fx %6.0f %6.0f | drive %6.0f\n",
                static_cast<float>(i) * kStep, car.forwardSpeed(), car.position().y, car.engineRpm(), car.gear(),
                w[0].load, w[1].load, w[2].load, w[3].load, w[0].spin, w[2].spin, w[0].slipRatio, w[2].slipRatio,
                w[0].longitudinalForce, w[2].longitudinalForce, w[2].driveTorque);
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: fh1cartest DISC CAR_ID|MEDIA_NAME|all\n");
        return 2;
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const fh::Database db(vfs.readOrThrow("media/db/gamedb.slt"));
        std::vector<int> ids;
        const std::string which = argv[2];
        if (argc > 3 && std::string(argv[3]) == "--trace") {
            int id = std::atoi(which.c_str());
            if (id == 0) {
                id = fh::carIdByMediaName(db, which);
            }
            trace(fh::loadCarSpec(db, id, &vfs));
            return 0;
        }
        if (which == "all") {
            ids = fh::drivableCarIds(db);
        } else {
            int id = std::atoi(which.c_str());
            if (id == 0) {
                id = fh::carIdByMediaName(db, which);
            }
            ids.push_back(id);
        }
        std::printf("%-28s %13s %13s %13s %13s %13s %13s\n", "car", "0-60 s", "0-100 s", "1/4 mi s", "top m/s",
            "60-0 m", "lat g");
        double sums[6] = {};
        int counts[6] = {};
        for (int id : ids) {
            fh::CarSpec spec = fh::loadCarSpec(db, id, &vfs);
            // The game's recorded figures leave out its in-game power trim.
            spec.gameTorqueScale = 1.0F;
            Result r;
            accelerationRun(spec, r);
            brakingRun(spec, r);
            skidpadRun(spec, r);
            const float ours[6] = {r.to60, r.to100, r.quarterTime, r.topSpeed, r.brake60, r.lateralG};
            const float theirs[6] = {spec.reference.timeTo60Mph, spec.reference.timeTo100Mph,
                spec.reference.quarterMileTime, spec.reference.topSpeed, spec.reference.brakeDistance60Mph,
                spec.reference.lateralG60Mph};
            std::printf("%-28s", spec.mediaName.c_str());
            for (int k = 0; k < 6; ++k) {
                std::printf(" %6.2f/%6.2f", ours[k], theirs[k]);
                const float e = relativeError(ours[k], theirs[k]);
                if (!std::isnan(e)) {
                    sums[k] += std::abs(e);
                    ++counts[k];
                }
            }
            std::printf("\n");
            std::fflush(stdout);
        }
        std::printf("mean absolute error:");
        const char* names[6] = {"0-60", "0-100", "1/4", "top", "brake", "lat"};
        for (int k = 0; k < 6; ++k) {
            std::printf(" %s %.1f%%", names[k], counts[k] ? 100.0 * sums[k] / counts[k] : 0.0);
        }
        std::printf("\n");
        return 0;
    } catch (const std::exception& e) {
        fh::logError("{}", e.what());
        return 1;
    }
}
