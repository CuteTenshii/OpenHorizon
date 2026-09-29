#include "Activities.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <format>
#include <iterator>
#include <utility>

namespace fh {

namespace {

/// The executable's miles-per-hour to metres-per-second factor (0x82005aec).
constexpr float kMphToMs = 0.44704F;

ActivityZone readZone(const pugi::xml_node& node)
{
    ActivityZone zone;
    zone.object = node.attribute("object").as_string();
    zone.secondObject = node.attribute("secondObject").as_string();
    zone.name = node.attribute("name").as_string();
    zone.radius = node.attribute("radius").as_float();
    zone.promptTitle = node.attribute("promptTitle").as_string();
    zone.prompt = node.attribute("prompt").as_string();
    if (const pugi::xml_attribute mph = node.attribute("maxMPH")) {
        zone.maxSpeed = mph.as_float() * kMphToMs;
    }
    zone.mapTag = node.attribute("mapTag").as_string();
    zone.mapDescription = node.attribute("mapDescription").as_string();
    zone.isActivation = node.attribute("is_activation").as_bool(true);
    zone.requiresInput = node.attribute("requires_input").as_bool(true);
    if (const pugi::xml_attribute radius = node.attribute("triggerZoneRadius")) {
        zone.triggerZoneRadius = radius.as_float();
    }
    return zone;
}

} // namespace

const std::vector<std::string_view>& ActivityCatalogue::fileNames()
{
    static const std::vector<std::string_view> names = {"Leaderboards.xml", "pause.xml", "gas_stations.xml",
        "street_race_hubs.xml", "Flyers.xml", "dlccenter.xml", "rally_event_activations.xml",
        "career_event_activations.xml", "festival_entrances.xml", "multiplayer.xml", "activities.xml",
        "race_encounters.xml", "speed_camera.xml", "average_speed.xml", "race_activations.xml", "playmovie.xml",
        "race_central.xml", "autoshow.xml", "carclub.xml", "paintshop.xml", "mission_speedstunts.xml",
        "mission_prstunts.xml", "mission_photoshoots.xml", "barnfinds.xml", "workshops.xml", "state_definitions.xml"};
    return names;
}

std::vector<Activity> ActivityCatalogue::parse(std::string_view text, const std::string& file)
{
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text.data(), text.size());
    if (!parsed) {
        throw DataError("{}: {}", file, parsed.description());
    }
    std::vector<Activity> activities;
    for (const pugi::xml_node node : doc.child("ActivityManager").children("Activity")) {
        Activity activity;
        activity.type = node.attribute("type").as_string();
        activity.name = node.attribute("name").as_string();
        activity.scoreboardId = node.attribute("scoreboard_id").as_int(-1);
        if (const pugi::xml_node threshold = node.child("SpeedThreshold")) {
            activity.minimumSpeedMph = threshold.attribute("minimumSpeed").as_float();
        }
        for (const pugi::xml_node zone : node.children("TriggerZone")) {
            activity.zones.push_back(readZone(zone));
        }
        for (const pugi::xml_node mission : node.child("Missions").children("Mission")) {
            activity.missions.emplace_back(mission.attribute("name").as_string());
        }
        activity.file = file;
        activities.push_back(std::move(activity));
    }
    return activities;
}

ActivityCatalogue ActivityCatalogue::load(const Vfs& vfs, std::string_view mode)
{
    ActivityCatalogue catalogue;
    for (const std::string_view name : fileNames()) {
        const std::optional<std::string> text = vfs.readText(std::format("media/gamemodes/{}/{}", mode, name));
        if (!text) {
            continue;
        }
        std::vector<Activity> activities = parse(*text, std::string(name));
        catalogue.m_activities.insert(catalogue.m_activities.end(), std::make_move_iterator(activities.begin()),
            std::make_move_iterator(activities.end()));
    }
    return catalogue;
}

ActivityCatalogue ActivityCatalogue::fromActivities(std::vector<Activity> activities)
{
    ActivityCatalogue catalogue;
    catalogue.m_activities = std::move(activities);
    return catalogue;
}

std::vector<const Activity*> ActivityCatalogue::ofType(std::string_view type) const
{
    std::vector<const Activity*> result;
    for (const Activity& activity : m_activities) {
        if (iequals(activity.type, type)) {
            result.push_back(&activity);
        }
    }
    return result;
}

std::optional<std::pair<const Activity*, const ActivityZone*>> ActivityCatalogue::zoneFor(std::string_view object) const
{
    for (const Activity& activity : m_activities) {
        for (const ActivityZone& zone : activity.zones) {
            if (iequals(zone.object, object)) {
                return std::pair{&activity, &zone};
            }
        }
    }
    return std::nullopt;
}

} // namespace fh
