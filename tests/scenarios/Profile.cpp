// Scenarios of the player's progress: what a game saves to its profile and
// finds again after a restart or a quit.

#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>

#include "game/SpeedTraps.h"
#include "ui/Navigation.h"
#include "ui/PauseMenu.h"

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

// "Add a game progression system so stuff is saved": what the player found
// in free roam was gone after a restart. A speed trap's first pass is a new
// personal best, which makes the game save its profile ("Activity Speed
// Camera: New PB", 0x8296f76c), roads driven included; the next game loads
// it back.
TEST_CASE_METHOD(GameScenario, "progressIsKeptAcrossRestarts", "[scenario][profile]")
{
    const glm::vec2 gate{2274.2F, 319.6F};
    const glm::vec2 north = glm::normalize(glm::vec2(0.1167F, 0.9932F));
    openhorizon::GameOptions options;
    options.spawnAt = gate - north * 120.0F;
    options.spawnHeading = std::atan2(north.x, north.y);
    options.profileDir = freshProfileDir("restart");
    float best = 0.0F;
    std::vector<bool> driven;
    {
        Session session(options);
        CHECK(session.waitForCar());
        const openhorizon::SpeedTraps* traps = session.game().speedTraps();
        const openhorizon::SpeedTrap* trap = traps ? traps->find("speed_camera_01") : nullptr;
        CHECK(trap != nullptr);
        if (trap == nullptr) {
            return;
        }
        session.runUntil(20.0F, [&] { return trap->discovered; }, {.throttle = 1.0F});
        CHECK(trap->discovered);
        CHECK(session.game().profileSaves() == std::size_t{1});
        best = trap->best.value_or(0.0F);
        CHECK(best >= 30.0F * 0.44704F);
        // The save came at the end of that frame, with the roads driven up to
        // it; those driven after wait for the next save, as in the game.
        const openhorizon::ui::Navigation* navigation = session.game().navigation();
        CHECK(navigation != nullptr);
        if (navigation != nullptr) {
            driven = navigation->coverage().visitedNodes();
        }
        CHECK(std::count(driven.begin(), driven.end(), true) > 0);
        CHECK(std::filesystem::exists(options.profileDir / "ForzaProfile.bin"));
        session.run(0.5F, {});
    }
    Session session(options);
    CHECK(session.waitForCar());
    openhorizon::Game& game = session.game();
    const openhorizon::SpeedTrap* trap = game.speedTraps() ? game.speedTraps()->find("speed_camera_01") : nullptr;
    CHECK(trap != nullptr);
    CHECK((trap && trap->discovered));
    CHECK((trap && trap->best.value_or(0.0F) == best));
    CHECK(hasMarker(game.currentMapState(), "speed_camera_discovered"));
    const openhorizon::ui::Navigation* navigation = game.navigation();
    CHECK(navigation != nullptr);
    if (navigation != nullptr) {
        const std::vector<bool> now = navigation->coverage().visitedNodes();
        std::size_t kept = 0;
        for (std::size_t node = 0; node < driven.size() && node < now.size(); ++node) {
            kept += driven[node] && now[node] ? 1 : 0;
        }
        CHECK(kept == static_cast<std::size_t>(std::count(driven.begin(), driven.end(), true)));
    }
}

// The player chose that leaving the game keeps the roads driven since the
// last save, which the console game has no moment for: QUIT in the pause
// menu saves, and so does ending the game any other way.
TEST_CASE_METHOD(GameScenario, "quittingKeepsTheRoadsDriven", "[scenario][profile]")
{
    // Colorado's road_094, as in drivenRoadsColourTheMap, far from any
    // speed trap, outpost or discount sign.
    openhorizon::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    options.profileDir = freshProfileDir("quit");
    const auto drivenCount = [](const std::vector<bool>& nodes) {
        return static_cast<std::size_t>(std::count(nodes.begin(), nodes.end(), true));
    };
    const auto keeps = [](const std::vector<bool>& before, const std::vector<bool>& now) {
        for (std::size_t node = 0; node < before.size(); ++node) {
            if (before[node] && (node >= now.size() || !now[node])) {
                return false;
            }
        }
        return true;
    };
    const auto visited = [](Session& session) {
        const openhorizon::ui::Navigation* navigation = session.game().navigation();
        return navigation != nullptr ? navigation->coverage().visitedNodes() : std::vector<bool>{};
    };
    std::vector<bool> beforeQuit;
    {
        Session session(options);
        CHECK(session.waitForCar());
        session.run(4.0F, {.throttle = 0.7F});
        session.run(3.0F, {.brake = 1.0F});
        CHECK(session.game().profileSaves() == std::size_t{0});
        beforeQuit = visited(session);
        CHECK(drivenCount(beforeQuit) > 1);
        session.press(openhorizon::Button::Start);
        const openhorizon::ui::PauseMenu* menu = session.game().pauseMenu();
        CHECK(menu != nullptr);
        if (menu == nullptr) {
            return;
        }
        CHECK(session.focusPauseItem(openhorizon::ui::PauseItem::Quit));
        CHECK(session.pressMayQuit(openhorizon::Button::A));
        CHECK(menu->confirming());
        // EXIT SINGLE PLAYER, confirmed: the game quits, saving once.
        CHECK(!session.pressMayQuit(openhorizon::Button::A));
        CHECK(session.game().profileSaves() == std::size_t{1});
    }
    std::vector<bool> beforeClose;
    {
        Session session(options);
        CHECK(session.waitForCar());
        CHECK(keeps(beforeQuit, visited(session)));
        // Then further down the road, and the game simply ends (the window
        // closed).
        session.run(12.0F, {.throttle = 0.7F});
        CHECK(session.game().profileSaves() == std::size_t{0});
        beforeClose = visited(session);
        CHECK(drivenCount(beforeClose) > drivenCount(beforeQuit));
    }
    Session session(options);
    CHECK(session.waitForCar());
    CHECK(keeps(beforeClose, visited(session)));
}

} // namespace
} // namespace openhorizon::test
