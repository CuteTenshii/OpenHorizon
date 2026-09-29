// The game mode's activities: the catalogue read from the disc, and the
// rules of the speed traps, the Horizon outposts and the set dressing of
// activation zones, on synthetic data and on Colorado when FH1_GAME_DIR
// names the disc.

#include "TestHarness.h"

#include "game/Activities.h"
#include "game/ActivityProps.h"
#include "game/GameObjects.h"
#include "game/Outposts.h"
#include "game/SpeedTraps.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
}

fh::GameObject object(const std::string& id, glm::vec3 position)
{
    fh::GameObject o;
    o.id = id;
    o.position = position;
    return o;
}

constexpr const char* kSpeedCamera = R"(<ActivityManager>
    <Activity type="ActivitySpeedCamera" name="speed_camera_01" scoreboard_id="201">
        <SpeedThreshold minimumSpeed="30.0" />
    </Activity>
    <!--Activity type="ActivitySpeedCamera" name="speed_camera_02" scoreboard_id="202" /-->
</ActivityManager>)";

constexpr const char* kGasStation = R"(<ActivityManager>
  <Activity type="ActivityGasStation" name="GasStation_01">
    <TriggerZone object="GASSTATION_001" radius="54" promptTitle="IDS_Description_GasStation"
        prompt="IDS_GasStationName_01" maxMPH="100" mapDescription="IDS_GasStationName_01"
        triggerZoneRadius="175" tutorialRadius="175" is_activation="false"/>
    <Missions>
      <Mission name="speedstunt_01" />
      <Mission name="photoshoot_01" />
    </Missions>
  </Activity>
</ActivityManager>)";

TEST(zone_attributes_parse_as_the_game_reads_them)
{
    const std::vector<fh::Activity> activities = fh::ActivityCatalogue::parse(kGasStation, "gas_stations.xml");
    CHECK_EQ(activities.size(), std::size_t{1});
    const fh::ActivityZone& zone = activities.front().zones.front();
    CHECK_EQ(zone.object, std::string("GASSTATION_001"));
    CHECK_NEAR(zone.radius, 54.0, 1e-6);
    CHECK(zone.maxSpeed.has_value());
    CHECK_NEAR(zone.maxSpeed.value_or(0.0F), 100.0 * 0.44704, 1e-4);
    CHECK(!zone.isActivation);
    CHECK(zone.requiresInput);
    CHECK_NEAR(zone.triggerZoneRadius.value_or(0.0F), 175.0, 1e-6);
    CHECK_EQ(activities.front().missions.size(), std::size_t{2});
}

TEST(commented_out_activities_are_skipped)
{
    const std::vector<fh::Activity> activities = fh::ActivityCatalogue::parse(kSpeedCamera, "speed_camera.xml");
    CHECK_EQ(activities.size(), std::size_t{1});
    CHECK_EQ(activities.front().scoreboardId, 201);
    CHECK_NEAR(activities.front().minimumSpeedMph.value_or(0.0F), 30.0, 1e-6);
}

/// A speed trap across the Z axis at the origin, its posts 20 m apart.
fh::SpeedTraps oneTrap()
{
    const std::vector<fh::Activity> activities = fh::ActivityCatalogue::parse(kSpeedCamera, "speed_camera.xml");
    const fh::GameObjects objects = fh::GameObjects::fromObjects({
        object("speed_camera_01_left", {-10.0F, 0.0F, 0.0F}),
        object("speed_camera_01_right", {10.0F, 0.0F, 0.0F}),
    });
    return fh::SpeedTraps::build(fh::ActivityCatalogue::fromActivities(activities), objects);
}

TEST(a_speed_trap_counts_a_pass_at_its_minimum_speed_or_more)
{
    fh::SpeedTraps traps = oneTrap();
    CHECK_EQ(traps.traps().size(), std::size_t{1});
    // 30 mph is 13.41 m/s: just below does not count and discovers nothing.
    CHECK(!traps.update({0.0F, 0.0F, -0.1F}, {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 13.3F}));
    CHECK(!traps.traps().front().discovered);
    const auto pass = traps.update({0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, -0.1F}, {0.0F, 0.0F, -40.0F});
    CHECK(pass.has_value());
    CHECK_NEAR(pass->speed, 40.0, 1e-4);
    CHECK_NEAR(pass->previousBest, 0.0, 1e-6);
    CHECK(traps.traps().front().discovered);
    // A slower pass keeps the best and reports it.
    const auto second = traps.update({0.0F, 0.0F, -0.1F}, {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 30.0F});
    CHECK(second.has_value());
    CHECK_NEAR(second->previousBest, 40.0, 1e-4);
    CHECK_NEAR(traps.traps().front().best.value_or(0.0F), 40.0, 1e-4);
    CHECK_EQ(traps.passCount(), std::size_t{2});
}

TEST(driving_past_a_speed_trap_outside_its_posts_does_not_count)
{
    fh::SpeedTraps traps = oneTrap();
    CHECK(!traps.update({15.0F, 0.0F, -1.0F}, {15.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 50.0F}));
    CHECK(!traps.update({0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 3.0F}, {0.0F, 0.0F, 50.0F}));
    CHECK_EQ(traps.passCount(), std::size_t{0});
}

TEST(an_outpost_is_discovered_in_its_discovery_zone_and_then_offers_its_zone)
{
    const std::vector<fh::Activity> activities = fh::ActivityCatalogue::parse(kGasStation, "gas_stations.xml");
    const fh::GameObjects objects = fh::GameObjects::fromObjects({
        object("GASSTATION_001", {0.0F, 0.0F, 0.0F}),
        object("GASSTATION_001_DISCOVERY", {300.0F, 0.0F, 0.0F}),
    });
    fh::Outposts outposts = fh::Outposts::build(fh::ActivityCatalogue::fromActivities(activities), objects);
    CHECK_EQ(outposts.all().size(), std::size_t{1});
    // Undiscovered, its own zone offers nothing.
    CHECK(!outposts.zoneAt({0.0F, 0.0F, 0.0F}, 0.0F));
    CHECK(!outposts.updateDiscovery({100.0F, 0.0F, 0.0F}));
    CHECK(outposts.updateDiscovery({130.0F, 0.0F, 0.0F}) == std::optional<std::size_t>(0));
    CHECK(!outposts.updateDiscovery({130.0F, 0.0F, 0.0F}));
    CHECK(outposts.zoneAt({10.0F, 0.0F, 0.0F}, 0.0F) == std::optional<std::size_t>(0));
    CHECK(!outposts.zoneAt({60.0F, 0.0F, 0.0F}, 0.0F));
    // Faster than its 100 mph limit, the zone does not offer itself.
    CHECK(!outposts.zoneAt({10.0F, 0.0F, 0.0F}, 45.0F));
}

TEST(a_zone_shows_its_object_and_its_numbered_objects_until_one_is_missing)
{
    const fh::GameObjects objects = fh::GameObjects::fromObjects({
        object("FR04", {}),
        object("FR04_00", {}),
        object("FR04_01", {}),
        object("FR04_03", {}),
        object("FR04_NODE", {}),
    });
    const std::vector<std::string> names = fh::ActivityProps::zoneNames("FR04", objects);
    CHECK((names == std::vector<std::string>{"FR04", "FR04_L", "FR04_00", "FR04_01"}));
}

TEST(colorado_activities)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const fh::ActivityCatalogue catalogue = fh::ActivityCatalogue::load(vfs, "colorado");
    const fh::GameObjects objects = fh::GameObjects::load(vfs, "colorado");
    CHECK_EQ(catalogue.ofType("ActivitySpeedCamera").size(), std::size_t{22});
    CHECK_EQ(catalogue.ofType("ActivityAverageSpeed").size(), std::size_t{9});
    CHECK_EQ(catalogue.ofType("ActivityGasStation").size(), std::size_t{10});
    CHECK_EQ(catalogue.ofType("ActivityFlyers").size(), std::size_t{100});
    CHECK_EQ(catalogue.ofType("ActivityBarnFind").size(), std::size_t{9});
    CHECK_EQ(catalogue.ofType("ActivityCareerEventActivation").size(), std::size_t{88});

    const fh::SpeedTraps traps = fh::SpeedTraps::build(catalogue, objects);
    CHECK_EQ(traps.traps().size(), std::size_t{22});
    for (const fh::SpeedTrap& trap : traps.traps()) {
        CHECK_NEAR(trap.minimumSpeedMph, 30.0, 1e-6);
        // The posts stand either side of a road.
        const float gap = glm::length(trap.right - trap.left);
        CHECK(gap > 5.0F && gap < 60.0F);
    }

    const fh::Outposts outposts = fh::Outposts::build(catalogue, objects);
    CHECK_EQ(outposts.all().size(), std::size_t{10});
    for (const fh::Outpost& outpost : outposts.all()) {
        CHECK_EQ(outpost.promptTitle, std::string("IDS_Description_GasStation"));
        CHECK(outpost.discoveryRadius > 0.0F);
        CHECK(glm::length(outpost.discoveryPosition - outpost.position) > 0.0F);
        CHECK_EQ(outpost.missions.size(), std::size_t{3});
    }

    // A career event's start zone: 25 m, 150 mph.
    const auto fr04 = catalogue.zoneFor("FR04");
    CHECK(fr04.has_value());
    if (fr04) {
        CHECK_EQ(fr04->first->type, std::string("ActivityCareerEventActivation"));
        CHECK_NEAR(fr04->second->radius, 25.0, 1e-6);
        CHECK_NEAR(fr04->second->maxSpeed.value_or(0.0F), 150.0 * 0.44704, 1e-3);
        CHECK(fr04->second->isActivation);
    }
    const std::vector<std::string> names = fh::ActivityProps::zoneNames("FR04", objects);
    CHECK_EQ(names.size(), std::size_t{19});
}

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
