#include "SpeedTraps.h"

#include "game/Activities.h"
#include "game/GameObjects.h"

#include "base/Bytes.h"

#include <algorithm>
#include <format>

namespace openhorizon {

namespace {

/// The game's factor (0x82005ae8).
constexpr float kMsToMph = 2.2369F;

float cross(glm::vec2 a, glm::vec2 b)
{
    return a.x * b.y - a.y * b.x;
}

bool crosses(glm::vec2 p0, glm::vec2 p1, glm::vec2 q0, glm::vec2 q1)
{
    const glm::vec2 r = p1 - p0;
    const glm::vec2 s = q1 - q0;
    const float denominator = cross(r, s);
    if (denominator == 0.0F) {
        return false;
    }
    const glm::vec2 d = q0 - p0;
    const float t = cross(d, s) / denominator;
    const float u = cross(d, r) / denominator;
    return t >= 0.0F && t <= 1.0F && u >= 0.0F && u <= 1.0F;
}

glm::vec2 ground(const glm::vec3& p)
{
    return {p.x, p.z};
}

} // namespace

std::string SpeedTrap::nameStringId() const
{
    return std::format("IDS_SpeedCamera_{}", scoreboardId);
}

SpeedTraps SpeedTraps::build(const ActivityCatalogue& catalogue, const GameObjects& objects)
{
    SpeedTraps traps;
    for (const Activity* activity : catalogue.ofType("ActivitySpeedCamera")) {
        const GameObject* left = objects.find(activity->name + "_left");
        const GameObject* right = objects.find(activity->name + "_right");
        if (left == nullptr || right == nullptr) {
            continue;
        }
        SpeedTrap trap;
        trap.name = activity->name;
        trap.scoreboardId = activity->scoreboardId;
        trap.minimumSpeedMph = activity->minimumSpeedMph.value_or(0.0F);
        trap.left = left->position;
        trap.right = right->position;
        traps.m_traps.push_back(std::move(trap));
    }
    return traps;
}

std::optional<SpeedTrapPass> SpeedTraps::update(const glm::vec3& from, const glm::vec3& to, const glm::vec3& velocity)
{
    const float speed = glm::length(velocity);
    for (std::size_t i = 0; i < m_traps.size(); ++i) {
        SpeedTrap& trap = m_traps[i];
        if (!crosses(ground(from), ground(to), ground(trap.left), ground(trap.right))) {
            continue;
        }
        if (speed * kMsToMph < trap.minimumSpeedMph) {
            return std::nullopt;
        }
        SpeedTrapPass pass;
        pass.trap = i;
        pass.speed = speed;
        pass.previousBest = trap.best.value_or(0.0F);
        trap.discovered = true;
        trap.best = std::max(pass.previousBest, speed);
        ++m_passes;
        m_last = pass;
        return pass;
    }
    return std::nullopt;
}

const SpeedTrap* SpeedTraps::find(std::string_view name) const
{
    for (const SpeedTrap& trap : m_traps) {
        if (iequals(trap.name, name)) {
            return &trap;
        }
    }
    return nullptr;
}

bool SpeedTraps::restore(std::string_view name, bool discovered, float personalBest)
{
    for (SpeedTrap& trap : m_traps) {
        if (iequals(trap.name, name)) {
            trap.discovered = discovered;
            trap.best = personalBest > 0.0F ? std::optional<float>(personalBest) : std::nullopt;
            return true;
        }
    }
    return false;
}

} // namespace openhorizon
