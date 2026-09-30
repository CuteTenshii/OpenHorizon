// The game mode's activities: the catalogue read from the disc, and the
// rules of the speed traps, the Horizon outposts and the set dressing of
// activation zones, on synthetic data and on Colorado when FH1_GAME_DIR
// names the disc.

#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "game/ActivationFlares.h"
#include "game/Activities.h"
#include "game/ActivityProps.h"
#include "game/GameObjects.h"
#include "game/Outposts.h"
#include "game/SpeedTraps.h"
#include "particles/ParticleEmitters.h"
#include "particles/ParticleManager.h"
#include "ui/MenuCommon.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <vector>

using Catch::Matchers::WithinAbs;

using openhorizon::test::gameDir;

namespace {

openhorizon::GameObject object(const std::string& id, glm::vec3 position)
{
    openhorizon::GameObject o;
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

TEST_CASE("zone_attributes_parse_as_the_game_reads_them", "[activities]")
{
    const std::vector<openhorizon::Activity> activities
        = openhorizon::ActivityCatalogue::parse(kGasStation, "gas_stations.xml");
    CHECK(activities.size() == std::size_t{1});
    const openhorizon::ActivityZone& zone = activities.front().zones.front();
    CHECK(zone.object == std::string("GASSTATION_001"));
    CHECK_THAT(zone.radius, WithinAbs(54.0, 1e-6));
    CHECK(zone.maxSpeed.has_value());
    CHECK_THAT(zone.maxSpeed.value_or(0.0F), WithinAbs(100.0 * 0.44704, 1e-4));
    CHECK(!zone.isActivation);
    CHECK(zone.requiresInput);
    CHECK_THAT(zone.triggerZoneRadius.value_or(0.0F), WithinAbs(175.0, 1e-6));
    CHECK(activities.front().missions.size() == std::size_t{2});
}

TEST_CASE("commented_out_activities_are_skipped", "[activities]")
{
    const std::vector<openhorizon::Activity> activities
        = openhorizon::ActivityCatalogue::parse(kSpeedCamera, "speed_camera.xml");
    CHECK(activities.size() == std::size_t{1});
    CHECK(activities.front().scoreboardId == 201);
    CHECK_THAT(activities.front().minimumSpeedMph.value_or(0.0F), WithinAbs(30.0, 1e-6));
}

/// A speed trap across the Z axis at the origin, its posts 20 m apart.
openhorizon::SpeedTraps oneTrap()
{
    const std::vector<openhorizon::Activity> activities
        = openhorizon::ActivityCatalogue::parse(kSpeedCamera, "speed_camera.xml");
    const openhorizon::GameObjects objects = openhorizon::GameObjects::fromObjects({
        object("speed_camera_01_left", {-10.0F, 0.0F, 0.0F}),
        object("speed_camera_01_right", {10.0F, 0.0F, 0.0F}),
    });
    return openhorizon::SpeedTraps::build(openhorizon::ActivityCatalogue::fromActivities(activities), objects);
}

TEST_CASE("a_speed_trap_counts_a_pass_at_its_minimum_speed_or_more", "[activities]")
{
    openhorizon::SpeedTraps traps = oneTrap();
    CHECK(traps.traps().size() == std::size_t{1});
    // 30 mph is 13.41 m/s: just below does not count and discovers nothing.
    CHECK(!traps.update({0.0F, 0.0F, -0.1F}, {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 13.3F}));
    CHECK(!traps.traps().front().discovered);
    const auto pass = traps.update({0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, -0.1F}, {0.0F, 0.0F, -40.0F});
    CHECK(pass.has_value());
    CHECK_THAT(pass->speed, WithinAbs(40.0, 1e-4));
    CHECK_THAT(pass->previousBest, WithinAbs(0.0, 1e-6));
    CHECK(traps.traps().front().discovered);
    // A slower pass keeps the best and reports it.
    const auto second = traps.update({0.0F, 0.0F, -0.1F}, {0.0F, 0.0F, 0.1F}, {0.0F, 0.0F, 30.0F});
    CHECK(second.has_value());
    CHECK_THAT(second->previousBest, WithinAbs(40.0, 1e-4));
    CHECK_THAT(traps.traps().front().best.value_or(0.0F), WithinAbs(40.0, 1e-4));
    CHECK(traps.passCount() == std::size_t{2});
}

TEST_CASE("driving_past_a_speed_trap_outside_its_posts_does_not_count", "[activities]")
{
    openhorizon::SpeedTraps traps = oneTrap();
    CHECK(!traps.update({15.0F, 0.0F, -1.0F}, {15.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 50.0F}));
    CHECK(!traps.update({0.0F, 0.0F, 1.0F}, {0.0F, 0.0F, 3.0F}, {0.0F, 0.0F, 50.0F}));
    CHECK(traps.passCount() == std::size_t{0});
}

TEST_CASE("an_outpost_is_discovered_in_its_discovery_zone_and_then_offers_its_zone", "[activities]")
{
    const std::vector<openhorizon::Activity> activities
        = openhorizon::ActivityCatalogue::parse(kGasStation, "gas_stations.xml");
    const openhorizon::GameObjects objects = openhorizon::GameObjects::fromObjects({
        object("GASSTATION_001", {0.0F, 0.0F, 0.0F}),
        object("GASSTATION_001_DISCOVERY", {300.0F, 0.0F, 0.0F}),
    });
    openhorizon::Outposts outposts
        = openhorizon::Outposts::build(openhorizon::ActivityCatalogue::fromActivities(activities), objects);
    CHECK(outposts.all().size() == std::size_t{1});
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

TEST_CASE("a_zone_shows_its_object_and_its_numbered_objects_until_one_is_missing", "[activities]")
{
    const openhorizon::GameObjects objects = openhorizon::GameObjects::fromObjects({
        object("FR04", {}),
        object("FR04_00", {}),
        object("FR04_01", {}),
        object("FR04_03", {}),
        object("FR04_NODE", {}),
    });
    const std::vector<std::string> names = openhorizon::ActivityProps::zoneNames("FR04", objects);
    CHECK((names == std::vector<std::string>{"FR04", "FR04_L", "FR04_00", "FR04_01"}));
}

TEST_CASE("colorado_activities", "[activities]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const openhorizon::ActivityCatalogue catalogue = openhorizon::ActivityCatalogue::load(vfs, "colorado");
    const openhorizon::GameObjects objects = openhorizon::GameObjects::load(vfs, "colorado");
    CHECK(catalogue.ofType("ActivitySpeedCamera").size() == std::size_t{22});
    CHECK(catalogue.ofType("ActivityAverageSpeed").size() == std::size_t{9});
    CHECK(catalogue.ofType("ActivityGasStation").size() == std::size_t{10});
    CHECK(catalogue.ofType("ActivityFlyers").size() == std::size_t{100});
    CHECK(catalogue.ofType("ActivityBarnFind").size() == std::size_t{9});
    CHECK(catalogue.ofType("ActivityCareerEventActivation").size() == std::size_t{88});

    const openhorizon::SpeedTraps traps = openhorizon::SpeedTraps::build(catalogue, objects);
    CHECK(traps.traps().size() == std::size_t{22});
    for (const openhorizon::SpeedTrap& trap : traps.traps()) {
        CHECK_THAT(trap.minimumSpeedMph, WithinAbs(30.0, 1e-6));
        // The posts stand either side of a road.
        const float gap = glm::length(trap.right - trap.left);
        CHECK((gap > 5.0F && gap < 60.0F));
    }

    const openhorizon::Outposts outposts = openhorizon::Outposts::build(catalogue, objects);
    CHECK(outposts.all().size() == std::size_t{10});
    for (const openhorizon::Outpost& outpost : outposts.all()) {
        CHECK(outpost.promptTitle == std::string("IDS_Description_GasStation"));
        CHECK(outpost.discoveryRadius > 0.0F);
        CHECK(glm::length(outpost.discoveryPosition - outpost.position) > 0.0F);
        CHECK(outpost.missions.size() == std::size_t{3});
    }

    // A career event's start zone: 25 m, 150 mph.
    const auto fr04 = catalogue.zoneFor("FR04");
    CHECK(fr04.has_value());
    if (fr04) {
        CHECK(fr04->first->type == std::string("ActivityCareerEventActivation"));
        CHECK_THAT(fr04->second->radius, WithinAbs(25.0, 1e-6));
        CHECK_THAT(fr04->second->maxSpeed.value_or(0.0F), WithinAbs(150.0 * 0.44704, 1e-3));
        CHECK(fr04->second->isActivation);
    }
    const std::vector<std::string> names = openhorizon::ActivityProps::zoneNames("FR04", objects);
    CHECK(names.size() == std::size_t{19});
}

// Each Horizon outpost GasStation_NN is the zone GASSTATION_0NN, and its
// prompt and map entry name it with IDS_GasStationName_NN of the Activities
// table, under the title IDS_Description_GasStation. A player took the
// "HORIZON OUTPOST / MEDIA CENTER" prompt for a festival building's: MEDIA
// CENTER is outpost 5's own name, its zone lies over 3 km from the festival
// buildings' zones, and none of those zones uses an outpost's text.
TEST_CASE("colorado_outposts_are_named_by_their_own_strings", "[activities]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const openhorizon::ActivityCatalogue catalogue = openhorizon::ActivityCatalogue::load(vfs, "colorado");
    const openhorizon::GameObjects objects = openhorizon::GameObjects::load(vfs, "colorado");
    const openhorizon::Outposts outposts = openhorizon::Outposts::build(catalogue, objects);
    openhorizon::ui::UiStrings strings(vfs, "en");
    CHECK(outposts.all().size() == std::size_t{10});
    std::vector<std::string> shownNames;
    for (std::size_t i = 0; i < outposts.all().size(); ++i) {
        const openhorizon::Outpost& outpost = outposts.all()[i];
        const std::size_t number = i + 1;
        CHECK(outpost.name == std::format("GasStation_{:02}", number));
        CHECK(outpost.object == std::format("GASSTATION_{:03}", number));
        CHECK(outpost.prompt == std::format("IDS_GasStationName_{:02}", number));
        CHECK(outpost.mapDescription == outpost.prompt);
        CHECK(strings.has("Activities", outpost.prompt));
        shownNames.push_back(strings.get("Activities", outpost.prompt));
    }
    CHECK(strings.get("Activities", "IDS_Description_GasStation") == std::string("HORIZON OUTPOST"));
    std::vector<std::string> distinct = shownNames;
    std::sort(distinct.begin(), distinct.end());
    CHECK(std::adjacent_find(distinct.begin(), distinct.end()) == distinct.end());

    const openhorizon::Outpost& mediaCenter = outposts.all()[4];
    CHECK(shownNames[4] == std::string("MEDIA CENTER"));
    std::size_t buildings = 0;
    for (const char* type : {"ActivityAutoshow", "ActivityRaceCentral", "ActivityCarClub", "ActivityDlcCenter",
             "ActivityPaintshop", "ActivityWorkshop"}) {
        for (const openhorizon::Activity* activity : catalogue.ofType(type)) {
            for (const openhorizon::ActivityZone& zone : activity->zones) {
                ++buildings;
                CHECK(!zone.prompt.starts_with("IDS_GasStationName_"));
                CHECK(zone.promptTitle != "IDS_Description_GasStation");
                const openhorizon::GameObject* building = objects.find(zone.object);
                CHECK(building != nullptr);
                if (building != nullptr) {
                    CHECK(glm::length(building->position - mediaCenter.position) > 3000.0F);
                }
            }
        }
    }
    CHECK(buildings == std::size_t{6});
}

} // namespace

// Every career event zone of Colorado has its flare in ParticleEmitters.xml,
// named after the zone's object. A zone burns it while enabled with the
// player's car within 500 m (0x8298dcf0, 0x8298dbe8), and puts it out when
// disabled, as in a race, or when the car leaves.
TEST_CASE("an_enabled_zone_burns_its_flare_while_the_car_is_within_500_m", "[activities]")
{
    const char* dir = gameDir();
    openhorizon::Vfs vfs;
    vfs.mountDisc(dir);
    const openhorizon::ActivityCatalogue catalogue = openhorizon::ActivityCatalogue::load(vfs, "colorado");
    const openhorizon::particles::ParticleEmitterSet set
        = openhorizon::particles::ParticleEmitterSet::load(vfs, "colorado");
    std::size_t zones = 0;
    std::size_t withFlare = 0;
    for (const openhorizon::Activity* activity : catalogue.ofType("ActivityCareerEventActivation")) {
        for (const openhorizon::ActivityZone& zone : activity->zones) {
            ++zones;
            withFlare += set.findSimple(openhorizon::ActivationFlares::flareId(zone.object)) != nullptr ? 1 : 0;
        }
    }
    CHECK(zones >= 88);
    CHECK(withFlare == std::size_t{88});

    const openhorizon::GameObjects objects = openhorizon::GameObjects::load(vfs, "colorado");
    const openhorizon::GameObject* fr04 = objects.find("FR04");
    CHECK(fr04 != nullptr);
    if (fr04 == nullptr) {
        return;
    }
    openhorizon::particles::ParticleManager particles = openhorizon::particles::ParticleManager::load(vfs);
    particles.placeSimpleEmitters(vfs, set);
    const openhorizon::particles::SimpleEmitterState* emitter = particles.simpleEmitter("FR04_flare");
    CHECK(emitter != nullptr);
    if (emitter == nullptr) {
        return;
    }
    // The flare stands by its zone's object.
    CHECK(glm::distance(emitter->position, fr04->position) < 20.0F);

    openhorizon::ActivationFlares flares;
    const std::size_t zone = flares.add("FR04", fr04->position);
    const glm::vec3 near = fr04->position + glm::vec3(0.0F, 0.0F, 499.0F);
    const glm::vec3 far = fr04->position + glm::vec3(0.0F, 0.0F, 501.0F);
    flares.update(near, particles);
    CHECK(!flares.burning(zone));
    flares.setEnabled(zone, true);
    flares.update(far, particles);
    CHECK(!flares.burning(zone));
    flares.update(near, particles);
    CHECK((flares.burning(zone) && emitter->running));
    flares.setEnabled(zone, false);
    flares.update(near, particles);
    CHECK((!flares.burning(zone) && !emitter->running));
    flares.setEnabled(zone, true);
    flares.update(near, particles);
    CHECK(emitter->running);
    flares.update(std::nullopt, particles);
    CHECK(!emitter->running);
}
