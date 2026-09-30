#pragma once

#include <cstddef>

namespace openhorizon::audio {

/// A filter as FMOD's DSP units describe it; the engine tunings drive these
/// with curves (see EngineTuning).
struct FilterSpec {
    enum class Type {
        /// FMOD lowpass: `frequency` cutoff in Hz, `resonance` 1..10 where 1
        /// is no resonance.
        Lowpass,
        /// FMOD highpass, same parameters as Lowpass.
        Highpass,
        /// FMOD parametric EQ: `frequency` centre in Hz, `bandwidth` in
        /// octaves (0.2..5), `gain` linear (0.05..3).
        Peaking,
    };
    Type type = Type::Lowpass;
    float frequency = 1000.0F;
    float resonance = 1.0F;
    float bandwidth = 1.0F;
    float gain = 1.0F;

    bool operator==(const FilterSpec&) const = default;
};

/// Normalised direct-form biquad coefficients (a0 = 1).
struct BiquadCoefficients {
    float b0 = 1.0F;
    float b1 = 0.0F;
    float b2 = 0.0F;
    float a1 = 0.0F;
    float a2 = 0.0F;

    /// The coefficients for `spec` at `sampleRate`, from the RBJ audio EQ
    /// cookbook formulas. Frequencies are clamped below Nyquist. FMOD's
    /// resonance maps to Q = resonance / sqrt(2) so that 1 gives the
    /// maximally flat Butterworth response (the mapping is an assumption).
    static BiquadCoefficients design(const FilterSpec& spec, float sampleRate);

    /// Magnitude response at `frequency`, for tests and tools.
    float magnitudeAt(float frequency, float sampleRate) const;

    /// Coefficient-wise blend from `a` (t = 0) to `b` (t = 1), used to glide
    /// between designs within a block instead of jumping, which clicks.
    static BiquadCoefficients interpolate(const BiquadCoefficients& a, const BiquadCoefficients& b, float t)
    {
        const auto mix = [t](float x, float y) { return x + (y - x) * t; };
        return {mix(a.b0, b.b0), mix(a.b1, b.b1), mix(a.b2, b.b2), mix(a.a1, b.a1), mix(a.a2, b.a2)};
    }
};

/// One channel of biquad state, transposed direct form II, which stays well
/// behaved when coefficients change between blocks.
class Biquad {
public:
    void setCoefficients(const BiquadCoefficients& coefficients) { m_c = coefficients; }
    void reset()
    {
        m_z1 = 0.0F;
        m_z2 = 0.0F;
    }

    float process(float x)
    {
        const float y = m_c.b0 * x + m_z1;
        m_z1 = m_c.b1 * x - m_c.a1 * y + m_z2;
        m_z2 = m_c.b2 * x - m_c.a2 * y;
        return y;
    }

    void process(float* samples, std::size_t count, std::size_t stride)
    {
        for (std::size_t i = 0; i < count; ++i) {
            samples[i * stride] = process(samples[i * stride]);
        }
    }

private:
    BiquadCoefficients m_c;
    float m_z1 = 0.0F;
    float m_z2 = 0.0F;
};

/// Four-point cubic Hermite (Catmull-Rom) interpolation between y1 and y2 at
/// fraction t, the resampler's kernel.
inline float hermite(float y0, float y1, float y2, float y3, float t)
{
    const float c1 = 0.5F * (y2 - y0);
    const float c2 = y0 - 2.5F * y1 + 2.0F * y2 - 0.5F * y3;
    const float c3 = 0.5F * (y3 - y0) + 1.5F * (y1 - y2);
    return ((c3 * t + c2) * t + c1) * t + y1;
}

} // namespace openhorizon::audio
