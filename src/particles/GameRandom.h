#pragma once

#include <cstdint>

namespace openhorizon::particles {

/// The particle code's LCG: seed * 0x41C64E6D + 12345, bits 16-30 (0x82DBE490); uniform() divides by 32767,
/// so [0, 1] inclusive (0x82DBE4B0, 0x82000E88); range() is lo + (hi - lo) * uniform() (0x82DBE4F0). Verified.
/// One per particle system (+540); the game's starting seed is not decoded.
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

} // namespace openhorizon::particles
