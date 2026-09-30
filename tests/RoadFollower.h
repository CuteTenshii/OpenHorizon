#pragma once

// Drives the player's car along the road network, for tests that need a
// drive over real roads: the frame-time test and the streaming scenario.

#include "car/CarPhysics.h"
#include "platform/Input.h"
#include "ui/Navigation.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fh::test {

/// Drives along the road network: follows a way to its end and there takes
/// the way through the end node that turns least, as a driver cruising the
/// roads would. The car stays on the roads the minimap draws, so it keeps
/// discovering them.
class RoadFollower {
public:
    explicit RoadFollower(const fh::ui::Navigation& navigation)
        : m_navigation(navigation)
    {
    }

    /// Controls for this frame, and whether the car needs putting back on
    /// the road (it has been stuck for a while).
    fh::InputState controls(const fh::CarPhysics& car)
    {
        fh::InputState input;
        const glm::vec3 position = car.position();
        const glm::vec2 ground(position.x, position.z);
        const glm::vec2 forward(car.forward().x, car.forward().z);
        m_stuck = car.speed() < 1.0F ? m_stuck + kStep : 0.0F;
        if (m_stuck > 4.0F) {
            input.keys[static_cast<std::size_t>(fh::Key::R)] = true;
            m_stuck = 0.0F;
            m_way.reset();
            return input;
        }
        if (!m_way && !start(position, forward)) {
            input.axes[static_cast<std::size_t>(fh::Axis::RightTrigger)] = 0.3F;
            return input;
        }
        // A point ahead on the road to steer at.
        for (int guard = 0; guard < 64 && glm::length(point() - ground) < kLookahead; ++guard) {
            advance();
        }
        const float error = std::remainder(headingOf(point() - ground) - headingOf(forward), 2.0F * kPi);
        const float steer = std::clamp(error * 1.5F, -1.0F, 1.0F);
        const float target = kCruise * (1.0F - 0.6F * std::min(std::abs(error), 1.0F));
        const float speed = car.speed();
        input.axes[static_cast<std::size_t>(fh::Axis::LeftX)] = steer;
        input.axes[static_cast<std::size_t>(fh::Axis::RightTrigger)] = speed < target ? 0.7F : 0.0F;
        input.axes[static_cast<std::size_t>(fh::Axis::LeftTrigger)] = speed > target + 4.0F ? 0.5F : 0.0F;
        return input;
    }

private:
    /// The fixed step the tests run the game at.
    static constexpr float kStep = 1.0F / 60.0F;
    static constexpr float kPi = 3.14159265F;
    static constexpr float kLookahead = 18.0F;
    static constexpr float kCruise = 22.0F;

    static float headingOf(glm::vec2 direction) { return std::atan2(direction.x, direction.y); }

    glm::vec2 point() const { return m_navigation.network().ways[*m_way].points[m_index]; }

    bool start(const glm::vec3& position, glm::vec2 forward)
    {
        const std::optional<std::uint32_t> node
            = m_navigation.locator().nearest(position, fh::ui::NavFilter::NoShortcutOrNone);
        if (!node) {
            return false;
        }
        return turnAt(*node, forward, std::nullopt);
    }

    /// Takes the way through `node` whose next point lies most nearly along
    /// `direction`, other than `from`.
    bool turnAt(std::uint32_t node, glm::vec2 direction, std::optional<std::uint32_t> from)
    {
        const fh::ui::NavFile& network = m_navigation.network();
        float best = -2.0F;
        bool found = false;
        for (const fh::ui::NavLink& link : network.nodes[node].links) {
            if (link.way == from || !m_navigation.locator().acceptsWay(link.way, fh::ui::NavFilter::NoShortcutOrNone)) {
                continue;
            }
            const std::vector<glm::vec2>& points = network.ways[link.way].points;
            for (const int step : {-1, 1}) {
                const long next = static_cast<long>(link.position) + step;
                if (next < 0 || next >= static_cast<long>(points.size())) {
                    continue;
                }
                const glm::vec2 along = points[static_cast<std::size_t>(next)] - points[link.position];
                if (glm::length(along) <= 0.0F || glm::length(direction) <= 0.0F) {
                    continue;
                }
                const float score = glm::dot(glm::normalize(along), glm::normalize(direction));
                if (score > best) {
                    best = score;
                    m_way = link.way;
                    m_index = static_cast<std::size_t>(next);
                    m_step = step;
                    found = true;
                }
            }
        }
        return found;
    }

    void advance()
    {
        const fh::ui::NavWay& way = m_navigation.network().ways[*m_way];
        const long next = static_cast<long>(m_index) + m_step;
        if (next >= 0 && next < static_cast<long>(way.points.size())) {
            m_index = static_cast<std::size_t>(next);
            return;
        }
        if (way.points.size() < 2) {
            m_step = -m_step;
            return;
        }
        const std::size_t previous = m_step > 0 ? m_index - 1 : m_index + 1;
        const glm::vec2 direction = way.points[m_index] - way.points[previous];
        if (!turnAt(way.nodes[m_index], direction, m_way)) {
            // A dead end: back the way it came.
            m_step = -m_step;
        }
    }

    const fh::ui::Navigation& m_navigation;
    std::optional<std::uint32_t> m_way;
    std::size_t m_index = 0;
    int m_step = 1;
    float m_stuck = 0.0F;
};

} // namespace fh::test
