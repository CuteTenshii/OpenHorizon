#include "EnvironmentCubeMath.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace fh::render::envcube {

namespace {

/// Per face, in the game's coordinates: the axis GL addresses along s (sc),
/// along t (tc) and the major axis (ma), from the OpenGL cube map table.
struct FaceBasis {
    glm::vec3 s;
    glm::vec3 t;
    glm::vec3 axis;
};

constexpr std::array<FaceBasis, 6> kBases{{
    {{0.0F, 0.0F, -1.0F}, {0.0F, -1.0F, 0.0F}, {1.0F, 0.0F, 0.0F}},
    {{0.0F, 0.0F, 1.0F}, {0.0F, -1.0F, 0.0F}, {-1.0F, 0.0F, 0.0F}},
    {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, {0.0F, 1.0F, 0.0F}},
    {{1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, -1.0F}, {0.0F, -1.0F, 0.0F}},
    {{1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F}},
    {{-1.0F, 0.0F, 0.0F}, {0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, -1.0F}},
}};

/// The game's coordinates are this renderer's with z negated.
glm::vec3 mirrorZ(const glm::vec3& v)
{
    return {v.x, v.y, -v.z};
}

} // namespace

int facesPerFrame(float envMapFrequencyScale)
{
    if (!(envMapFrequencyScale > 0.0F)) {
        return 0;
    }
    // fctiwz: truncation towards zero, then the clamp to 1..6.
    const float scaled = std::trunc(envMapFrequencyScale * 6.0F);
    if (scaled < 1.0F) {
        return 1;
    }
    return scaled > 6.0F ? 6 : static_cast<int>(scaled);
}

FaceSchedule::Frame FaceSchedule::next(float envMapFrequencyScale)
{
    Frame frame;
    frame.count = facesPerFrame(envMapFrequencyScale);
    if (frame.count == 0) {
        return frame;
    }
    for (int i = 0; i < frame.count; ++i) {
        const int face = kFaceOrder[static_cast<std::size_t>((m_cursor + i) % 6)];
        frame.faces[static_cast<std::size_t>(i)] = face;
        m_drawn |= 1U << static_cast<unsigned>(face);
    }
    m_cursor = (m_cursor + frame.count) % 6;
    m_mipsBuilt = false;
    return frame;
}

void FaceSchedule::reset()
{
    m_drawn = 0;
    m_mipsBuilt = false;
}

glm::vec3 centre(const glm::mat4& body)
{
    return glm::vec3(body[3]) + glm::vec3(0.0F, kCentreHeight, 0.0F);
}

glm::mat4 faceView(int face, const glm::vec3& centre)
{
    const FaceBasis& basis = kBases[static_cast<std::size_t>(std::clamp(face, 0, 5))];
    // Rows s, t and axis of the basis, applied to the game's coordinates of
    // a point relative to the centre: each row takes this renderer's z
    // negated.
    const glm::vec3 rows[3] = {mirrorZ(basis.s), mirrorZ(basis.t), mirrorZ(basis.axis)};
    glm::mat4 view(1.0F);
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            view[c][r] = rows[r][c];
        }
        view[3][r] = -glm::dot(rows[r], centre);
    }
    return view;
}

glm::mat4 faceProjection()
{
    // Depth (a z + b) / z: 1 at the near plane, 0 at the far one.
    const float b = 1.0F / (1.0F / kNear - 1.0F / kFar);
    const float a = -b / kFar;
    glm::mat4 projection(0.0F);
    projection[0][0] = 1.0F;
    projection[1][1] = 1.0F;
    projection[2][2] = a;
    projection[3][2] = b;
    projection[2][3] = 1.0F;
    return projection;
}

glm::vec3 sphereDirection(float u, float v)
{
    constexpr float kPi = 3.14159265358979F;
    const float p = u * 2.0F * kPi - kPi;
    const float t = v * kPi;
    return {std::sin(t) * std::cos(p), std::cos(t), std::sin(t) * std::sin(p)};
}

glm::vec2 sphereCoordinates(const glm::vec3& d)
{
    constexpr float kInverseTwoPi = 0.159154943F;
    constexpr float kInversePi = 0.318309886F;
    return {gameAtan2(d.z, d.x) * kInverseTwoPi + 0.5F, gameAcos(std::clamp(d.y, -1.0F, 1.0F)) * kInversePi};
}

float gameAtan2(float y, float x)
{
    const float ax = std::abs(x);
    const float ay = std::abs(y);
    const float r = std::min(ax, ay) / std::max(std::max(ax, ay), 1e-30F);
    const float r2 = r * r;
    float a = ((((0.0208351F * r2 - 0.085133F) * r2 + 0.180141F) * r2 - 0.3302995F) * r2 + 0.999866F) * r;
    a = ay > ax ? 1.570796327F - a : a;
    a = x < 0.0F ? 3.141592654F - a : a;
    return y < 0.0F ? -a : a;
}

float gameAcos(float x)
{
    const float ax = std::abs(x);
    float a = ((-0.0187293F * ax + 0.0742610F) * ax - 0.2121144F) * ax + 1.5707288F;
    a *= std::sqrt(std::max(1.0F - ax, 0.0F));
    return x < 0.0F ? 3.141592654F - a : a;
}

glm::vec3 faceDirection(int face, float s, float t)
{
    const FaceBasis& basis = kBases[static_cast<std::size_t>(std::clamp(face, 0, 5))];
    return glm::normalize(basis.axis + (2.0F * s - 1.0F) * basis.s + (2.0F * t - 1.0F) * basis.t);
}

} // namespace fh::render::envcube
