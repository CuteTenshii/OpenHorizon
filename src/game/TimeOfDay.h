#pragma once

#include "render/CloudDefs.h"

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {

class Vfs;

namespace render {
struct SceneLighting;
}

/// `Ribbon_00/TimeOfDayA.xml`: named curves keyed at minutes of the day, wrapping at midnight. Colours are linear
/// RGB with no gamma or scale (0x82660E80); `...Pos` curves aim at their `...TargetPos` curve like a targeted light.
/// Also the default template of fog_templates.xml, whose multipliers apply to the fog curves (0x8265AB2C).
class TimeOfDayCurves {
public:
    /// Throws DataError if the file is missing or not a `TOD` document.
    static TimeOfDayCurves load(const Vfs& vfs, std::string_view track);

    bool has(std::string_view curve) const;
    /// Zero for a curve the file lacks.
    glm::vec3 sample(std::string_view curve, float minutes) const;
    float sampleScalar(std::string_view curve, float minutes) const { return sample(curve, minutes).x; }

    /// Multipliers for the fog curves, and the sun scatter distances in metres.
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
    /// Nothing when the track has no template file.
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
    /// 16:00 (0x8265F748).
    static constexpr float kStartSeconds = 16.0F * 3600.0F;

    explicit TimeOfDay(TimeOfDayCurves curves);

    /// The clock runs at the `TimeSpeed` curve times real time (0x8265A48C): 28x by day, 24x at dusk, 220x at night.
    void advance(float realSeconds);
    void setSeconds(float seconds);
    float seconds() const { return m_seconds; }
    float minutes() const { return m_seconds / 60.0F; }

    /// Direction light travels from the sun (pointing down by day).
    glm::vec3 sunDirection() const;
    render::SceneLighting lighting() const;
    /// See render::sampleCloudParameters().
    render::CloudParameters clouds() const;
    /// 0 by day, 1 when the street lights are on.
    float lights() const;
    float stars() const;
    const TimeOfDayCurves& curves() const { return m_curves; }
    /// PostProcessing::fog(), replacing the file's default template in lighting().
    void setFog(const TimeOfDayCurves::FogTemplate& fog) { m_fog = fog; }

private:
    glm::vec3 aim(std::string_view position, std::string_view target) const;

    TimeOfDayCurves m_curves;
    float m_seconds = kStartSeconds;
    std::optional<TimeOfDayCurves::FogTemplate> m_fog;
};

} // namespace openhorizon
