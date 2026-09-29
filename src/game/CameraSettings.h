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

/// The values of one chase camera (CCamFollowLow or CCamFollowHigh).
///
/// Verified in the executable: the ini section is registered at 0x828FDD30
/// into a 60-byte table (Low at 0x83336DCC, High at 0x83336E08) whose
/// initial contents are the fallbacks below; CCamFollow::Init (0x8290D358)
/// and CCamFollow::Update (0x8290B220) read it.
struct FollowCamera {
    /// Metres added to the top of the car's box for the rest height of the
    /// camera, and behind the rear of the box for its distance. Constants of
    /// the camera classes, not of the ini (0x828FE5E8 stores 0.35 and 2.95
    /// for Low, 0x828FE6C8 0.6 and 3.1 for High).
    float baseHeight = 0.35F;
    float baseRadius = 2.95F;
    /// Metres added to the camera's height, its distance and the height it
    /// looks at. The ini's *OffsetGlobal values, or the car's
    /// CameraOverrides columns in their place when any of the three is
    /// non-zero (the others are then 0).
    float heightOffset = 0.0F;
    float radiusOffset = 0.0F;
    float targetYOffset = 0.0F;
    /// True when the offsets come from CameraOverrides; the height limits
    /// below then do not apply.
    bool overridden = false;
    /// Limits of the camera's rest height above the ground; Init moves the
    /// height and target offsets together to bring it within them.
    float heightMinAboveGround = 1.48F;
    float heightMaxAboveGround = 2.0F;
    float nearClip = 0.3F;
    /// Vertical field of view in degrees.
    float fovDegrees = 53.0F;
    /// Heading spring: stiffness and damping, how much of the car's own
    /// yaw rate the damping lets through, and a limit (rad/s) on the
    /// camera's turning speed.
    float yawSpringK = 4.5F;
    float yawSpringD = 4.4F;
    float yawCarDampScale = 0.76F;
    float yawMaxSpeed = 2.0F;
    /// Height spring; the in-air stiffness applies with no wheel on the
    /// ground. The ini leaves HeightSpringKInAir out, so the table's 10
    /// stays.
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
    /// Hood camera (CCamHood): the eye sits at (0, hoodHeight, hoodZ) from
    /// the camera's base point and looks along the car, pitched by
    /// hoodPitch radians (negative looks down). CameraOverrides' three hood columns
    /// replace the ini's CamYOffset, CamZOffset and CamPitchOffset when any
    /// of them is non-zero (0x828FC9D0).
    float hoodHeight = 0.0F;
    float hoodZ = 0.0F;
    float hoodPitch = 0.0F;
    float hoodFovDegrees = 53.0F;
    float hoodNearClip = 0.3F;
    /// Bumper camera (CCamBumperHigh): RestHeightAboveGround above the
    /// ground under the front wheels, 0.6096 m behind the front of the box
    /// plus bumperZ, which is CameraOverrides' CamBumperHighZOffset when it
    /// is non-zero and the ini's CamZOffset otherwise (0x828FC7D8; the
    /// override's height column is not read).
    float bumperRestHeight = 0.98F;
    float bumperZ = 0.0F;
    float bumperFovDegrees = 53.0F;
    float bumperNearClip = 0.3F;

    static CarCameras load(const CameraSettings& settings, const Database& db, int carId);
};

} // namespace fh
