#pragma once

#include "game/RaceData.h"

#include <glm/glm.hpp>

#include <vector>

namespace openhorizon {

/// The rules of one race: the countdown, each racer's progress along the
/// route, laps, positions and the finish. It knows racers only by index and
/// position; the game owns the cars.
class Race {
public:
    enum class Phase { Countdown, Racing, Finished };
    static constexpr float kCountdownSeconds = 3.0F;

    struct Racer {
        /// The caller's index for the car.
        std::size_t vehicle = 0;
        bool player = false;
        /// Fractional point index on the line, counting up past the end of
        /// a loop.
        float lineIndex = 0.0F;
        /// Metres covered since the start line; negative behind it.
        float progress = 0.0F;
        bool finished = false;
        float finishTime = 0.0F;
        int finishPlace = 0;
    };

    Race(RaceEvent event, RaceRoute route, RacingLine line);

    /// Adds a racer standing at `position` on the grid.
    void addRacer(std::size_t vehicle, bool player, const glm::vec3& position);
    /// Advances the clock and every racer's progress; `positions` is indexed
    /// by vehicle.
    void update(float dt, const std::vector<glm::vec3>& positions);

    Phase phase() const { return m_phase; }
    /// Seconds left before the start, while counting down.
    float countdown() const { return m_countdown; }
    /// Seconds since the start.
    float elapsed() const { return m_elapsed; }
    bool started() const { return m_phase != Phase::Countdown; }

    const RaceEvent& event() const { return m_event; }
    const RaceRoute& route() const { return m_route; }
    const RacingLine& line() const { return m_line; }
    const std::vector<Racer>& racers() const { return m_racers; }
    const Racer* player() const;
    /// 1-based place of a racer: by finishing order, then by progress.
    int place(const Racer& racer) const;
    /// The racer's lap, 1-based, clamped to the race's laps.
    int lap(const Racer& racer) const;
    /// Metres from the start line to the finish.
    float distance() const { return m_distance; }
    /// Racers in their current order.
    std::vector<const Racer*> standings() const;

private:
    RaceEvent m_event;
    RaceRoute m_route;
    RacingLine m_line;
    std::vector<Racer> m_racers;
    Phase m_phase = Phase::Countdown;
    float m_countdown = kCountdownSeconds;
    float m_elapsed = 0.0F;
    /// Distance along the line of the start line, and the race length.
    float m_startDistance = 0.0F;
    float m_distance = 0.0F;
    int m_finishedCount = 0;
};

} // namespace openhorizon
