#pragma once

#include "car/CarPhysics.h"
#include "game/RaceData.h"

#include <vector>

namespace fh {

/// Drives a car along a racing line: steers at a point ahead on the line
/// (pure pursuit) and holds a speed profile computed from the line's
/// curvature, the car's grip and the driver's skill.
class AiDriver {
public:
    /// `skill` is the event's 0-100 AI skill; `laneOffset` shifts the
    /// driver sideways from the line in metres, so a field does not drive
    /// nose to tail on one line.
    AiDriver(const RacingLine& line, const CarSpec& spec, int skill, float laneOffset);

    /// Controls for this frame. `speedScale` lets the race hold drivers back
    /// during the countdown (0) or ease them (below 1).
    CarControls update(const CarPhysics& car, float dt, float speedScale = 1.0F);
    /// Caps the speed (m/s) the driver aims for, for traffic that keeps to
    /// road speeds; unlimited by default.
    void setMaxSpeed(float speed) { m_maxSpeed = speed; }

    /// Where the driver is on the line, as a fractional point index that
    /// keeps counting up past the end of a loop.
    float lineIndex() const { return m_index; }
    void setLineIndex(float index) { m_index = index; }
    /// True when the driver has been stuck long enough that the race should
    /// put it back on the line.
    bool needsRecovery() const { return m_stuckTime > 8.0F; }
    void recovered() { m_stuckTime = 0.0F; m_reverseTime = 0.0F; }
    float targetSpeed() const { return m_targetSpeed; }
    /// The speed (m/s) this driver would take at a point of the line; the
    /// race's driving line uses it to show where to brake.
    float speedLimitAt(float index) const;

private:
    const RacingLine& m_line;
    const CarSpec& m_spec;
    std::vector<float> m_speedLimit;
    float m_index = -1.0F;
    float m_laneOffset = 0.0F;
    float m_skill = 0.7F;
    float m_targetSpeed = 0.0F;
    float m_maxSpeed = 1000.0F;
    float m_stuckTime = 0.0F;
    float m_reverseTime = 0.0F;
};

} // namespace fh
