#pragma once

#include <cmath>
#include <cstdint>

namespace fh::radio {

/// The game's random number generator, which the radio uses for every pick
/// (verified at 0x82dbe490 and the helpers after it): a 32-bit linear
/// congruential generator, state = state * 0x41C64E6D + 12345, whose value
/// is bits 16..30 of the new state (0..32767).
///
/// The game seeds the radio's generator from a value taken when the radio
/// starts (0x82c9d564 calls 0x82421870 and hands the result to the
/// generator's constructor); what that value is was not traced, so callers
/// choose a seed.
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

    /// next() / 32767, in [0, 1] (0x82dbe4b0). The game multiplies by the
    /// float nearest 1/32767, and so does this.
    float unit() { return static_cast<float>(next()) * kInverseMax; }

    /// low + unit() * (high - low), as the game computes it: in single
    /// precision with a fused multiply-add (0x82dbe4f0).
    float range(float low, float high)
    {
        const float span = high - low;
        return std::fma(static_cast<float>(next()) * kInverseMax, span, low);
    }

    /// An integer in [low, high], rounded from low + unit() * (high - low)
    /// by adding 0.5 and truncating (0x82dbe538), so the two ends are half
    /// as likely as the values between.
    int integer(int low, int high)
    {
        const auto span = static_cast<float>(high - low);
        const float value = std::fma(static_cast<float>(next()) * kInverseMax, span, static_cast<float>(low));
        // The game rounds this way, not to nearest-even; lround would change
        // which integers come out.
        return static_cast<int>(value + 0.5F); // NOLINT(bugprone-incorrect-roundings)
    }

private:
    /// The constant at 0x82000e88.
    static constexpr float kInverseMax = 3.0518509447574615e-05F;

    std::uint32_t m_state;
};

} // namespace fh::radio
