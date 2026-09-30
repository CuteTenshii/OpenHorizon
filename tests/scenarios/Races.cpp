// Scenarios of races and event starts: the zone an event starts from, its
// flare, and the flares a race puts out.

#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>

#include "particles/ParticleManager.h"

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

namespace fh::test {
namespace {

// "Mannnnnn this is a recurring issue atp", about the name of an event
// start floating in a dark box the text ran out of. Nothing of the event
// start is made up: no beams, rings or labels in the plain overlay. The
// start shows the set dressing the game puts out for its zone (the world's
// FR04 props), and inside the zone's 25 m the HUD's activation prompt names
// the event with the zone's call to action, ENTER NOW.
TEST_CASE_METHOD(GameScenario, "eventStartShowsTheGamesZone", "[scenario][races]")
{
    fh::GameOptions options;
    options.spawnObject = "FR04";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(3.0F, {});
    fh::Game& game = session.game();
    session.capture("event_start_fr04");
    CHECK(game.overlayVerticesDrawn() == 0);
    CHECK(game.activityPropChunksDrawn() > 0);
    const fh::ui::HudState hud = game.currentHudState();
    CHECK(!hud.prompt.empty());
    CHECK(hud.promptDetail == std::string("ENTER NOW"));
    CHECK(hud.promptTitle.empty());
    // X is the zone's activation.
    session.press(fh::Button::X);
    session.run(0.5F, {});
    CHECK(game.race() != nullptr);
    CHECK(game.activityPropChunksDrawn() == 0);
}

/// The flare of `object`'s activation zone: whether it runs, and the
/// particles alive in the systems it runs in.
std::pair<bool, std::size_t> zoneFlare(const fh::Game& game, std::string_view object)
{
    const fh::particles::ParticleManager* particles = game.particleManager();
    if (particles == nullptr) {
        return {false, 0};
    }
    const fh::particles::SimpleEmitterState* flare = particles->simpleEmitter(std::string(object) + "_flare");
    if (flare == nullptr) {
        return {false, 0};
    }
    std::size_t live = 0;
    for (const fh::particles::ParticleSystem* system : particles->systemsOf(*flare)) {
        live += system->size();
    }
    return {flare->running, live};
}

// An event start burns the flare the game places by its zone
// (ParticleEmitters.xml's FR04_flare, a _Composite_Flare_6) while the car is
// within 500 m; a start farther away does not.
TEST_CASE_METHOD(GameScenario, "eventStartBurnsItsFlare", "[scenario][races]")
{
    fh::GameOptions options;
    // 60 m north of the FR04 zone, outside its 25 m prompt, facing it.
    options.spawnAt = glm::vec2(-1958.7F, 52.7F);
    options.spawnHeading = std::numbers::pi_v<float>;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(2.0F, {});
    fh::Game& game = session.game();
    session.capture("event_start_flare_fr04");
    const auto [running, live] = zoneFlare(game, "FR04");
    CHECK(running);
    CHECK(live > 0);
    // FR17's zone is over a kilometre away.
    CHECK(!zoneFlare(game, "FR17").first);
}

// In a race the free-roam zones are disabled, and their flares are out.
TEST_CASE_METHOD(GameScenario, "raceNoZoneFlareBurns", "[scenario][races]")
{
    fh::GameOptions options;
    options.race = "FR02";
    Session session(options);
    CHECK(session.waitForCar());
    session.run(1.0F, {});
    const fh::particles::ParticleManager* particles = session.game().particleManager();
    CHECK(particles != nullptr);
    if (particles != nullptr) {
        CHECK(std::none_of(particles->simpleEmitters().begin(), particles->simpleEmitters().end(),
            [](const fh::particles::SimpleEmitterState& e) { return e.running; }));
    }
}

} // namespace
} // namespace fh::test
