// The driving line assist's rules as the game's builder (0x82618c10) has
// them: the chevron colours for the speed margin, the alpha along the line,
// the chevrons' size and placement, and the draw-out after a restart. On
// synthetic lines; no disc needed.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "game/DrivingLine.h"
#include "game/RaceData.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace {

using Rgb = std::array<std::uint8_t, 3>;

/// A straight line north along +z with the game's usual 2 m spacing.
fh::RacingLine straightLine(int points)
{
    fh::RacingLine line;
    line.points.reserve(static_cast<std::size_t>(points));
    for (int i = 0; i < points; ++i) {
        fh::RacingLine::Point point;
        point.position = glm::vec3(0.0F, 0.0F, 2.0F * static_cast<float>(i));
        point.distance = point.position.z;
        line.points.push_back(point);
    }
    line.length = line.points.back().distance;
    return line;
}

float constantSpeed(float /*index*/)
{
    return 20.0F;
}

TEST_CASE("drivingLineColoursFollowTheSpeedMargin", "[race]")
{
    // Amber at the target and just under it, down to the braking margin.
    CHECK(fh::DrivingLine::colour(0.0F) == (Rgb{192, 164, 52}));
    CHECK(fh::DrivingLine::colour(-0.5F) == (Rgb{192, 164, 52}));
    // 5 m/s either way is full strength: red when braking is needed...
    CHECK(fh::DrivingLine::colour(-5.0F) == (Rgb{192, 39, 52}));
    CHECK(fh::DrivingLine::colour(-20.0F) == (Rgb{192, 39, 52}));
    // ...green when there is speed to gain.
    CHECK(fh::DrivingLine::colour(5.0F) == (Rgb{44, 147, 44}));
    // Halfway, truncated as the game converts: 192 - 74, 164 - 8.5, 52 - 4.
    CHECK(fh::DrivingLine::colour(2.5F) == (Rgb{118, 155, 48}));
}

TEST_CASE("drivingLineAlphaAlongTheLine", "[race]")
{
    // Faint behind the car, rising from 16 points after the first, full
    // from 22 and fading out by 102; the Full setting's (a * 255) >> 8.
    CHECK(fh::DrivingLine::positionAlpha(0, 0.0F) == 1);
    CHECK(fh::DrivingLine::positionAlpha(12, 0.0F) == 31);
    CHECK(fh::DrivingLine::positionAlpha(15, 0.5F) == 31);
    CHECK(fh::DrivingLine::positionAlpha(18, 0.0F) == 142);
    CHECK(fh::DrivingLine::positionAlpha(30, 0.0F) == 254);
    CHECK(fh::DrivingLine::positionAlpha(90, 0.0F) == 139);
    CHECK(fh::DrivingLine::positionAlpha(101, 0.0F) == 0);
}

TEST_CASE("drivingLineChevronsSitOnTheLine", "[race]")
{
    const fh::RacingLine line = straightLine(400);
    fh::DrivingLine drivingLine(1.0F);
    drivingLine.update(1.0F);
    const std::vector<fh::DrivingLine::Vertex> vertices = drivingLine.build(line, 100.25F, 20.0F, constantSpeed);
    // Even points 88 to 200, each towards the point two on.
    CHECK(vertices.size() == 57 * 4);
    if (vertices.size() != 57 * 4) {
        return;
    }
    const fh::DrivingLine::Vertex& a = vertices[0];
    const fh::DrivingLine::Vertex& b = vertices[1];
    const fh::DrivingLine::Vertex& c = vertices[2];
    CHECK_THAT(a.position.z, WithinAbs(176.0, 1e-4));
    CHECK_THAT(a.position.y, WithinAbs(0.1, 1e-5));
    CHECK_THAT(glm::length(a.position - b.position), WithinAbs(1.2, 1e-5));
    CHECK_THAT(c.position.z - b.position.z, WithinAbs(2.4, 1e-5));
    CHECK(a.uv == glm::vec2(0.0F, 1.0F));
    CHECK(c.uv == glm::vec2(1.0F, 0.0F));
    // At the target speed every chevron is amber.
    for (const fh::DrivingLine::Vertex& v : vertices) {
        CHECK((v.colour[0] == 192 && v.colour[1] == 164 && v.colour[2] == 52));
    }
    // The chevron 30 points after the first is at full brightness.
    CHECK(vertices[15 * 4].colour[3] > 200);
}

TEST_CASE("drivingLineDrawsOutAfterARestart", "[race]")
{
    const fh::RacingLine line = straightLine(400);
    fh::DrivingLine drivingLine(1.0F);
    drivingLine.restart();
    drivingLine.update(0.5F);
    const std::vector<fh::DrivingLine::Vertex> vertices = drivingLine.build(line, 100.0F, 20.0F, constantSpeed);
    // Half a second in, the chevrons up to 25.5 of 51 from the first show.
    CHECK(vertices.size() == 57 * 4);
    if (vertices.size() != 57 * 4) {
        return;
    }
    CHECK(vertices[25 * 4].colour[3] > 0);
    CHECK(vertices[26 * 4].colour[3] == 0);
}

TEST_CASE("drivingLineStopsAtTheEndOfAnOpenLine", "[race]")
{
    const fh::RacingLine line = straightLine(60);
    fh::DrivingLine drivingLine(1.0F);
    drivingLine.update(1.0F);
    const std::vector<fh::DrivingLine::Vertex> vertices = drivingLine.build(line, 50.0F, 20.0F, constantSpeed);
    // Points 38 to 56 start chevrons that end by the last point, 59.
    CHECK(vertices.size() == 10 * 4);
}

} // namespace
