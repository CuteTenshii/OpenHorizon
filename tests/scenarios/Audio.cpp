// Scenarios of the game's sound: the race mix and the collision sounds of
// a car driven into a wall and into another car.

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

// A race drives under the Race mix (exterior_race before start_race in the
// race flows) and goes back to FreeRoam when it is left.
TEST_CASE_METHOD(GameScenario, "raceUsesTheRaceMix", "[scenario][audio]")
{
    openhorizon::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    CHECK(gameplayMix(session.game()) == openhorizon::audio::MixSnapshotId::Race);
}

/// The collision events started since `before` of them had been.
std::vector<openhorizon::audio::CollisionSounds::Started> collisionsSince(
    const openhorizon::GameplaySounds& sounds, std::size_t before)
{
    const auto& all = sounds.collisions().started();
    return {all.begin() + static_cast<std::ptrdiff_t>(std::min(before, all.size())), all.end()};
}

void printCollisions(const char* name, const std::vector<openhorizon::audio::CollisionSounds::Started>& started)
{
    for (const auto& event : started) {
        std::fprintf(stderr, "%s: %s at %.1f mph (car %.1f mph)%s\n", name, event.event.c_str(),
            event.relativeVelocityMph, event.carSpeedMph, event.aiOnly ? ", AI only" : "");
    }
}

// "Lots of sounds aren't rendered (like wrecking into another car ...)":
// driving into the building's wall of the scenario above plays the
// collision table's impact, its low thump and its scrape for the car's
// body against the wall's surface, at the speed of the hit; losing that
// much speed at once takes the mix to the Crash snapshot.
TEST_CASE_METHOD(GameScenario, "wallImpactPlaysTheCollisionSounds", "[scenario][audio]")
{
    using Slot = openhorizon::audio::CollisionSoundTable::Slot;
    const glm::vec2 target(-3898.0F, 1046.0F);
    openhorizon::GameOptions options;
    options.spawnAt = target - glm::vec2(68.0F, 0.0F);
    options.spawnHeading = std::numbers::pi_v<float> * 0.5F;
    options.startTime = 12.0F * 3600.0F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    openhorizon::Game& game = session.game();
    const openhorizon::GameplaySounds* sounds = game.gameplaySounds();
    CHECK(sounds != nullptr);
    if (!sounds) {
        return;
    }
    const std::size_t before = sounds->collisions().started().size();
    const openhorizon::CarPhysics& car = game.playerCar();
    float speedAtImpact = 0.0F;
    for (float t = 0.0F; t < 20.0F && speedAtImpact == 0.0F; t += kStep) {
        const float speed = car.forwardSpeed();
        session.run(kStep, {.throttle = speed < 100.0F / 3.6F ? 1.0F : 0.2F, .steer = steerTowards(car, target)});
        if (speed - car.forwardSpeed() > 3.0F) {
            speedAtImpact = speed;
        }
    }
    CHECK(speedAtImpact > 0.0F);
    const openhorizon::audio::SnapshotMixer* mix = game.mixSnapshots();
    CHECK((mix && mix->current(openhorizon::audio::MixGroup::Impact) == openhorizon::audio::MixSnapshotId::Crash));
    session.run(0.2F, {});
    session.capture("wall_impact_sounds");
    const auto started = collisionsSince(*sounds, before);
    printCollisions("wall impact", started);
    const auto impact = std::find_if(started.begin(), started.end(), [](const auto& event) {
        return event.slot == Slot::Impact3D && event.event.ends_with("/IMPACT_3D") && event.relativeVelocityMph > 30.0F;
    });
    CHECK(impact != started.end());
    if (impact != started.end()) {
        CHECK(!impact->aiOnly);
        CHECK(impact->relativeVelocityMph < speedAtImpact * 2.2369F + 1.0F);
        const auto thump = std::find_if(started.begin(), started.end(), [](const auto& event) {
            return event.slot == Slot::ImpactLFE && event.event == "Collisions/LFE/IMPACT_LFE";
        });
        CHECK(thump != started.end());
    }
}

// Driving into the traffic truck of the scenario above plays the impact of
// the two cars' body materials.
TEST_CASE_METHOD(GameScenario, "carImpactPlaysTheCollisionSounds", "[scenario][audio]")
{
    using Slot = openhorizon::audio::CollisionSoundTable::Slot;
    const glm::vec2 truckAt(-3920.0F, 1046.0F);
    const float halfPi = std::numbers::pi_v<float> * 0.5F;
    openhorizon::GameOptions options;
    options.spawnAt = truckAt - glm::vec2(48.0F, 0.0F);
    options.spawnHeading = halfPi;
    options.startTime = 12.0F * 3600.0F;
    options.parkedCar = "KEN_T440_12";
    options.parkedAt = truckAt;
    options.parkedHeading = halfPi;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    openhorizon::Game& game = session.game();
    const openhorizon::GameplaySounds* sounds = game.gameplaySounds();
    CHECK((sounds != nullptr && game.parkedCar() != nullptr));
    if (!sounds || !game.parkedCar()) {
        return;
    }
    const std::size_t before = sounds->collisions().started().size();
    const openhorizon::CarPhysics& car = game.playerCar();
    float speedAtImpact = 0.0F;
    for (float t = 0.0F; t < 20.0F && speedAtImpact == 0.0F; t += kStep) {
        const float speed = car.speed();
        session.run(kStep, {.throttle = speed < 40.0F / 3.6F ? 1.0F : 0.0F, .steer = steerTowards(car, truckAt)});
        if (speed - car.speed() > 1.5F) {
            speedAtImpact = speed;
        }
    }
    CHECK(speedAtImpact > 0.0F);
    session.run(0.2F, {});
    const auto started = collisionsSince(*sounds, before);
    printCollisions("truck impact", started);
    // The car-against-car groups are Collisions/<material>/<material>.
    const auto impact = std::find_if(started.begin(), started.end(), [](const auto& event) {
        return event.slot == Slot::Impact3D && event.event.starts_with("Collisions/Car")
            && event.event.find("/Car", 11) != std::string::npos && event.event.ends_with("/IMPACT_3D");
    });
    CHECK(impact != started.end());
    if (impact != started.end()) {
        CHECK(!impact->aiOnly);
        CHECK(impact->relativeVelocityMph > 10.0F);
        CHECK(impact->relativeVelocityMph < speedAtImpact * 2.2369F + 1.0F);
    }
}

} // namespace
} // namespace openhorizon::test
