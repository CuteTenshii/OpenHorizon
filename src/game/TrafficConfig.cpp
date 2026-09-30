#include "TrafficConfig.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>

namespace openhorizon {

namespace {

constexpr std::string_view kPath = "media/gametunablesettings/aiopenworld.xml";

std::vector<std::string> groupIds(const pugi::xml_node& node)
{
    std::vector<std::string> ids;
    for (const pugi::xml_node group : node.children("Group")) {
        ids.emplace_back(group.attribute("id").as_string());
    }
    return ids;
}

std::map<int, TrafficConfig::Density> readDensities(const pugi::xml_node& list)
{
    std::map<int, TrafficConfig::Density> densities;
    for (const pugi::xml_node node : list.children("Density")) {
        TrafficConfig::Density d;
        d.min = node.attribute("min").as_float();
        d.max = node.attribute("max").as_float();
        d.speedMph = node.attribute("speed").as_float();
        d.spawnAwayChance = node.attribute("spawnAwayChance").as_float(d.spawnAwayChance);
        if (const pugi::xml_attribute in = node.attribute("spawnInDistance")) {
            d.spawnInDistance = in.as_float();
        }
        if (const pugi::xml_attribute out = node.attribute("spawnOutDistance")) {
            d.spawnOutDistance = out.as_float();
        }
        d.banned = groupIds(node.child("BannedVehicles"));
        d.allowed = groupIds(node.child("AllowedVehicles"));
        densities[node.attribute("id").as_int()] = std::move(d);
    }
    return densities;
}

} // namespace

bool TrafficConfig::Settings::allows(const Density& density, int carId) const
{
    const auto inGroup = [&](const std::string& group) {
        const auto it = groups.find(group);
        return it != groups.end() && std::find(it->second.begin(), it->second.end(), carId) != it->second.end();
    };
    if (!density.allowed.empty()) {
        return std::any_of(density.allowed.begin(), density.allowed.end(), inGroup);
    }
    return std::none_of(density.banned.begin(), density.banned.end(), inGroup);
}

TrafficConfig TrafficConfig::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kPath);
    if (!text) {
        throw DataError("{} not found", kPath);
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text->data(), text->size());
    if (!parsed) {
        throw DataError("{}: {}", kPath, parsed.description());
    }
    TrafficConfig config;
    for (const pugi::xml_node node : doc.child("AIOpenWorld").children("Settings")) {
        Settings s;
        const pugi::xml_node list = node.child("CarList");
        s.initialTrafficCars = list.attribute("numInitialTrafficCars").as_int();
        s.initialFestivalCars = list.attribute("numInitialFestivalCars").as_int();
        s.maxLoadedTrafficModels = list.attribute("maxLoadedTrafficModels").as_int(4);
        for (const pugi::xml_node car : list.children("Car")) {
            s.cars.push_back({car.attribute("model").as_int(), car.attribute("maxactive").as_int(),
                car.attribute("largevehicle").as_bool()});
        }
        const pugi::xml_node densities = node.child("Densities");
        for (const pugi::xml_node group : densities.child("CarGroups").children("Group")) {
            std::vector<int>& members = s.groups[group.attribute("id").as_string()];
            for (const pugi::xml_node car : group.children("Car")) {
                members.push_back(car.attribute("model").as_int());
            }
        }
        for (const pugi::xml_node densityList : densities.children("DensityList")) {
            const std::string type = densityList.attribute("cartype").as_string();
            (type == "festival" ? s.festival : s.traffic) = readDensities(densityList);
        }
        config.settings[node.attribute("name").as_string()] = std::move(s);
    }
    // Situations without their own car list (the race one) use free roam's.
    const auto freeroam = config.settings.find("freeroam");
    if (freeroam != config.settings.end()) {
        for (auto& [name, s] : config.settings) {
            if (s.cars.empty()) {
                s.cars = freeroam->second.cars;
            }
            if (s.groups.empty()) {
                s.groups = freeroam->second.groups;
            }
        }
    }
    return config;
}

const TrafficConfig::Settings* TrafficConfig::find(std::string_view name) const
{
    const auto it = settings.find(std::string(name));
    return it == settings.end() ? nullptr : &it->second;
}

} // namespace openhorizon
