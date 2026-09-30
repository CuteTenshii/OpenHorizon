#include "CameraSettings.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <cstdlib>
#include <format>
#include <sstream>

namespace openhorizon {

namespace {

constexpr std::string_view kPath = "media/camera/camerasettings.ini";

std::optional<Row> overridesOf(const Database& db, int carId)
{
    return db.queryOne("SELECT * FROM CameraOverrides WHERE CarId = ?", {std::to_string(carId)});
}

} // namespace

CameraSettings CameraSettings::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kPath);
    if (!text) {
        throw DataError("{} not found", kPath);
    }
    CameraSettings settings;
    std::istringstream in(*text);
    std::string line;
    while (std::getline(in, line)) {
        const std::size_t space = line.find_first_of(" \t");
        if (space == std::string::npos) {
            continue;
        }
        const std::string key = toLower(line.substr(0, space));
        const std::string value = line.substr(line.find_first_not_of(" \t=", space));
        char* end = nullptr;
        const float number = std::strtof(value.c_str(), &end);
        if (end != value.c_str()) {
            settings.m_values[key] = number;
        }
    }
    return settings;
}

float CameraSettings::get(std::string_view key, float fallback) const
{
    const auto it = m_values.find(toLower(key));
    return it == m_values.end() ? fallback : it->second;
}

FollowCamera FollowCamera::load(const CameraSettings& settings, std::string_view section, const Database& db, int carId,
    std::string_view prefix, float baseHeight, float baseRadius)
{
    const auto key = [&](std::string_view name) { return std::format("{}\\{}", section, name); };
    FollowCamera camera;
    camera.baseHeight = baseHeight;
    camera.baseRadius = baseRadius;
    camera.heightMinAboveGround = settings.get(key("HeightMinAboveGround"), camera.heightMinAboveGround);
    camera.heightMaxAboveGround = settings.get(key("HeightMaxAboveGround"), camera.heightMaxAboveGround);
    camera.nearClip = settings.get(key("CamNearClip"), camera.nearClip);
    camera.fovDegrees = settings.get(key("FOV"), camera.fovDegrees);
    camera.yawSpringK = settings.get(key("Spring\\YRotSpringK"), camera.yawSpringK);
    camera.yawSpringD = settings.get(key("Spring\\YRotSpringD"), camera.yawSpringD);
    camera.yawCarDampScale = settings.get(key("Spring\\YRotCarDampScale"), camera.yawCarDampScale);
    camera.yawMaxSpeed = settings.get(key("Spring\\YRotMaxSpeed"), camera.yawMaxSpeed);
    camera.heightSpringK = settings.get(key("Spring\\HeightSpringK"), camera.heightSpringK);
    camera.heightSpringKInAir = settings.get(key("Spring\\HeightSpringKInAir"), camera.heightSpringKInAir);
    camera.heightSpringD = settings.get(key("Spring\\HeightSpringD"), camera.heightSpringD);

    // The override pass (0x828FE5E8) stores each non-zero column and, if it
    // stored any, Init skips the pass that copies the ini's globals
    // (0x828FE2D0) and the height limits; the untouched offsets stay 0.
    if (const std::optional<Row> row = overridesOf(db, carId)) {
        const float height = row->f(std::format("{}HeightOffset", prefix));
        const float radius = row->f(std::format("{}RadiusOffset", prefix));
        const float target = row->f(std::format("{}TargetYOffset", prefix));
        if (height != 0.0F || radius != 0.0F || target != 0.0F) {
            camera.overridden = true;
            camera.heightOffset = height;
            camera.radiusOffset = radius;
            camera.targetYOffset = target;
            return camera;
        }
    }
    camera.heightOffset = settings.get(key("HeightOffsetGlobal"), 0.0F);
    camera.radiusOffset = settings.get(key("RadiusOffsetGlobal"), 0.0F);
    camera.targetYOffset = settings.get(key("TargetYOffsetGlobal"), 0.0F);
    return camera;
}

CarCameras CarCameras::load(const CameraSettings& settings, const Database& db, int carId)
{
    CarCameras cameras;
    cameras.near = FollowCamera::load(settings, "FollowLowCam", db, carId, "CamFollowLow", 0.35F, 2.95F);
    cameras.far = FollowCamera::load(settings, "FollowHighCam", db, carId, "CamFollowHigh", 0.6F, 3.1F);

    cameras.hoodFovDegrees = settings.get("HoodCam\\FOV", cameras.hoodFovDegrees);
    cameras.hoodNearClip = settings.get("HoodCam\\CamNearClip", cameras.hoodNearClip);
    cameras.hoodHeight = settings.get("HoodCam\\CamYOffset", 0.0F);
    cameras.hoodZ = settings.get("HoodCam\\CamZOffset", 0.0F);
    cameras.hoodPitch = settings.get("HoodCam\\CamPitchOffset", 0.0F);

    cameras.bumperFovDegrees = settings.get("BumperHighCam\\FOV", cameras.bumperFovDegrees);
    cameras.bumperNearClip = settings.get("BumperHighCam\\CamNearClip", cameras.bumperNearClip);
    cameras.bumperRestHeight = settings.get("BumperHighCam\\RestHeightAboveGround", cameras.bumperRestHeight);
    cameras.bumperZ = settings.get("BumperHighCam\\CamZOffset", 0.0F);

    if (const std::optional<Row> row = overridesOf(db, carId)) {
        const float hoodHeight = row->f("CamHoodHeightOffset");
        const float hoodZ = row->f("CamHoodZOffset");
        const float hoodPitch = row->f("CamHoodPitchOffset");
        if (hoodHeight != 0.0F || hoodZ != 0.0F || hoodPitch != 0.0F) {
            cameras.hoodHeight = hoodHeight;
            cameras.hoodZ = hoodZ;
            cameras.hoodPitch = hoodPitch;
        }
        if (const float bumperZ = row->f("CamBumperHighZOffset"); bumperZ != 0.0F) {
            cameras.bumperZ = bumperZ;
        }
    }
    return cameras;
}

} // namespace openhorizon
