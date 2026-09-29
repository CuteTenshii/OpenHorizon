#include "Outposts.h"

#include "game/Activities.h"
#include "game/GameObjects.h"

#include <algorithm>

namespace fh {

Outposts Outposts::build(const ActivityCatalogue& catalogue, const GameObjects& objects)
{
    Outposts outposts;
    for (const Activity* activity : catalogue.ofType("ActivityGasStation")) {
        if (activity->zones.empty()) {
            continue;
        }
        const ActivityZone& zone = activity->zones.front();
        const GameObject* object = objects.find(zone.object);
        if (object == nullptr) {
            continue;
        }
        Outpost outpost;
        outpost.name = activity->name;
        outpost.object = zone.object;
        outpost.position = object->position;
        outpost.radius = zone.radius;
        outpost.maxSpeed = zone.maxSpeed;
        outpost.promptTitle = zone.promptTitle;
        outpost.prompt = zone.prompt;
        outpost.mapDescription = zone.mapDescription;
        const GameObject* discovery = objects.find(zone.object + "_DISCOVERY");
        outpost.discoveryPosition = discovery != nullptr ? discovery->position : object->position;
        outpost.discoveryRadius = zone.triggerZoneRadius.value_or(0.0F);
        outpost.missions = activity->missions;
        outposts.m_outposts.push_back(std::move(outpost));
    }
    return outposts;
}

bool Outposts::restoreDiscovered(std::string_view name)
{
    for (Outpost& outpost : m_outposts) {
        if (outpost.name == name) {
            outpost.discovered = true;
            return true;
        }
    }
    return false;
}

std::optional<std::size_t> Outposts::updateDiscovery(const glm::vec3& position)
{
    for (std::size_t i = 0; i < m_outposts.size(); ++i) {
        Outpost& outpost = m_outposts[i];
        if (!outpost.discovered && glm::length(position - outpost.discoveryPosition) < outpost.discoveryRadius) {
            outpost.discovered = true;
            return i;
        }
    }
    return std::nullopt;
}

std::optional<std::size_t> Outposts::zoneAt(const glm::vec3& position, float speed) const
{
    for (std::size_t i = 0; i < m_outposts.size(); ++i) {
        const Outpost& outpost = m_outposts[i];
        if (!outpost.discovered || glm::length(position - outpost.position) >= outpost.radius) {
            continue;
        }
        if (outpost.maxSpeed && speed >= *outpost.maxSpeed) {
            continue;
        }
        return i;
    }
    return std::nullopt;
}

std::size_t Outposts::discoveredCount() const
{
    return static_cast<std::size_t>(
        std::count_if(m_outposts.begin(), m_outposts.end(), [](const Outpost& o) { return o.discovered; }));
}

} // namespace fh
