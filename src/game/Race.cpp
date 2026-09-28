#include "Race.h"

#include <algorithm>
#include <cmath>

namespace fh {

Race::Race(RaceEvent event, RaceRoute route, RacingLine line)
    : m_event(std::move(event))
    , m_route(std::move(route))
    , m_line(std::move(line))
{
    // Circuits start and finish at the finish trigger; sprints start at the
    // beginning of their line and end at the trigger.
    if (m_line.loop) {
        m_startDistance = m_route.finish ? m_line.distanceAt(m_line.project(m_route.finish->position)) : 0.0F;
        m_distance = m_line.length * static_cast<float>(m_event.laps);
    } else {
        m_startDistance = m_route.grid.empty() ? 0.0F : m_line.distanceAt(m_line.project(m_route.grid.front().position));
        const float end = m_route.finish ? m_line.distanceAt(m_line.project(m_route.finish->position)) : m_line.length;
        m_distance = std::max(end - m_startDistance, 50.0F);
    }
}

void Race::addRacer(std::size_t vehicle, bool player, const glm::vec3& position)
{
    Racer racer;
    racer.vehicle = vehicle;
    racer.player = player;
    racer.lineIndex = m_line.project(position);
    float progress = m_line.distanceAt(racer.lineIndex) - m_startDistance;
    // A grid behind a circuit's start line projects near the end of the
    // loop; count it as just behind the line.
    if (m_line.loop && progress > m_line.length * 0.5F) {
        racer.lineIndex -= static_cast<float>(m_line.points.size());
        progress -= m_line.length;
    }
    racer.progress = progress;
    m_racers.push_back(racer);
}

void Race::update(float dt, const std::vector<glm::vec3>& positions)
{
    if (m_phase == Phase::Countdown) {
        m_countdown -= dt;
        if (m_countdown <= 0.0F) {
            m_countdown = 0.0F;
            m_phase = Phase::Racing;
        }
    } else if (m_phase == Phase::Racing) {
        m_elapsed += dt;
    }
    const float count = static_cast<float>(m_line.points.size());
    for (Racer& racer : m_racers) {
        if (racer.vehicle >= positions.size()) {
            continue;
        }
        // Project near the last known point, so cutting across a hairpin or
        // an infield cannot jump a racer ahead.
        const float laps = std::floor(racer.lineIndex / count);
        const float local = racer.lineIndex - laps * count;
        float projected = m_line.project(positions[racer.vehicle], local, 80);
        if (m_line.loop) {
            if (projected < local - count * 0.5F) {
                projected += count;
            } else if (projected > local + count * 0.5F) {
                projected -= count;
            }
        }
        racer.lineIndex = laps * count + projected;
        racer.progress = m_line.distanceAt(racer.lineIndex) - m_startDistance;
        if (m_phase == Phase::Racing && !racer.finished && racer.progress >= m_distance) {
            racer.finished = true;
            racer.finishTime = m_elapsed;
            racer.finishPlace = ++m_finishedCount;
            if (racer.player) {
                m_phase = Phase::Finished;
            }
        }
    }
    if (m_phase == Phase::Finished) {
        m_elapsed += dt;
    }
}

const Race::Racer* Race::player() const
{
    for (const Racer& racer : m_racers) {
        if (racer.player) {
            return &racer;
        }
    }
    return nullptr;
}

std::vector<const Race::Racer*> Race::standings() const
{
    std::vector<const Racer*> order;
    for (const Racer& racer : m_racers) {
        order.push_back(&racer);
    }
    std::stable_sort(order.begin(), order.end(), [](const Racer* a, const Racer* b) {
        if (a->finished != b->finished) {
            return a->finished;
        }
        if (a->finished) {
            return a->finishPlace < b->finishPlace;
        }
        return a->progress > b->progress;
    });
    return order;
}

int Race::place(const Racer& racer) const
{
    const std::vector<const Racer*> order = standings();
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == &racer) {
            return static_cast<int>(i) + 1;
        }
    }
    return static_cast<int>(order.size());
}

int Race::lap(const Racer& racer) const
{
    if (!m_line.loop || m_line.length <= 0.0F) {
        return 1;
    }
    const int lap = static_cast<int>(std::floor(std::max(racer.progress, 0.0F) / m_line.length)) + 1;
    return std::clamp(lap, 1, m_event.laps);
}

} // namespace fh
