#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace openhorizon::audio {

BiquadCoefficients BiquadCoefficients::design(const FilterSpec& spec, float sampleRate)
{
    const double rate = sampleRate;
    const double frequency = std::clamp<double>(spec.frequency, 10.0, rate * 0.49);
    const double w0 = 2.0 * std::numbers::pi * frequency / rate;
    const double cosW = std::cos(w0);
    const double sinW = std::sin(w0);
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a0 = 1.0;
    double a1 = 0.0;
    double a2 = 0.0;
    switch (spec.type) {
    case FilterSpec::Type::Lowpass:
    case FilterSpec::Type::Highpass: {
        const double q = std::max(0.1, static_cast<double>(spec.resonance) / std::numbers::sqrt2);
        const double alpha = sinW / (2.0 * q);
        const bool low = spec.type == FilterSpec::Type::Lowpass;
        b1 = low ? 1.0 - cosW : -(1.0 + cosW);
        b0 = low ? b1 / 2.0 : -b1 / 2.0;
        b2 = b0;
        a0 = 1.0 + alpha;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha;
        break;
    }
    case FilterSpec::Type::Peaking: {
        const double octaves = std::clamp(static_cast<double>(spec.bandwidth), 0.05, 8.0);
        const double amplitude = std::sqrt(std::clamp(static_cast<double>(spec.gain), 0.01, 16.0));
        const double alpha = sinW * std::sinh(std::numbers::ln2 / 2.0 * octaves * w0 / sinW);
        b0 = 1.0 + alpha * amplitude;
        b1 = -2.0 * cosW;
        b2 = 1.0 - alpha * amplitude;
        a0 = 1.0 + alpha / amplitude;
        a1 = -2.0 * cosW;
        a2 = 1.0 - alpha / amplitude;
        break;
    }
    }
    BiquadCoefficients c;
    c.b0 = static_cast<float>(b0 / a0);
    c.b1 = static_cast<float>(b1 / a0);
    c.b2 = static_cast<float>(b2 / a0);
    c.a1 = static_cast<float>(a1 / a0);
    c.a2 = static_cast<float>(a2 / a0);
    return c;
}

float BiquadCoefficients::magnitudeAt(float frequency, float sampleRate) const
{
    const double w = 2.0 * std::numbers::pi * frequency / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    const std::complex<double> numerator = static_cast<double>(b0) + static_cast<double>(b1) * z1
        + static_cast<double>(b2) * z2;
    const std::complex<double> denominator = 1.0 + static_cast<double>(a1) * z1 + static_cast<double>(a2) * z2;
    return static_cast<float>(std::abs(numerator / denominator));
}

} // namespace openhorizon::audio
