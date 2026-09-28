#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {

class Database;
class Localisation;
class Vfs;

/// One race of the career, from gamedb: an `Events` row joined with its
/// `Races` row and the `Tracks` row that names the route.
struct RaceEvent {
    int eventId = 0;
    int raceId = 0;
    /// "FR04", "STREET_PLNS_005", ...: names the event's objects in
    /// GameObjs.xml and its props in the world.
    std::string horizonId;
    std::string name;
    std::string type;
    /// The route file (`Ribbon_00/TrackRouteNNN.xml`) and racing line
    /// (`route_NNN.owt`).
    int routeId = 0;
    int laps = 1;
    /// Opponents, not counting the player.
    int drivers = 7;
    int targetClass = 0;
    float lengthMetres = 0.0F;
    int cashPrize = 0;
    /// Seconds after midnight the race is run at.
    float timeOfDay = 16.0F * 3600.0F;
    /// 1-based grid slot of the player; 0 when the table leaves it open.
    int playerGridPosition = 0;
    /// AI settings for the medium difficulty (0 to 100 skill).
    int aiSkill = 70;
    int aiTemperament = 5;
    /// Car ids (Data_Car.Id) of the opponents, in grid order, and the
    /// factory colour each runs (Combo_Colors.Sequence).
    std::vector<int> opponents;
    std::vector<int> opponentColours;
};

/// Loads the races run on `track` (a Tracks.MediaName, e.g. "Colorado"),
/// resolving names through `strings`. Free-roam sessions are left out.
std::vector<RaceEvent> loadRaceEvents(const Database& db, Localisation& strings, std::string_view track);

/// A named point of a route file: its position, the direction it faces and,
/// for checkpoints and the finish, the gate width.
struct RouteTransform {
    std::string name;
    glm::vec3 position{0.0F};
    glm::vec3 facing{0.0F, 0.0F, 1.0F};
    float width = 0.0F;
};

/// `Ribbon_00/TrackRouteNNN.xml`: `<NamedTransform>` elements whose names
/// give their roles (`start_location_NN` grid slots with pole first,
/// `route_checkpoint_NN`, `route_waypoint_NN`, `end_race_cannon_trigger`
/// for the finish).
struct RaceRoute {
    std::vector<RouteTransform> grid;
    std::vector<RouteTransform> checkpoints;
    std::vector<RouteTransform> waypoints;
    std::optional<RouteTransform> finish;
    std::vector<RouteTransform> postRace;

    static RaceRoute load(const Vfs& vfs, std::string_view track, int routeId);
};

/// An AI racing line, `aiopenworld/<track>/ribbon_00/route_NNN.owt`
/// (big-endian): "OWTM", u32 version 1, 8 zero bytes, u32 point count, u32
/// closed-loop flag, 8 zero bytes, then 48-byte points and a 16-byte
/// trailer. A point holds f32 x, y, z, then eight more floats; the fourth of
/// them stays near 6 along ordinary roads and is taken as the half-width of
/// the drivable corridor (inferred from the data). Points lie about 2 m
/// apart.
struct RacingLine {
    struct Point {
        glm::vec3 position{0.0F};
        float halfWidth = 5.0F;
        /// Distance along the line from its first point.
        float distance = 0.0F;
    };
    std::vector<Point> points;
    bool loop = false;
    float length = 0.0F;

    static std::optional<RacingLine> load(const Vfs& vfs, std::string_view track, int routeId);
    /// A line through the given points, for routes without a racing line.
    static RacingLine through(const std::vector<glm::vec3>& points, bool loop);

    /// The point on the line nearest to `position`, searched within
    /// `window` points of `hint` (or everywhere when hint is negative), as
    /// a fractional index.
    float project(const glm::vec3& position, float hint = -1.0F, int window = 60) const;
    /// Position and direction at a fractional index, wrapping on loops.
    glm::vec3 positionAt(float index) const;
    glm::vec3 directionAt(float index) const;
    float distanceAt(float index) const;
    /// Advances a fractional index by `metres`.
    float advance(float index, float metres) const;
};

} // namespace fh
