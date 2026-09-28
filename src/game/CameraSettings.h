#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace fh {

class Database;
class Vfs;

/// The game's camera tuning: `camera/camerasettings.ini` (`Section\Key
/// value` lines) for the cameras themselves, and gamedb's CameraOverrides
/// for per-car adjustments.
class CameraSettings {
public:
    /// Reads the ini. Throws DataError if it is missing.
    static CameraSettings load(const Vfs& vfs);

    float get(std::string_view key, float fallback) const;

private:
    std::unordered_map<std::string, float> m_values;
};

/// The values of one chase camera (FollowLowCam or FollowHighCam).
struct FollowCamera {
    /// Metres above the ground the camera holds, and the vertical field of
    /// view in degrees.
    float height = 1.8F;
    float fovDegrees = 50.0F;
    /// Springs that bring the camera round behind the car (yaw) and to its
    /// height; the yaw one is limited to `yawMaxSpeed` rad/s.
    float yawSpringK = 4.5F;
    float yawSpringD = 4.4F;
    float yawMaxSpeed = 2.0F;
    float heightSpringK = 30.0F;
    float heightSpringD = 11.85F;
    /// The per-car adjustments of CameraOverrides.
    float heightOffset = 0.0F;
    float radiusOffset = 0.0F;
    float targetYOffset = 0.0F;

    /// Reads `section` ("FollowLowCam") and the car's overrides, whose
    /// columns carry `prefix` ("CamFollowLow").
    static FollowCamera load(const CameraSettings& settings, std::string_view section, const Database& db, int carId,
        std::string_view prefix);
};

/// The hood and bumper cameras of a car.
struct CarCameras {
    FollowCamera near;
    FollowCamera far;
    float hoodFovDegrees = 66.0F;
    float hoodHeight = 0.0F;
    float hoodZOffset = 0.0F;
    float hoodPitchDegrees = 0.0F;
    float bumperFovDegrees = 53.0F;
    float bumperHeight = 1.15F;
    float bumperZOffset = 0.0F;

    static CarCameras load(const CameraSettings& settings, const Database& db, int carId);
};

} // namespace fh
