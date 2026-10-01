#include "DrivingLine.h"

#include <algorithm>
#include <cmath>

namespace openhorizon {

namespace {

/// Half the chevron's width (0x82029ee8).
constexpr float kHalfWidth = 0.6F;
/// Applied twice along the up vector (0x82000cd0).
constexpr float kLift = 0.05F;
/// Share of the two-point segment a chevron covers (0x82431238-0x82431390).
constexpr float kLength = 0.6F;
constexpr float kRevealChevrons = 51.0F;
/// 0x820296a0-0x820296d0 and 0.2 at 0x82000d68; full strength at a 5 m/s
/// margin.
constexpr float kAmberR = 192.0F;
constexpr float kAmberG = 164.0F;
constexpr float kAmberB = 52.0F;
constexpr float kBrakeG = -125.0F;
constexpr float kGainR = -148.0F;
constexpr float kGainG = -17.0F;
constexpr float kGainB = -8.0F;
constexpr float kMarginScale = 0.2F;
/// Below this margin (m/s, 0x82000d40) the car has to slow down.
constexpr float kBrakeMargin = -1.0F;

std::uint8_t channel(float value)
{
    // The game converts with a truncating float-to-int.
    return static_cast<std::uint8_t>(std::clamp(static_cast<int>(value), 0, 255));
}

} // namespace

DrivingLine::DrivingLine(float revealSeconds)
    : m_revealSeconds(revealSeconds)
{
}

std::array<std::uint8_t, 3> DrivingLine::colour(float speedMargin)
{
    const float strength = std::clamp(std::abs(speedMargin) * kMarginScale, 0.0F, 1.0F);
    if (speedMargin <= kBrakeMargin) {
        return {channel(kAmberR), channel(kAmberG + kBrakeG * strength), channel(kAmberB)};
    }
    if (speedMargin <= 0.0F) {
        return {channel(kAmberR), channel(kAmberG), channel(kAmberB)};
    }
    return {channel(kAmberR + kGainR * strength), channel(kAmberG + kGainG * strength),
        channel(kAmberB + kGainB * strength)};
}

std::uint8_t DrivingLine::positionAlpha(int offset, float fraction)
{
    // 0x826198cc; the factors are the table at 0x820296a0.
    const float along = static_cast<float>(offset) + 1.0F - fraction;
    float alpha = 0.0F;
    if (offset < kPointsBehind) {
        alpha = std::min(std::max(0.0F, along * (8.0F / 3.0F)), 32.0F);
    } else if (offset < 16) {
        alpha = 32.0F;
    } else if (offset < 22) {
        alpha = std::clamp((along - 16.0F) * (223.0F / 6.0F) + 32.0F, 32.0F, 255.0F);
    } else {
        alpha = std::clamp((static_cast<float>(kPointsAhead) - along) * 12.75F, 0.0F, 255.0F);
    }
    // The Full setting's multiplier.
    return static_cast<std::uint8_t>((static_cast<int>(alpha) * 255) >> 8);
}

std::vector<DrivingLine::Vertex> DrivingLine::build(
    const RacingLine& line, float carIndex, float speed, const std::function<float(float)>& targetSpeed) const
{
    std::vector<Vertex> vertices;
    if (line.points.size() < 2) {
        return vertices;
    }
    const int last = static_cast<int>(line.points.size()) - 1;
    const auto carPoint = static_cast<int>(std::floor(carIndex));
    const float fraction = carIndex - static_cast<float>(carPoint);
    const int first = carPoint - kPointsBehind;
    const int end = carPoint + kPointsAhead;
    const int start = first + ((first % 2) + 2) % 2;
    const float reveal = m_revealSeconds > 0.0F ? std::min(m_revealTime / m_revealSeconds, 1.0F) : 1.0F;
    vertices.reserve(static_cast<std::size_t>((end - start) / 2 + 1) * 4);

    int chevron = 0;
    for (int index = start; index + 2 <= end; index += 2, ++chevron) {
        if (!line.loop && (index < 0 || index + 2 > last)) {
            continue;
        }
        const glm::vec3 point = line.positionAt(static_cast<float>(index));
        const glm::vec3 along = line.positionAt(static_cast<float>(index + 2)) - point;
        const glm::vec3 flatSide = glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), along);
        if (glm::length(flatSide) < 1.0e-4F) {
            continue;
        }
        glm::vec3 up = glm::normalize(glm::cross(along, glm::normalize(flatSide)));
        if (up.y < 0.0F) {
            up = -up;
        }
        const glm::vec3 across = glm::normalize(glm::cross(up, along)) * kHalfWidth;
        const glm::vec3 base = point + up * (2.0F * kLift);

        const std::array<std::uint8_t, 3> rgb = colour(targetSpeed(static_cast<float>(index + 5)) - speed);
        const bool hidden = static_cast<float>(chevron) / kRevealChevrons > reveal;
        const std::uint8_t alpha = hidden ? 0 : positionAlpha(index - first, fraction);
        const std::array<std::uint8_t, 4> rgba{rgb[0], rgb[1], rgb[2], alpha};

        vertices.push_back({base + across, rgba, {0.0F, 1.0F}});
        vertices.push_back({base - across, rgba, {1.0F, 1.0F}});
        vertices.push_back({base - across + along * kLength, rgba, {1.0F, 0.0F}});
        vertices.push_back({base + across + along * kLength, rgba, {0.0F, 0.0F}});
    }
    return vertices;
}

} // namespace openhorizon
