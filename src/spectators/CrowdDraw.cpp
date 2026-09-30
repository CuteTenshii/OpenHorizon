#include "CrowdDraw.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace openhorizon::spectators::draw {

namespace {

constexpr float kFrontCosine = 0.707F;
/// PROC_CHARACTER_BILLBOARD_VS's constants c202.x and c203.x, c252.w and
/// c202.z: the arc cosine polynomial.
constexpr float kArcCos0 = 1.5707288F;
constexpr float kArcCos1 = -0.2121144F;
constexpr float kArcCos2 = 0.074261F;
constexpr float kArcCos3 = -0.0187293F;
/// c253.z / pi and c254.x: the six-view column step (8 / pi, a column every
/// 22.5 degrees) and its rounding bias.
constexpr float kSixViewSteps = 8.0F;
constexpr float kSixViewBias = 1.5000114F;
constexpr std::uint32_t kRandomMultiplier = 0x41C64E6DU;
constexpr std::uint32_t kRandomIncrement = 0x3039U;
constexpr std::uint32_t kCrtMultiplier = 214013U;
constexpr std::uint32_t kCrtIncrement = 2531011U;
constexpr std::uint32_t kRandomMask = 0x7FFFU;
constexpr float kRandomMax = 32767.0F;

float saturate(float value)
{
    return std::clamp(value, 0.0F, 1.0F);
}

float ramp(float value, float from, float to)
{
    // The shader divides by (to - from) through a reciprocal; the tables
    // never make it zero.
    return saturate((value - from) / (to - from));
}

} // namespace

float modelFade(float distance, const glm::vec4& fade)
{
    return ramp(distance, fade.x, fade.y);
}

float spriteFade(float distance, bool billboardOnly, const glm::vec4& fade)
{
    const float in = ramp(distance, fade.x, fade.y);
    const float out = ramp(distance, fade.z, fade.w);
    return billboardOnly ? out : std::min(in, out);
}

std::vector<int> modelLods(std::span<const float> distances, int lodCount)
{
    std::array<int, kLodBands.size()> taken{};
    std::vector<int> lods;
    lods.reserve(distances.size());
    const int last = static_cast<int>(kLodBands.size()) - 1;
    for (const float distance : distances) {
        int band = last;
        for (int i = 0; i < last; ++i) {
            if (distance <= kLodBands[static_cast<std::size_t>(i)].distance) {
                band = i;
                break;
            }
        }
        while (band < last && kLodBands[static_cast<std::size_t>(band)].cap >= 0
            && taken[static_cast<std::size_t>(band)] >= kLodBands[static_cast<std::size_t>(band)].cap) {
            ++band;
        }
        ++taken[static_cast<std::size_t>(band)];
        lods.push_back(std::clamp(band, 0, std::max(lodCount - 1, 0)));
    }
    return lods;
}

float shaderArcCos(float c)
{
    const float a = std::min(std::abs(c), 1.0F);
    const float polynomial = ((kArcCos3 * a + kArcCos2) * a + kArcCos1) * a + kArcCos0;
    const float angle = std::sqrt(1.0F - a) * polynomial;
    return c < 0.0F ? std::numbers::pi_v<float> - angle : angle;
}

SpriteColumn spriteColumn(const glm::vec2& facing, const glm::vec2& toCamera, bool sixViews)
{
    const float c = facing.x * toCamera.x + facing.y * toCamera.y;
    const float s = toCamera.y * facing.x - toCamera.x * facing.y;
    SpriteColumn result;
    if (sixViews) {
        const float angle = std::min(shaderArcCos(c), std::numbers::pi_v<float> / 2.0F);
        result.column = static_cast<int>(std::floor(angle * kSixViewSteps / std::numbers::pi_v<float> + kSixViewBias));
    } else {
        result.column = c > kFrontCosine ? 1 : 2;
    }
    if (c < -kFrontCosine) {
        result.column = 0;
    }
    result.mirrored = s > 0.0F && result.column > 1;
    return result;
}

std::uint32_t CrowdRandom::next()
{
    m_state = m_state * kRandomMultiplier + kRandomIncrement;
    return (m_state >> 16) & kRandomMask;
}

float CrowdRandom::range(float lo, float hi)
{
    return lo + (hi - lo) * (static_cast<float>(next()) / kRandomMax);
}

std::uint32_t CrtRandom::next()
{
    m_state = m_state * kCrtMultiplier + kCrtIncrement;
    return (m_state >> 16) & kRandomMask;
}

std::vector<std::uint32_t> clipTable(std::span<const std::uint32_t> clips, std::span<const float> weights)
{
    if (clips.empty()) {
        return {};
    }
    float total = 0.0F;
    for (std::size_t i = 0; i < clips.size() && i < weights.size(); ++i) {
        total += weights[i];
    }
    const float scale = total != 0.0F ? 1.0F / total : 0.0F;
    std::vector<std::uint32_t> table;
    table.reserve(kClipSlots);
    float count = 0.0F;
    for (std::size_t i = 0; i < clips.size() && i < weights.size(); ++i) {
        count += weights[i] * scale * static_cast<float>(kClipSlots);
        while (count > 0.0F && table.size() < kClipSlots) {
            table.push_back(clips[i]);
            count -= 1.0F;
        }
    }
    table.resize(kClipSlots, clips.front());
    return table;
}

} // namespace openhorizon::spectators::draw
