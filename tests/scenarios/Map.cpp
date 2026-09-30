// Scenarios of the map and the sat-nav: opening the map, the roads it
// colours as they are driven, its cursor, routes to markers and to roads,
// the profile's player bar and the HUD's distance left.

#include "scenarios/Session.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "profile/Profile.h"
#include "ui/Hud.h"
#include "ui/Navigation.h"
#include "ui/Presentation.h"
#include "ui/WorldMap.h"

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

using Catch::Matchers::WithinAbs;

TEST_CASE_METHOD(GameScenario, "mapReplacesTheWorld", "[scenario][map]")
{
    Session session({});
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    CHECK(session.game().worldDrawn());

    const std::size_t sounds = session.game().uiSoundsPlayed().size();
    session.press(fh::Button::Back);
    CHECK(session.game().mapShown());
    session.run(1.0F, {});
    session.capture("map");
    CHECK(!session.game().worldDrawn());
    // The map pauses the game (pause_for_map) with the pause sound
    // (CWaitForPause) and plays its ambience loop.
    CHECK(gameplayMix(session.game()) == fh::audio::MixSnapshotId::Paused);
    CHECK(session.game().uiSoundPlaying("Map_AmbientBackgroundLoop"));

    // B is the map's EXIT MAP.
    session.press(fh::Button::B);
    session.run(0.2F, {});
    CHECK(!session.game().mapShown());
    CHECK(session.game().worldDrawn());
    CHECK(!session.game().uiSoundPlaying("Map_AmbientBackgroundLoop"));
    CHECK((playedSince(session.game(), sounds)
        == std::vector<std::string>{"Pause", "Map_AmbientBackgroundLoop", "Cancel"}));
    session.run(1.0F, {});
    CHECK(gameplayMix(session.game()) == fh::audio::MixSnapshotId::FreeRoam);
}

// "Map road colours are the same (normally they're different depending on
// the road type)." The map's road lines lerp from their grey primary colour
// to their road type's secondary colour where the player has driven
// (CMapCoverage's visit counts through CMapEntityWay's colour source), so
// driving a stretch of road colours it.
TEST_CASE_METHOD(GameScenario, "drivenRoadsColourTheMap", "[scenario][map]")
{
    // On colorado's way 33, the b road "road_094", at its first node and
    // facing its second, 20 m to the south.
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    CHECK(navigation != nullptr);
    if (navigation == nullptr) {
        return;
    }
    const fh::ui::NavFile& network = navigation->network();
    const auto drivenNodes = [&] {
        std::size_t count = 0;
        for (std::uint32_t node = 0; node < network.nodes.size(); ++node) {
            count += navigation->coverage().visits(node) > 0 ? 1 : 0;
        }
        return count;
    };
    CHECK(navigation->playerNode().has_value());
    const std::size_t atStart = drivenNodes();
    CHECK(atStart > 0);

    session.run(10.0F, {.throttle = 0.7F});
    CHECK(drivenNodes() > atStart);
    const std::optional<std::uint32_t> node = navigation->playerNode();
    CHECK(node.has_value());
    if (node) {
        bool coloured = false;
        for (const fh::ui::NavLink& link : network.nodes[*node].links) {
            const std::vector<float> ratios = navigation->coverage().wayRatios(link.way);
            coloured = coloured || std::find(ratios.begin(), ratios.end(), 1.0F) != ratios.end();
        }
        CHECK(coloured);
    }
    session.press(fh::Button::Back);
    session.run(3.0F, {.zoom = 1.0F});
    session.capture("map_driven");
}

/// The width the map overlay (0x828b6998) scales its cursor to: the
/// crosshair's 128 units as authored (a 100-unit rectangle at scale 1.28)
/// times cursorSize * (0.75 + 0.25 * distance / cursorSizeDistance) /
/// distance * viewport width / cursorAnarkSize, the viewport being the
/// scene's 1280-unit wide MODEL_MAP_VIEWPORT.
float expectedCursorWidth(const fh::ui::WorldMapTuning& tuning, float distance)
{
    const float grow = 0.75F + 0.25F * distance / tuning.cursorSizeDistance;
    return 128.0F * tuning.cursorSize * grow / distance * 1280.0F / tuning.cursorAnarkSize;
}

// "The map cursor is too big." The game scales the cursor with the map
// camera's distance every frame; drawn at its authored size it covered
// about a tenth of the screen.
TEST_CASE_METHOD(GameScenario, "mapCursorKeepsTheGameSize", "[scenario][map]")
{
    const fh::ui::WorldMapTuning tuning = fh::ui::WorldMapTuning::load(disc());
    Session session({});
    CHECK(session.waitForCar());
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK(map != nullptr);
    if (map == nullptr) {
        return;
    }
    CHECK_THAT(map->cursorExtent().x, WithinAbs(expectedCursorWidth(tuning, map->camera().distance()), 0.5));
    session.capture("map_cursor");
    // Zoomed in, the camera is nearer and the cursor grows.
    session.run(2.0F, {.zoom = 1.0F});
    CHECK_THAT(map->cursorExtent().x, WithinAbs(expectedCursorWidth(tuning, map->camera().distance()), 0.5));
    session.capture("map_cursor_zoomed");
}

// "Selecting a route on the map doesn't show its itinerary." SET ROUTE on
// a marker gives the sat-nav the waypoint, which searches the roads from
// the car's node to the node nearest the waypoint; the map draws that
// route, and it stays while the car drives.
TEST_CASE_METHOD(GameScenario, "setRouteShowsTheItinerary", "[scenario][map]")
{
    // On colorado's way 33, the b road "road_094", facing along it.
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK((navigation != nullptr && map != nullptr));
    if (navigation == nullptr || map == nullptr) {
        return;
    }
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    // The cursor goes east until it rests on a marker at least 500 m from
    // the car.
    const glm::vec2 car = session.game().currentMapState().playerPosition;
    const auto onFarMarker = [&] {
        const fh::ui::WorldMapMarker* marker = map->hovered();
        return marker != nullptr && glm::length(marker->marker.position - car) > 500.0F;
    };
    for (int frame = 0; frame < 1200 && !onFarMarker(); ++frame) {
        session.run(kStep, {.steer = 0.25F});
    }
    CHECK(onFarMarker());
    session.capture("route_before");
    session.press(fh::Button::A);
    CHECK(map->waypoint().has_value());
    CHECK(navigation->destination().has_value());
    const std::vector<glm::vec2> route = navigation->route();
    CHECK(route.size() > 2);
    if (route.empty() || !map->waypoint() || !navigation->playerNode()) {
        return;
    }
    // From the car's node...
    const glm::vec3 start = navigation->network().nodes[*navigation->playerNode()].position;
    CHECK(glm::length(route.front() - glm::vec2(start.x, start.z)) < 1e-3F);
    CHECK(glm::length(route.front() - car) < 100.0F);
    // ...to the node nearest the waypoint.
    const std::optional<std::uint32_t> goal
        = navigation->locator().nearestOnGround(*map->waypoint(), fh::ui::NavFilter::NoShortcutOrNone);
    CHECK((goal.has_value() && navigation->destination() == goal));
    if (goal) {
        const glm::vec3 end = navigation->network().nodes[*goal].position;
        CHECK(glm::length(route.back() - glm::vec2(end.x, end.z)) < 1e-3F);
    }
    // The map shows it, drawing in, then whole.
    CHECK(session.game().currentMapState().route.size() == route.size());
    CHECK(map->routeReveal() < 1.0F);
    session.run(2.0F, {});
    CHECK_THAT(map->routeReveal(), WithinAbs(1.0, 1e-6));
    session.capture("route_map");

    // Driving on, the route stays and the car's place moves along it.
    session.press(fh::Button::B);
    session.run(6.0F, {.throttle = 0.6F});
    CHECK(navigation->destination().has_value());
    CHECK(!navigation->routeNodes().empty());
    session.capture("route_minimap");
}

// "On the map you can't select a destination somewhere on the road. Only
// events." With the cursor on no marker, A hands the sat-nav the point of
// the roads nearest the cursor (the map overlay's update, 0x828b6998), and
// the route goes to that point's node; A again on it clears the route.
TEST_CASE_METHOD(GameScenario, "setRouteOnARoadAwayFromMarkers", "[scenario][map]")
{
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK((navigation != nullptr && map != nullptr));
    if (navigation == nullptr || map == nullptr) {
        return;
    }
    const std::vector<fh::ui::MapMarker> activities = fh::ui::loadActivityMarkers(disc(), "colorado");
    session.press(fh::Button::Back);
    session.run(1.0F, {});

    // The cursor goes west until it is 800 m from the car with no marker
    // within 400 m of it.
    const glm::vec2 car = session.game().currentMapState().playerPosition;
    const auto clearOfMarkers = [&] {
        const glm::vec2 at = map->camera().cursor();
        const auto near = [&](glm::vec2 marker) { return glm::length(marker - at) < 400.0F; };
        for (const fh::ui::MapMarker& marker : activities) {
            if (near(marker.position)) {
                return false;
            }
        }
        for (const fh::ui::WorldMapMarker& marker : session.game().currentMapState().markers) {
            if (near(marker.marker.position)) {
                return false;
            }
        }
        return glm::length(at - car) > 800.0F;
    };
    for (int frame = 0; frame < 1800 && !clearOfMarkers(); ++frame) {
        session.run(kStep, {.steer = -0.25F});
    }
    session.run(0.5F, {});
    CHECK(clearOfMarkers());
    CHECK(map->hovered() == nullptr);
    const std::optional<glm::vec2> target = map->target();
    CHECK(target.has_value());
    if (!target) {
        return;
    }
    // The target is the pick of the profile's road lines (a, b, dirt and
    // freeway ways, SnapSize 60 each) nearest the cursor.
    const fh::ui::NavFile& nav = navigation->network();
    std::optional<fh::ui::MapLinePick> best;
    for (const fh::ui::NavWay& way : nav.ways) {
        const std::string_view type = way.tag("road_type");
        if (type != "a" && type != "b" && type != "dirt" && type != "freeway") {
            continue;
        }
        const std::optional<fh::ui::MapLinePick> pick = fh::ui::pickMapLine(way.points, map->camera().cursor(), 60.0F);
        if (pick && (!best || pick->distance < best->distance)) {
            best = pick;
        }
    }
    CHECK(best.has_value());
    if (best) {
        CHECK(glm::length(best->point - *target) < 1e-3F);
    }
    session.capture("route_road_before");

    session.press(fh::Button::A);
    CHECK(map->waypoint().has_value());
    CHECK(navigation->destination().has_value());
    const std::optional<std::uint32_t> goal
        = navigation->locator().nearestOnGround(*target, fh::ui::NavFilter::NoShortcutOrNone);
    CHECK((goal.has_value() && navigation->destination() == goal));
    CHECK(map->targetNode() == goal);
    const std::vector<glm::vec2> route = navigation->route();
    CHECK(route.size() > 2);
    if (goal && !route.empty()) {
        const glm::vec3 end = nav.nodes[*goal].position;
        CHECK(glm::length(route.back() - glm::vec2(end.x, end.z)) < 1e-3F);
    }
    CHECK(session.game().currentMapState().route.size() == route.size());
    session.run(2.0F, {});
    session.capture("route_road_map");

    // A on the destination's node clears it (CLEAR ROUTE).
    session.press(fh::Button::A);
    CHECK(!map->waypoint().has_value());
    CHECK(!navigation->destination().has_value());
    CHECK(navigation->routeNodes().empty());
}

// A text the scene draws now, with some opacity.
bool drawsText(const fh::ui::Presentation& scene, std::string_view text)
{
    const std::vector<fh::ui::DrawItem> items = scene.evaluate();
    return std::any_of(items.begin(), items.end(), [&](const fh::ui::DrawItem& item) {
        return item.isText && item.text.text == text && item.text.color.a > 0.0F;
    });
}

// "The map doesn't have the profile elements such as wristband, credits."
// h_map's PLAYER_BAR shows the profile's Main/Credits, the points from
// Main/XP to the band after Main/WristbandLevel, and the car.
TEST_CASE_METHOD(GameScenario, "mapShowsTheProfilesPlayerBar", "[scenario][map]")
{
    fh::GameOptions options;
    options.profileDir = freshProfileDir("playerbar");
    saveProfile(options.profileDir, [](fh::profile::Profile& profile) {
        CHECK(profile.set("Main/Credits", std::uint64_t{1234567}));
        CHECK(profile.set("Main/XP", std::int64_t{500}));
        CHECK(profile.set("Main/WristbandLevel", std::int64_t{1}));
    });
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    const fh::ui::WorldMap* map = session.game().worldMap();
    CHECK((map != nullptr && session.game().mapShown()));
    if (map == nullptr) {
        return;
    }
    const fh::ui::Presentation& scene = map->scene();
    const fh::ui::PlayerBar& bar = map->playerBar();
    const auto textOf = [&](int element) { return element >= 0 ? scene.text(element) : std::string(); };
    CHECK(bar.bound());
    CHECK(textOf(bar.creditsText()) == std::string("1,234,567 Cr"));
    // The band after the green one (Id 1) needs 750 XP.
    CHECK(textOf(bar.wristbandText()) == std::string("250 PTS"));
    CHECK(textOf(bar.nextLevelText()) == std::string("Nxt Lvl"));
    CHECK(textOf(bar.carText()) == std::string("Alfa Romeo 8C Competizione"));
    CHECK(drawsText(scene, "1,234,567 Cr"));
    CHECK(drawsText(scene, "250 PTS"));
    session.capture("map_player_bar");
}

// "When a direction is selected, the number of km is supposed to be
// shown." The HUD's sat-nav readout shows the planner's distance left
// (0x8266cf20 reads its +624) in kilometres with one decimal, and it goes
// down as the car drives along the route.
TEST_CASE_METHOD(GameScenario, "satNavShowsTheDistanceLeft", "[scenario][map]")
{
    fh::GameOptions options;
    options.spawnAt = glm::vec2(-3748.15F, 1382.54F);
    options.spawnHeading = 3.14159265F;
    options.metric = true;
    Session session(options);
    CHECK(session.waitForCar());
    session.run(0.5F, {});
    const fh::ui::Navigation* navigation = session.game().navigation();
    const fh::ui::WorldMap* map = session.game().worldMap();
    const fh::ui::Hud* hud = session.game().hud();
    CHECK((navigation != nullptr && map != nullptr && hud != nullptr));
    if (navigation == nullptr || map == nullptr || hud == nullptr) {
        return;
    }
    CHECK(!session.game().currentHudState().destinationDistance.has_value());
    // The car faces south along its road: the destination is that road
    // 800 m to the south, so the route runs ahead of the car.
    session.press(fh::Button::Back);
    session.run(1.0F, {});
    const glm::vec2 car = session.game().currentMapState().playerPosition;
    for (int frame = 0; frame < 1200 && car.y - map->camera().cursor().y < 800.0F; ++frame) {
        session.run(kStep, {.cursorY = -0.25F});
    }
    session.run(0.5F, {});
    CHECK(map->hovered() == nullptr);
    session.press(fh::Button::A);
    session.press(fh::Button::B);
    session.run(0.5F, {});
    CHECK(navigation->destination().has_value());

    // What is left: from the car to the route's node after its place, then
    // along the route.
    const auto left = [&] {
        const std::vector<std::uint32_t>& route = navigation->routeNodes();
        const std::size_t next = navigation->routeProgress() + 1;
        if (next >= route.size()) {
            return 0.0F;
        }
        const fh::ui::NavFile& nav = navigation->network();
        float distance = glm::length(session.game().playerCar().position() - nav.nodes[route[next]].position);
        for (std::size_t i = next + 1; i < route.size(); ++i) {
            distance += glm::length(nav.nodes[route[i]].position - nav.nodes[route[i - 1]].position);
        }
        return distance;
    };
    const auto shown = [&] {
        return std::format("{:.1f} KM", session.game().currentHudState().destinationDistance.value_or(0.0F) / 1000.0F);
    };
    const std::optional<float> first = session.game().currentHudState().destinationDistance;
    CHECK(first.has_value());
    if (!first) {
        return;
    }
    CHECK_THAT(*first, WithinAbs(left(), 1.0));
    CHECK(hud->satNavDistanceText() == shown());
    session.capture("satnav_distance");

    // Driving on along the route until the car's place moves along it,
    // then stopping there: the readout counts down with the car.
    for (int frame = 0; frame < 1200 && navigation->routeProgress() < 2; ++frame) {
        session.run(kStep, {.throttle = 0.5F});
    }
    CHECK(navigation->routeProgress() >= 2);
    session.run(3.0F, {.brake = 1.0F});
    CHECK(navigation->destination().has_value());
    const std::optional<float> later = session.game().currentHudState().destinationDistance;
    CHECK(later.has_value());
    if (later) {
        CHECK(*later < *first - 20.0F);
        CHECK_THAT(*later, WithinAbs(left(), 1.0));
    }
    CHECK(hud->satNavDistanceText() == shown());
    session.capture("satnav_distance_driven");
}

} // namespace
} // namespace fh::test
