#pragma once

#include <cmath>
#include <cstdint>

namespace openhorizon::radio {

/// The game's LCG for every radio pick: state * 0x41C64E6D + 12345, value bits 16..30 (verified at 0x82dbe490).
/// The game's seed (0x82c9d564 calls 0x82421870) was not traced, so callers choose one.
class RadioRandom {
public:
    static constexpr int kMax = 32767;

    explicit RadioRandom(std::uint32_t seed = 0)
        : m_state(seed)
    {
    }

    std::uint32_t state() const { return m_state; }

    int next()
    {
        m_state = m_state * 0x41C64E6DU + 12345U;
        return static_cast<int>((m_state >> 16) & 0x7FFFU);
    }

    /// In [0, 1] (0x82dbe4b0).
    float unit() { return static_cast<float>(next()) * kInverseMax; }

    /// Single precision with a fused multiply-add, as the game (0x82dbe4f0).
    float range(float low, float high)
    {
        const float span = high - low;
        return std::fma(static_cast<float>(next()) * kInverseMax, span, low);
    }

    /// In [low, high] (0x82dbe538); the two ends are half as likely as the values between.
    int integer(int low, int high)
    {
        const auto span = static_cast<float>(high - low);
        const float value = std::fma(static_cast<float>(next()) * kInverseMax, span, static_cast<float>(low));
        // The game's rounding; lround would change which integers come out.
        return static_cast<int>(value + 0.5F); // NOLINT(bugprone-incorrect-roundings)
    }

private:
    /// The constant at 0x82000e88.
    static constexpr float kInverseMax = 3.0518509447574615e-05F;

    std::uint32_t m_state;
};

} // namespace openhorizon::radio
