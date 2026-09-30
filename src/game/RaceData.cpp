#include "RaceData.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "base/Log.h"
#include "data/Database.h"
#include "data/StringTable.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

namespace openhorizon {

namespace {

constexpr std::size_t kOwtHeader = 32;
constexpr std::size_t kOwtPoint = 48;
constexpr std::size_t kOwtTrailer = 16;

/// The number after the last underscore of a transform name, for ordering
/// ("route_checkpoint_07" gives 7); -1 when there is none.
int trailingNumber(std::string_view name)
{
    const std::size_t underscore = name.rfind('_');
    if (underscore == std::string_view::npos) {
        return -1;
    }
    int value = 0;
    bool any = false;
    for (std::size_t i = underscore + 1; i < name.size() && name[i] >= '0' && name[i] <= '9'; ++i) {
        value = value * 10 + (name[i] - '0');
        any = true;
    }
    return any ? value : -1;
}

void sortByNumber(std::vector<RouteTransform>& transforms)
{
    std::stable_sort(transforms.begin(), transforms.end(), [](const RouteTransform& a, const RouteTransform& b) {
        return trailingNumber(a.name) < trailingNumber(b.name);
    });
}

void finishLine(RacingLine& line)
{
    float distance = 0.0F;
    for (std::size_t i = 0; i < line.points.size(); ++i) {
        if (i > 0) {
            distance += glm::length(line.points[i].position - line.points[i - 1].position);
        }
        line.points[i].distance = distance;
    }
    if (line.loop && line.points.size() > 1) {
        distance += glm::length(line.points.front().position - line.points.back().position);
    }
    line.length = distance;
}

} // namespace

std::vector<RaceEvent> loadRaceEvents(const Database& db, Localisation& strings, std::string_view track)
{
    const std::vector<Row> rows = db.query(
        "SELECT e.Id AS EventId, r.Id AS RaceId, e.HorizonEventID, e.Name, ty.Name AS TypeName, t.RouteId, "
        "r.NumLaps, t.Length, e.CashPrize, e.NumberOfDrivers, e.TimeOfDayStart, e.TargetClass, "
        "e.PlayerGridPosition, e.AISkillMed, e.AITemperamentMed, e.CareerEventStyle FROM Races r "
        "JOIN Events e ON e.Id = r.EventId JOIN Tracks t ON t.id = r.TrackId "
        "LEFT JOIN CareerEventTypes ty ON ty.id = e.CareerTypeId "
        "WHERE lower(t.MediaName) = lower(?) AND e.CareerEventStyle <> 0 ORDER BY e.EventOrder, e.Id, r.RaceNumber",
        {std::string(track)});
    std::vector<RaceEvent> events;
    for (const Row& row : rows) {
        RaceEvent event;
        event.eventId = row.i("EventId");
        event.raceId = row.i("RaceId");
        event.horizonId = row.text("HorizonEventID");
        event.name = strings.resolve("Events", row.text("Name"));
        event.type = strings.resolve("CareerEventTypes", row.text("TypeName"));
        event.routeId = row.i("RouteId");
        event.careerEventStyle = row.i("CareerEventStyle");
        event.laps = std::max(1, row.i("NumLaps", 1));
        event.drivers = row.i("NumberOfDrivers", 7);
        event.targetClass = row.i("TargetClass");
        event.lengthMetres = row.f("Length");
        event.cashPrize = row.i("CashPrize");
        event.timeOfDay = row.f("TimeOfDayStart", event.timeOfDay);
        event.playerGridPosition = row.i("PlayerGridPosition");
        event.aiSkill = row.i("AISkillMed", 70);
        event.aiTemperament = row.i("AITemperamentMed", 5);
        for (const Row& participant : db.query(
                 "SELECT Ordinal, ColorSequence FROM EventParticipants WHERE EventID = ? ORDER BY Priority, ID",
                 {std::to_string(event.eventId)})) {
            event.opponents.push_back(participant.i("Ordinal"));
            event.opponentColours.push_back(participant.i("ColorSequence"));
        }
        events.push_back(std::move(event));
    }
    logInfo("races: {} on {}", events.size(), track);
    return events;
}

RaceRoute RaceRoute::load(const Vfs& vfs, std::string_view track, int routeId)
{
    const std::string path = std::format("media/tracks/{}/Ribbon_00/TrackRoute{:03d}.xml", track, routeId);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        throw DataError("{} not found", path);
    }
    pugi::xml_document doc;
    const pugi::xml_parse_result parsed = doc.load_buffer(text->data(), text->size());
    if (!parsed) {
        throw DataError("{}: {}", path, parsed.description());
    }
    RaceRoute route;
    for (const pugi::xml_node node : doc.child("TrackRoute").child("NamedTransforms").children("NamedTransform")) {
        RouteTransform t;
        t.name = node.attribute("name").as_string();
        const pugi::xml_node transform = node.child("Transform");
        t.position = {transform.attribute("pos.x").as_float(), transform.attribute("pos.y").as_float(),
            transform.attribute("pos.z").as_float()};
        t.facing = {transform.attribute("facing.x").as_float(), transform.attribute("facing.y").as_float(),
            transform.attribute("facing.z").as_float(1.0F)};
        if (glm::length(t.facing) > 1e-4F) {
            t.facing = glm::normalize(t.facing);
        }
        t.width = node.attribute("width").as_float(transform.attribute("width").as_float(0.0F));
        const std::string lower = toLower(t.name);
        if (lower.starts_with("start_location")) {
            route.grid.push_back(std::move(t));
        } else if (lower.starts_with("route_checkpoint_indicator")) {
            continue;
        } else if (lower.starts_with("route_checkpoint")) {
            route.checkpoints.push_back(std::move(t));
        } else if (lower.starts_with("route_waypoint")) {
            route.waypoints.push_back(std::move(t));
        } else if (lower.starts_with("end_race_cannon_trigger")) {
            route.finish = std::move(t);
        } else if (lower.starts_with("post_race_location")) {
            route.postRace.push_back(std::move(t));
        }
    }
    sortByNumber(route.grid);
    sortByNumber(route.checkpoints);
    sortByNumber(route.waypoints);
    sortByNumber(route.postRace);
    return route;
}

std::optional<RacingLine> RacingLine::load(const Vfs& vfs, std::string_view track, int routeId)
{
    const std::string path = std::format("media/aiopenworld/{}/ribbon_00/route_{:03d}.owt", toLower(track), routeId);
    const std::optional<Bytes> data = vfs.read(path);
    if (!data) {
        return std::nullopt;
    }
    BeReader r(*data, path);
    if (data->size() < kOwtHeader || std::string_view(reinterpret_cast<const char*>(data->data()), 4) != "OWTM") {
        r.fail("not an OWTM racing line");
    }
    r.seek(4);
    r.expect(1, "version");
    r.skip(8);
    const std::uint32_t count = r.u32();
    RacingLine line;
    line.loop = r.u32() != 0;
    if (data->size() != kOwtHeader + static_cast<std::size_t>(count) * kOwtPoint + kOwtTrailer || count < 2) {
        r.fail(std::format("{} points do not fit {} bytes", count, data->size()));
    }
    line.points.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        r.seek(kOwtHeader + static_cast<std::size_t>(i) * kOwtPoint);
        Point point;
        point.position.x = r.f32();
        point.position.y = r.f32();
        point.position.z = r.f32();
        r.skip(12);
        const float halfWidth = r.f32();
        point.halfWidth = std::isfinite(halfWidth) && halfWidth > 1.0F ? halfWidth : 5.0F;
        line.points.push_back(point);
    }
    finishLine(line);
    return line;
}

RacingLine RacingLine::through(const std::vector<glm::vec3>& points, bool loop)
{
    // Resample to about 2 m, like the game's own lines, so lookups by index
    // advance evenly.
    RacingLine line;
    line.loop = loop;
    const std::size_t n = points.size();
    for (std::size_t i = 0; i + (loop ? 0 : 1) < n; ++i) {
        const glm::vec3 a = points[i];
        const glm::vec3 b = points[(i + 1) % n];
        const int steps = std::max(1, static_cast<int>(glm::length(b - a) / 2.0F));
        for (int s = 0; s < steps; ++s) {
            line.points.push_back({glm::mix(a, b, static_cast<float>(s) / static_cast<float>(steps)), 6.0F, 0.0F});
        }
    }
    if (!loop && n > 0) {
        line.points.push_back({points.back(), 6.0F, 0.0F});
    }
    finishLine(line);
    return line;
}

RacingLine RacingLine::reversed() const
{
    RacingLine line;
    line.loop = loop;
    line.points.assign(points.rbegin(), points.rend());
    finishLine(line);
    return line;
}

float RacingLine::project(const glm::vec3& position, float hint, int window) const
{
    const auto count = static_cast<int>(points.size());
    if (count < 2) {
        return 0.0F;
    }
    int first = 0;
    int last = count - 1;
    if (hint >= 0.0F) {
        first = static_cast<int>(hint) - window / 4;
        last = static_cast<int>(hint) + window;
        if (!loop) {
            first = std::max(first, 0);
            last = std::min(last, count - 1);
        }
    }
    float best = std::numeric_limits<float>::max();
    float bestIndex = 0.0F;
    for (int k = first; k < last; ++k) {
        const int i = ((k % count) + count) % count;
        const int j = (i + 1) % count;
        if (!loop && j == 0) {
            continue;
        }
        const glm::vec3 a = points[static_cast<std::size_t>(i)].position;
        const glm::vec3 b = points[static_cast<std::size_t>(j)].position;
        const glm::vec3 ab = b - a;
        const float t = std::clamp(glm::dot(position - a, ab) / std::max(glm::dot(ab, ab), 1e-6F), 0.0F, 1.0F);
        const float d = glm::length(position - (a + ab * t));
        if (d < best) {
            best = d;
            bestIndex = static_cast<float>(i) + t;
        }
    }
    return bestIndex;
}

glm::vec3 RacingLine::positionAt(float index) const
{
    const auto count = static_cast<float>(points.size());
    if (loop) {
        index = std::fmod(index, count);
        if (index < 0.0F) {
            index += count;
        }
    } else {
        index = std::clamp(index, 0.0F, count - 1.0F);
    }
    const auto i = static_cast<std::size_t>(index);
    const std::size_t j = loop ? (i + 1) % points.size() : std::min(i + 1, points.size() - 1);
    return glm::mix(points[i].position, points[j].position, index - static_cast<float>(i));
}

glm::vec3 RacingLine::directionAt(float index) const
{
    const glm::vec3 d = positionAt(index + 1.0F) - positionAt(index);
    const float span = glm::length(d);
    return span > 1e-4F ? d / span : glm::vec3(0.0F, 0.0F, 1.0F);
}

float RacingLine::distanceAt(float index) const
{
    const auto count = static_cast<float>(points.size());
    const float laps = loop ? std::floor(index / count) : 0.0F;
    float wrapped = loop ? index - laps * count : std::clamp(index, 0.0F, count - 1.0F);
    const auto i = static_cast<std::size_t>(wrapped);
    const float f = wrapped - static_cast<float>(i);
    const float next = (i + 1 < points.size()) ? points[i + 1].distance : length;
    return laps * length + points[i].distance + (next - points[i].distance) * f;
}

float RacingLine::advance(float index, float metres) const
{
    // Points are close to evenly spaced, so the average spacing converts
    // metres to index steps well enough for look-ahead.
    const float spacing = length / static_cast<float>(std::max<std::size_t>(points.size() - (loop ? 0 : 1), 1));
    return index + metres / std::max(spacing, 0.1F);
}

} // namespace openhorizon
