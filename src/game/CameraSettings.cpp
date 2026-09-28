#include "CameraSettings.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <cstdlib>
#include <format>
#include <sstream>

namespace fh {

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

FollowCamera FollowCamera::load(
    const CameraSettings& settings, std::string_view section, const Database& db, int carId, std::string_view prefix)
{
    const auto key = [&](std::string_view name) { return std::format("{}\\{}", section, name); };
    FollowCamera camera;
    // Minimum and maximum height above the ground are equal in the file for
    // both chase cameras: a fixed height.
    camera.height = settings.get(key("HeightMinAboveGround"), camera.height);
    camera.fovDegrees = settings.get(key("FOV"), camera.fovDegrees);
    camera.yawSpringK = settings.get(key("Spring\\YRotSpringK"), camera.yawSpringK);
    camera.yawSpringD = settings.get(key("Spring\\YRotSpringD"), camera.yawSpringD);
    camera.yawMaxSpeed = settings.get(key("Spring\\YRotMaxSpeed"), camera.yawMaxSpeed);
    camera.heightSpringK = settings.get(key("Spring\\HeightSpringK"), camera.heightSpringK);
    camera.heightSpringD = settings.get(key("Spring\\HeightSpringD"), camera.heightSpringD);
    if (const std::optional<Row> row = overridesOf(db, carId)) {
        camera.heightOffset = row->f(std::format("{}HeightOffset", prefix));
        camera.radiusOffset = row->f(std::format("{}RadiusOffset", prefix));
        camera.targetYOffset = row->f(std::format("{}TargetYOffset", prefix));
    }
    return camera;
}

CarCameras CarCameras::load(const CameraSettings& settings, const Database& db, int carId)
{
    CarCameras cameras;
    cameras.near = FollowCamera::load(settings, "FollowLowCam", db, carId, "CamFollowLow");
    cameras.far = FollowCamera::load(settings, "FollowHighCam", db, carId, "CamFollowHigh");
    cameras.hoodFovDegrees = settings.get("HoodCam\\FOV", cameras.hoodFovDegrees);
    cameras.bumperFovDegrees = settings.get("BumperHighCam\\FOV", cameras.bumperFovDegrees);
    cameras.bumperHeight = settings.get("BumperHighCam\\RestHeightAboveGround", cameras.bumperHeight);
    cameras.bumperZOffset = settings.get("BumperHighCam\\CamZOffset", cameras.bumperZOffset);
    if (const std::optional<Row> row = overridesOf(db, carId)) {
        cameras.hoodHeight = row->f("CamHoodHeightOffset");
        cameras.hoodZOffset = row->f("CamHoodZOffset");
        cameras.hoodPitchDegrees = row->f("CamHoodPitchOffset");
        cameras.bumperHeight += row->f("CamBumperHighHeightOffset");
        cameras.bumperZOffset += row->f("CamBumperHighZOffset");
    }
    return cameras;
}

} // namespace fh
