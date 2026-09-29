#pragma once

#include <cstdint>

namespace fh::particles {

/// The random number generator the game's particle code draws from.
///
/// Verified from the executable: every draw steps a 32-bit linear
/// congruential generator, seed = seed * 0x41C64E6D + 12345 (0x82DBE490),
/// and uses bits 16-30 of the new seed. uniform() scales those 15 bits by
/// 1/32767 (0x82DBE4B0, constant 3.0518509e-5 at 0x82000E88), so it covers
/// [0, 1] with both ends included; range() is lo + (hi - lo) * uniform()
/// (0x82DBE4F0). Each particle system owns one generator (at +540 of the
/// system); the seed the game starts it with is not decoded, so the
/// constructor takes one.
class GameRandom {
public:
    explicit GameRandom(std::uint32_t seed = 0)
        : m_seed(seed)
    {
    }

    /// Fifteen random bits, 0 to 32767.
    std::uint32_t next15()
    {
        m_seed = m_seed * 0x41C64E6DU + 12345U;
        return (m_seed >> 16) & 0x7FFFU;
    }

    float uniform() { return static_cast<float>(next15()) * (1.0F / 32767.0F); }

    float range(float lo, float hi)
    {
        const float r = uniform();
        return r * (hi - lo) + lo;
    }

    std::uint32_t seed() const { return m_seed; }

private:
    std::uint32_t m_seed;
};

} // namespace fh::particles
