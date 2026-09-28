#pragma once

#include <glm/glm.hpp>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh {

class Vfs;

/// A track's lighting over the day, from `Ribbon_00/TimeOfDayA.xml`: named
/// curves (`SunPos`, `SunColor`, `AtmosphereTopColour`, ...) of keys at
/// minutes of the day, each with one to three values (`x`, `y`, `z`).
/// Sampling interpolates linearly and wraps at midnight.
///
/// What the curves mean was read from their values: `SunPos` is the
/// direction towards the sun scaled to about 100 (high at noon, low in the
/// evening); colours are linear RGB; `SunColorMult` and `SkyGain` scale the
/// sun and the sky; `SwitchOnLights` and `StarFade` run from 0 by day to 1
/// by night.
class TimeOfDayCurves {
public:
    /// Reads the track's file. Throws DataError if it is missing or not a
    /// `TOD` document.
    static TimeOfDayCurves load(const Vfs& vfs, std::string_view track);

    bool has(std::string_view curve) const;
    /// The curve's value at `minutes` past midnight; zero for a curve the
    /// file lacks.
    glm::vec3 sample(std::string_view curve, float minutes) const;
    float sampleScalar(std::string_view curve, float minutes) const { return sample(curve, minutes).x; }

private:
    struct Key {
        float time = 0.0F;
        glm::vec3 value{0.0F};
    };
    std::unordered_map<std::string, std::vector<Key>> m_curves;
};

/// The post-processing settings of `Ribbon_00/TimeOfDay.xml` that the frame
/// renderer uses.
struct PostEffectSettings {
    /// Filmic tone curve parameters: shoulder, linear and toe strengths, the
    /// linear angle, toe numerator and denominator, and the white point, in
    /// the form of the common "Uncharted 2" operator the names match.
    float shoulderStrength = 0.22F;
    float linearStrength = 0.3F;
    float linearAngle = 0.1F;
    float toeStrength = 0.2F;
    float toeNumerator = 0.01F;
    float toeDenominator = 0.3F;
    float white = 11.2F;
    float exposure = 1.0F;
    float vignettePower = 3.0F;
    float desaturate = 1.0F;

    static PostEffectSettings load(const Vfs& vfs, std::string_view track);
};

/// The game clock and the lighting it gives.
class TimeOfDay {
public:
    /// Free roam opens at 16:00 and the clock runs 48 game seconds per real
    /// second (found in the game's time-of-day object).
    static constexpr float kStartSeconds = 16.0F * 3600.0F;
    static constexpr float kSpeed = 48.0F;

    explicit TimeOfDay(TimeOfDayCurves curves);

    void advance(float realSeconds) { setSeconds(m_seconds + realSeconds * m_speed); }
    void setSeconds(float seconds);
    float seconds() const { return m_seconds; }
    float minutes() const { return m_seconds / 60.0F; }
    void setSpeed(float speed) { m_speed = speed; }
    float speed() const { return m_speed; }

    /// Direction light travels from the sun (pointing down by day).
    glm::vec3 sunDirection() const;
    glm::vec3 sunColour() const;
    glm::vec3 skyTopColour() const;
    glm::vec3 skyBottomColour() const;
    glm::vec3 hazeColour() const;
    float hazeIntensity() const;
    /// 0 by day, 1 when the street lights are on.
    float lights() const;
    float stars() const;

private:
    TimeOfDayCurves m_curves;
    float m_seconds = kStartSeconds;
    float m_speed = kSpeed;
};

} // namespace fh
