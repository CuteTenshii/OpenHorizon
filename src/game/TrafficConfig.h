#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class Vfs;

/// `gametunablesettings/aiopenworld.xml` per situation ("freeroam", "race", "firsttimecareer").
/// Road classes per the file's comments: 2 B-road, 3 and 5 A-road, 4 freeway, 6 and 7 town, 8 festival, 12 dirt.
/// Inferred units: `speed` in mph, densities in cars per 100 m of road.
struct TrafficConfig {
    struct Car {
        int carId = 0;
        /// At most this many on the road at once; 0 means no limit.
        int maxActive = 0;
        bool largeVehicle = false;
    };
    struct Density {
        float min = 0.0F;
        float max = 0.0F;
        /// Mph; 0 when the class gives none.
        float speedMph = 0.0F;
        float spawnAwayChance = 0.5F;
        std::optional<float> spawnInDistance;
        std::optional<float> spawnOutDistance;
        /// Car groups banned on, or the only ones allowed on, this class.
        std::vector<std::string> banned;
        std::vector<std::string> allowed;
    };
    struct Settings {
        std::vector<Car> cars;
        int initialTrafficCars = 0;
        int initialFestivalCars = 0;
        int maxLoadedTrafficModels = 4;
        std::map<std::string, std::vector<int>> groups;
        /// Road class to density, for "traffic" and "festival" cars.
        std::map<int, Density> traffic;
        std::map<int, Density> festival;

        /// Whether `carId` may drive on a road of `density`.
        bool allows(const Density& density, int carId) const;
    };

    std::map<std::string, Settings> settings;

    /// Throws DataError if the file is missing or malformed.
    static TrafficConfig load(const Vfs& vfs);
    const Settings* find(std::string_view name) const;
};

} // namespace openhorizon
