#pragma once

#include "game/RaceData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace openhorizon {

/// The driving line assist ("GreenLine"): chevrons along the racing line
/// ahead of the car, coloured by the car's speed against the line's.
/// Verified: builder 0x82618c10 (called by 0x826ad5c0) and draw 0x82430e20;
/// 1.2 m wide (0x82029ee8), 0.10 m up (0x82000cd0); colour constants
/// 0x820296a0-0x820296d0, 0x82000d68, 0x82000d40; target speed at point 2i + 5.
/// Missing: the speed planner (0x824a4e40), so the caller's target speed is
/// used; the route's side vector, so the side is horizontal; the height
/// overrides at 0x8342dd20; the Braking setting, so Full is used.
class DrivingLine {
public:
    /// One corner of a chevron in the game's RaceLineDecl layout, 24 bytes.
    struct Vertex {
        glm::vec3 position{0.0F};
        /// R, G, B, A, 0-255.
        std::array<std::uint8_t, 4> colour{};
        glm::vec2 uv{0.0F};
    };

    /// Points behind and ahead of the car's point that carry chevrons
    /// (0x82618cfc).
    static constexpr int kPointsBehind = 12;
    static constexpr int kPointsAhead = 102;

    /// `revealSeconds` is TrackPositioning/GreenLine/ResetBlendSpeed
    /// (globalregistry.xml).
    explicit DrivingLine(float revealSeconds);

    /// Starts the reveal again, as a route or mode change does.
    void restart() { m_revealTime = 0.0F; }
    void update(float dt) { m_revealTime += dt; }

    /// Four vertices per chevron, in quad order. `carIndex` keeps counting
    /// past the end on loops; speeds are in m/s.
    std::vector<Vertex> build(
        const RacingLine& line, float carIndex, float speed, const std::function<float(float)>& targetSpeed) const;

    /// R, G, B for the target speed minus the car's (0x826195f0): amber at
    /// the target, greener with speed to gain, redder with speed to lose.
    static std::array<std::uint8_t, 3> colour(float speedMargin);
    /// Alpha of the chevron `offset` points after the first, the car being
    /// `fraction` of the way to its next point.
    static std::uint8_t positionAlpha(int offset, float fraction);

private:
    float m_revealSeconds = 1.0F;
    float m_revealTime = 0.0F;
};

} // namespace openhorizon
