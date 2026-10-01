#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace openhorizon {

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

/// The values of one chase camera (CCamFollowLow or CCamFollowHigh).
/// Verified: the fallbacks below are the ini table's defaults (0x828FDD30,
/// Low 0x83336DCC, High 0x83336E08), read by CCamFollow::Init (0x8290D358)
/// and Update (0x8290B220).
struct FollowCamera {
    /// Metres above the top and behind the rear of the car's box. Class
    /// constants, not ini values (0x828FE5E8 Low, 0x828FE6C8 High).
    float baseHeight = 0.35F;
    float baseRadius = 2.95F;
    /// Metres added to height, distance and look-at height: the ini's
    /// *OffsetGlobal, or all three CameraOverrides columns when any is non-zero.
    float heightOffset = 0.0F;
    float radiusOffset = 0.0F;
    float targetYOffset = 0.0F;
    /// True when the offsets come from CameraOverrides; the height limits
    /// below then do not apply.
    bool overridden = false;
    /// Init moves the height and target offsets together to keep the rest
    /// height within these.
    float heightMinAboveGround = 1.48F;
    float heightMaxAboveGround = 2.0F;
    float nearClip = 0.3F;
    /// Vertical field of view in degrees.
    float fovDegrees = 53.0F;
    /// Heading spring; yawMaxSpeed is in rad/s.
    float yawSpringK = 4.5F;
    float yawSpringD = 4.4F;
    float yawCarDampScale = 0.76F;
    float yawMaxSpeed = 2.0F;
    /// Height spring; the in-air stiffness applies with no wheel on the
    /// ground, and the ini leaves it at the table's 10.
    float heightSpringK = 30.0F;
    float heightSpringKInAir = 10.0F;
    float heightSpringD = 11.85F;

    /// Reads `section` ("FollowLowCam") and the car's overrides, whose
    /// columns carry `prefix` ("CamFollowLow").
    static FollowCamera load(const CameraSettings& settings, std::string_view section, const Database& db, int carId,
        std::string_view prefix, float baseHeight, float baseRadius);
};

/// The chase, hood and bumper cameras of a car.
struct CarCameras {
    FollowCamera near;
    FollowCamera far;
    /// Hood camera (CCamHood): eye offset and pitch in radians (negative
    /// looks down); CameraOverrides' hood columns win when any is non-zero
    /// (0x828FC9D0).
    float hoodHeight = 0.0F;
    float hoodZ = 0.0F;
    float hoodPitch = 0.0F;
    float hoodFovDegrees = 53.0F;
    float hoodNearClip = 0.3F;
    /// Bumper camera (CCamBumperHigh): 0.6096 m behind the front of the box
    /// plus bumperZ; a non-zero CamBumperHighZOffset wins (0x828FC7D8).
    float bumperRestHeight = 0.98F;
    float bumperZ = 0.0F;
    float bumperFovDegrees = 53.0F;
    float bumperNearClip = 0.3F;

    static CarCameras load(const CameraSettings& settings, const Database& db, int carId);
};

} // namespace openhorizon
