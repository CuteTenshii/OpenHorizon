#include "AiDriver.h"

#include "base/Log.h"

#include <algorithm>
#include <cmath>

namespace openhorizon {

namespace {

constexpr float kGravity = 9.80665F;
constexpr float kDegToRad = 3.14159265F / 180.0F;
/// Points either side used to measure the line's curvature, about 8 m.
constexpr int kCurvatureSpan = 4;

/// Curvature of the circle through three points (0 when they are in line).
float curvature(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c)
{
    const glm::vec2 p(a.x, a.z);
    const glm::vec2 q(b.x, b.z);
    const glm::vec2 r(c.x, c.z);
    const float ab = glm::length(q - p);
    const float bc = glm::length(r - q);
    const float ca = glm::length(p - r);
    const float twiceArea = std::abs((q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x));
    const float denominator = ab * bc * ca;
    return denominator > 1e-6F ? 2.0F * twiceArea / denominator : 0.0F;
}

} // namespace

AiDriver::AiDriver(const RacingLine& line, const CarSpec& spec, int skill, float laneOffset)
    : m_line(line)
    , m_spec(spec)
    , m_laneOffset(laneOffset)
    , m_skill(std::clamp(static_cast<float>(skill) / 100.0F, 0.2F, 1.0F))
{
    const std::size_t n = m_line.points.size();
    m_speedLimit.assign(n, 90.0F);
    if (n < 3) {
        return;
    }
    // Corner speed from the grip the car has, discounted by skill: a weak
    // driver leaves a margin a strong one does not.
    const float grip = (spec.reference.lateralG60Mph > 0.3F ? spec.reference.lateralG60Mph : 0.9F) * kGravity;
    const float usable = grip * (0.72F + 0.25F * m_skill);
    const float braking = grip * (0.6F + 0.3F * m_skill);
    const auto at = [&](long i) -> const glm::vec3& {
        const auto count = static_cast<long>(n);
        if (m_line.loop) {
            return m_line.points[static_cast<std::size_t>(((i % count) + count) % count)].position;
        }
        return m_line.points[static_cast<std::size_t>(std::clamp(i, 0L, count - 1))].position;
    };
    for (std::size_t i = 0; i < n; ++i) {
        const auto k = static_cast<long>(i);
        const float kappa = curvature(at(k - kCurvatureSpan), at(k), at(k + kCurvatureSpan));
        m_speedLimit[i] = kappa > 1e-5F ? std::min(90.0F, std::sqrt(usable / kappa)) : 90.0F;
    }
    // Brake in time for each corner: walk backwards so a slow point limits
    // the points before it by the braking distance. Loops need two passes to
    // carry the limit across the start.
    const int passes = m_line.loop ? 2 : 1;
    for (int pass = 0; pass < passes; ++pass) {
        for (std::size_t step = 0; step < n; ++step) {
            const std::size_t i = n - 1 - step;
            std::size_t next = i + 1;
            if (next >= n) {
                if (!m_line.loop) {
                    continue;
                }
                next = 0;
            }
            const float ds = glm::length(m_line.points[next].position - m_line.points[i].position);
            const float reachable = std::sqrt(m_speedLimit[next] * m_speedLimit[next] + 2.0F * braking * ds);
            m_speedLimit[i] = std::min(m_speedLimit[i], reachable);
        }
    }
    std::vector<float> sorted = m_speedLimit;
    std::sort(sorted.begin(), sorted.end());
    logDebug("ai: speed profile over {} points: min {:.1f}, median {:.1f}, max {:.1f} m/s", n, sorted.front(),
        sorted[n / 2], sorted.back());
}

float AiDriver::speedLimitAt(float index) const
{
    const auto n = static_cast<float>(m_speedLimit.size());
    if (m_line.loop) {
        index = std::fmod(index, n);
        if (index < 0.0F) {
            index += n;
        }
    } else {
        index = std::clamp(index, 0.0F, n - 1.0F);
    }
    return m_speedLimit[static_cast<std::size_t>(index)];
}

CarControls AiDriver::update(const CarPhysics& car, float dt, float speedScale)
{
    CarControls controls;
    const glm::vec3 position = car.position();
    const float count = static_cast<float>(m_line.points.size());
    if (m_index < 0.0F) {
        m_index = m_line.project(position);
    } else {
        // Keep the unwrapped count of loops while projecting near the last
        // known point.
        const float laps = std::floor(m_index / count);
        const float local = m_index - laps * count;
        float projected = m_line.project(position, local);
        if (m_line.loop && projected < local - count * 0.5F) {
            projected += count;
        }
        m_index = laps * count + projected;
    }

    const float speed = car.forwardSpeed();
    const float lookAheadMetres = 5.0F + std::abs(speed) * 0.55F;
    const float aheadIndex = m_line.advance(m_index, lookAheadMetres);
    glm::vec3 target = m_line.positionAt(aheadIndex);
    const glm::vec3 direction = m_line.directionAt(aheadIndex);
    const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), direction));
    target += side * m_laneOffset;

    // Pure pursuit: the arc through the target point gives the steering
    // angle, scaled to the car's full lock.
    const glm::vec3 local = glm::inverse(car.orientation()) * (target - position);
    const float distance = std::max(glm::length(glm::vec2(local.x, local.z)), 1.0F);
    const float alpha = std::atan2(local.x, local.z);
    const float wheelAngle = std::atan(2.0F * m_spec.wheelbase * std::sin(alpha) / distance);
    const float fullLock = std::max(m_spec.steerMaxAngleFilteredDeg, 10.0F) * kDegToRad;
    controls.steer = std::clamp(wheelAngle / fullLock, -1.0F, 1.0F);

    // Speed: the profile a little ahead, since the car needs time to react.
    const float reactionIndex = m_line.advance(m_index, std::abs(speed) * 0.35F + 2.0F);
    m_targetSpeed = std::min({speedLimitAt(m_index), speedLimitAt(reactionIndex), m_maxSpeed}) * speedScale;
    const float error = m_targetSpeed - speed;
    if (error > 0.0F) {
        controls.throttle = std::clamp(error * 0.4F + 0.3F, 0.0F, 1.0F);
    } else if (error < -1.5F) {
        controls.brake = std::clamp(-error * 0.25F, 0.0F, 1.0F);
    }
    // Ease off when the target is far to the side, as a driver would.
    controls.throttle *= std::clamp(1.2F - std::abs(alpha), 0.3F, 1.0F);

    // Stuck against something: back up with the wheel turned the other way.
    if (speedScale > 0.0F && std::abs(speed) < 1.0F && m_reverseTime <= 0.0F) {
        m_stuckTime += dt;
        if (m_stuckTime > 2.5F && std::fmod(m_stuckTime, 5.0F) < dt) {
            m_reverseTime = 1.8F;
        }
    } else if (std::abs(speed) > 3.0F) {
        m_stuckTime = 0.0F;
    }
    if (m_reverseTime > 0.0F) {
        m_reverseTime -= dt;
        controls.throttle = 0.0F;
        controls.brake = 1.0F;
        controls.steer = -controls.steer;
    }
    if (speedScale <= 0.0F) {
        // Held on the handbrake: the foot brake at a standstill makes the
        // automatic gearbox select reverse.
        controls.throttle = 0.0F;
        controls.brake = 0.0F;
        controls.handbrake = 1.0F;
    }
    return controls;
}

} // namespace openhorizon
