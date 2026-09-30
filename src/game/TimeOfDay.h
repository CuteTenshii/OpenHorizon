#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fh {

class Vfs;

namespace render {
struct SceneLighting;
}

/// A track's lighting over the day, from `Ribbon_00/TimeOfDayA.xml`: named
/// curves (`SunPos`, `SunColor`, `AtmosphereTopColour`, ...) of keys at
/// minutes of the day, each with one to three values (`x`, `y`, `z`).
/// Sampling interpolates linearly and wraps at midnight.
///
/// The executable's curve table (built at 0x82660E80) reads every colour
/// curve as three plain floats, with no gamma or scale, so colours are
/// linear RGB in the units the shaders take them. `SunPos`, `SunObjectPos`
/// and `HazeObjectPos` are points in a small editor space, each aimed at its
/// own `...TargetPos` curve, like a targeted light; the direction is
/// target minus position. `SunColorMult` and `SkyGain` scale the sun and the
/// sky; `SwitchOnLights` and `StarFade` run from 0 by day to 1 by night.
///
/// Also read with the curves: the default fog template of the track's
/// `media/dynamicpost/tracks/<track>/fog_templates.xml`, whose multipliers
/// the game applies to the fog curves (seen at 0x8265AB2C) and which alone
/// gives the distances over which the sun tints the fog.
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

    /// One `Template` of `fog_templates.xml`: multipliers for the fog
    /// curves and the sun scatter distances in metres.
    struct FogTemplate {
        glm::vec3 colour{1.0F};
        float density = 1.0F;
        float startDistance = 1.0F;
        glm::vec3 scatterColour{1.0F};
        float scatterPower = 1.0F;
        float heightFadeAmount = 1.0F;
        float heightFadeStart = 1.0F;
        float heightFadeEnd = 1.0F;
        float scatterStartDistance = 0.0F;
        float scatterRampDistance = 0.0F;
    };
    /// The template used away from any post-processing zone; nothing when
    /// the track has no template file.
    const std::optional<FogTemplate>& fogTemplate() const { return m_fogTemplate; }

private:
    struct Key {
        float time = 0.0F;
        glm::vec3 value{0.0F};
    };
    std::unordered_map<std::string, std::vector<Key>> m_curves;
    std::optional<FogTemplate> m_fogTemplate;
};

/// The game clock and the lighting it gives.
class TimeOfDay {
public:
    /// Free roam opens at 16:00 (57600 s, set by the time-of-day object's
    /// constructor at 0x8265F748).
    static constexpr float kStartSeconds = 16.0F * 3600.0F;

    explicit TimeOfDay(TimeOfDayCurves curves);

    /// Runs the clock on by `realSeconds`. The game adds the `TimeSpeed`
    /// curve times the frame time to its clock in seconds (0x8265A48C), so
    /// days pass at 28 times real time, dusk at 24, and night at 220.
    void advance(float realSeconds);
    void setSeconds(float seconds);
    float seconds() const { return m_seconds; }
    float minutes() const { return m_seconds / 60.0F; }

    /// Direction light travels from the sun (pointing down by day).
    glm::vec3 sunDirection() const;
    /// Sun, ambient, sky and fog for the renderers at the current time.
    render::SceneLighting lighting() const;
    /// 0 by day, 1 when the street lights are on.
    float lights() const;
    float stars() const;
    /// The curves, for the post-processing's multipliers.
    const TimeOfDayCurves& curves() const { return m_curves; }
    /// The fog template blend of the post-processing zones (see
    /// PostProcessing::fog()), which replaces the file's default template
    /// in lighting().
    void setFog(const TimeOfDayCurves::FogTemplate& fog) { m_fog = fog; }

private:
    /// Normalised direction from the curve `position` towards the curve
    /// `target`.
    glm::vec3 aim(std::string_view position, std::string_view target) const;

    TimeOfDayCurves m_curves;
    float m_seconds = kStartSeconds;
    std::optional<TimeOfDayCurves::FogTemplate> m_fog;
};

} // namespace fh
