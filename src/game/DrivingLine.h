#pragma once

#include "game/RaceData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace openhorizon {

/// The driving line assist ("suggested line", the tunables' "GreenLine"):
/// a row of chevrons along the racing line ahead of the car, coloured by
/// how the car's speed compares with the speed the line can be taken at.
/// Transcribed from the game's builder (0x82618c10, called each update by
/// the open-world AI component at 0x826ad5c0) and its draw (0x82430e20).
///
/// Verified against the executable:
/// - the points from 12 behind the car's point on the line to 102 ahead
///   (0x82618cfc), one chevron from every even point index 2i towards
///   point 2i + 2;
/// - a chevron is 1.2 m wide (half width 0.6 at 0x82029ee8), covers the
///   first 60% of its two-point segment and sits 0.10 m above the line
///   (two lifts of 0.05, 0x82000cd0);
/// - the colour rule and its constants (0x820296a0-0x820296d0, 0.2 at
///   0x82000d68, -1 at 0x82000d40), the alpha by position along the line
///   and the reveal over TrackPositioning/GreenLine/ResetBlendSpeed seconds
///   (globalregistry.xml, 1.0) after the route changes;
/// - the target speed is asked for at point 2i + 5.
///
/// Inferred or missing:
/// - the game's target speed comes from its speed planner (0x824a4e40),
///   whose internals were not traced; the caller passes its own target
///   speed along the line instead;
/// - the game builds each chevron's up vector from the route point's side
///   vector, which the racing line files here do not give; the side is
///   taken as horizontal;
/// - the game's route can override point heights over index ranges from a
///   list at 0x8342dd20 whose meaning was not traced; that is left out;
/// - the "Braking" setting (alpha fades where the car could go faster) is
///   not offered: the profile default, Full (ForzaProfile.sch), is used.
class DrivingLine {
public:
    /// One corner of a chevron, as the game's RaceLineDecl lays it out:
    /// position, D3DCOLOR and texture coordinate, 24 bytes.
    struct Vertex {
        glm::vec3 position{0.0F};
        /// R, G, B, A, 0-255.
        std::array<std::uint8_t, 4> colour{};
        glm::vec2 uv{0.0F};
    };

    /// Points behind and ahead of the car's point that carry chevrons.
    static constexpr int kPointsBehind = 12;
    static constexpr int kPointsAhead = 102;

    /// `revealSeconds` is TrackPositioning/GreenLine/ResetBlendSpeed.
    explicit DrivingLine(float revealSeconds);

    /// Starts the reveal again, as the game does when its route or mode
    /// changes.
    void restart() { m_revealTime = 0.0F; }
    void update(float dt) { m_revealTime += dt; }

    /// The chevrons along `line` around the car at fractional point
    /// `carIndex` (counting up past the end on loops) going at `speed`
    /// m/s; `targetSpeed` gives the speed a point of the line can be taken
    /// at, in m/s. Four vertices per chevron, in quad order.
    std::vector<Vertex> build(
        const RacingLine& line, float carIndex, float speed, const std::function<float(float)>& targetSpeed) const;

    /// The chevron colour for the target speed minus the car's speed
    /// (0x826195f0): amber at the target, greener the more speed there is
    /// to gain, redder the more there is to lose. R, G, B.
    static std::array<std::uint8_t, 3> colour(float speedMargin);
    /// The alpha of the chevron `offset` points after the first point
    /// carrying one, with the car `fraction` of the way to its next point:
    /// faint behind the car, rising in front of it and fading out towards
    /// the end.
    static std::uint8_t positionAlpha(int offset, float fraction);

private:
    float m_revealSeconds = 1.0F;
    float m_revealTime = 0.0F;
};

} // namespace openhorizon
