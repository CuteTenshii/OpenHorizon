#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace openhorizon {

class Database;
class Vfs;

/// A curve sampled at evenly spaced inputs from `minInput` to `maxInput`,
/// read with linear interpolation and clamped at both ends.
struct SampledCurve {
    float minInput = 0.0F;
    float maxInput = 1.0F;
    float scale = 1.0F;
    std::vector<float> values;

    float operator()(float input) const;
    bool empty() const { return values.empty(); }
};

/// Friction of one tyre direction: normalised friction against slip at a
/// light and a heavy load, blended for loads between. From
/// `List_TireFrictionMultiCurve` / `List_TireFrictionCurve`: slip in degrees
/// (lateral, maxSlip 49.5) or slip ratio (longitudinal, 1.1); loads in kgf.
struct TireFrictionCurve {
    float maxSlip = 1.0F;
    float lightLoadKgf = 10.1972F;
    float heavyLoadKgf = 1019.72F;
    float loadClampKgf = 5000.0F;
    SampledCurve light;
    SampledCurve heavy;

    /// Normalised friction (0 to about 1) times the curve's friction
    /// coefficient, at `slip` (same unit as maxSlip, sign ignored) under
    /// `loadN` newtons.
    float friction(float slip, float loadN) const;
    /// The slip at which the blended curve peaks for `loadN`.
    float peakSlip(float loadN) const;
    /// Peak friction for `loadN`: the curves' peaks blended by load and
    /// clamped, as the lookup at 0x82DD1678 does.
    float peakFriction(float loadN) const;
};

struct TireCompound {
    int id = 0;
    std::string name;
    float lateralFrictionScale = 1.0F;
    float brakingFrictionScale = 1.0F;
    /// Accelerating grip scale falls from the first value to the second as
    /// speed rises between the two speeds (m/s).
    float accelFrictionScale0 = 1.0F;
    float accelFrictionScale1 = 1.0F;
    float accelScaleSpeed0 = 5.0F;
    float accelScaleSpeed1 = 15.0F;
    float wetFrictionScale = 0.8F;
    float wetSlipScale = 1.5F;
    /// Kilogram square metres, per tyre and wheel.
    float momentOfInertia = 1.6F;
    /// Grip scale for tyre width: `widthScale0` at `width0` mm up to
    /// `widthScale1` at `width1` mm.
    float width0 = 150.0F;
    float width1 = 650.0F;
    float widthScale0 = 1.0F;
    float widthScale1 = 1.0F;
    float overheatedTempF = 240.0F;
    std::array<float, 4> hudTempsF{};
    TireFrictionCurve lateral;
    TireFrictionCurve longitudinal;
    /// Grip against tyre temperature (degrees Fahrenheit), and against speed.
    SampledCurve tempAffectFriction;
    SampledCurve speedAffectFriction;
    SampledCurve camberAffectLateral;

    float widthScale(float widthMm) const;
};

/// One axle's suspension, from `List_SpringDamperPhysics`, converted to SI:
/// the table stores springs in N/mm and dampers in kN·s/m.
struct AxleSuspension {
    float rideHeight = 0.12F;
    float maxCompression = 0.06F;
    float maxExtension = 0.1F;
    float springRate = 50000.0F;
    float bumpDamping = 3000.0F;
    float reboundDamping = 5000.0F;
    float bumpStopStiffness = 200000.0F;
    float bumpStopDamping = 2000.0F;
    float staticCamberDeg = 0.0F;
    float staticToeDeg = 0.0F;
    float casterDeg = 5.0F;
    float camberGainCompression = 0.0F;
    float camberGainExpansion = 0.0F;
    /// Anti-roll bar stiffness in N·m per radian of roll, and its damping.
    float antiRollStiffness = 0.0F;
    float antiRollDamping = 0.0F;
};

struct AxleTires {
    float widthMm = 225.0F;
    float aspect = 45.0F;
    float rimDiameterIn = 17.0F;

    /// Rolling radius in metres from the ISO size.
    float radius() const { return rimDiameterIn * 0.0254F * 0.5F + widthMm * 0.001F * aspect * 0.01F; }
};

enum class DriveType : std::uint8_t { Fwd = 1, Rwd = 2, Awd = 3 };

struct Differential {
    /// Locking torque (N·m) under power and when coasting, the input torque
    /// at which the accelerating lock reaches its full value, and the wheel
    /// speed difference (rad/s) at which locking stops growing.
    float lockAccel = 0.0F;
    float lockDecel = 0.0F;
    float relativeSpeedClamp = 50.0F;
    float accelReferenceTorque = 50.0F;
};

/// One `FrictionTorqueMod` block of `media/physics/physicssettings.ini`:
/// scale and clamp of the tyre forces' yaw torque (applied at 0x82DEFF88).
/// `0` values hold within the heading angle of cosine `cosYawAngle0`, `1`
/// values past `cosYawAngle1`, blended between; "Turn" adds to the yaw,
/// "Straighten" opposes it. Clamps in deg/s²; field order is the loader's
/// (0x82CA7218).
struct FrictionTorqueMod {
    float cosYawAngle0 = 0.0F;
    float cosYawAngle1 = 0.0F;
    float turnScale0 = 0.0F;
    float straightenScale0 = 0.0F;
    float turnClamp0 = 0.0F;
    float straightenClamp0 = 0.0F;
    float turnScale1 = 0.0F;
    float straightenScale1 = 0.0F;
    float turnClamp1 = 0.0F;
    float straightenClamp1 = 0.0F;
};

/// Driving-assist tunables of `media/physics/physicssettings.ini` (loader
/// 0x82CA0CE0, read from 0x8359FAD8); defaults are the disc file's values.
struct AssistSettings {
    /// Stability management and traction control act above these speeds
    /// (mph), compared as squared speeds (0x82DEEF80, 0x82DF8630).
    float stabilityManageSpeedMph = 30.0F;
    float tractionControlSpeedMph = 30.0F;
    /// Normalised slip past the allowed slip over which traction control
    /// goes from no cut to a fully closed throttle.
    float tcsFullEffectFricDiff = 0.25F;
    /// Scale on the allowed slip when the driven tyres are at their peak slip
    /// angle, per drive layout.
    float tcsFullSteerSlipScaleRwd = 1.1F;
    float tcsFullSteerSlipScaleFwd = 1.2F;
    float tcsFullSteerSlipScaleAwd = 2.0F;
    /// Without ABS, a brake pedal at or past `noAbsBrakeTravel` still lets
    /// a wheel go at these normalised slips, blended by steering input,
    /// holding it released for `noAbsDisableTime` seconds.
    float noAbsBrakeTravel = 0.975F;
    float noAbsDisableTime = 0.0F;
    float noAbsSlipFrontNoSteer = 1.0F;
    float noAbsSlipFrontFullSteer = 0.71F;
    float noAbsSlipRearNoSteer = 1.0F;
    float noAbsSlipRearFullSteer = 0.71F;
    /// CarTuning range of the traction-control slip settings: the car's
    /// value is held within it (0x82DF6600, 0x82DF5080).
    float tuneTcsSlipDefMin = 0.1F;
    float tuneTcsSlipDefMax = 30.0F;
    /// FrictionTorqueMod: the `0` and `1` blocks apply at these masses and
    /// blend between them.
    float frictionTorqueMass0Kg = 909.1F;
    float frictionTorqueMass1Kg = 1590.9F;
    std::array<FrictionTorqueMod, 2> frictionTorqueWithAssists{
        FrictionTorqueMod{0.996194F, 0.961261F, 1.0F, 1.0F, 180.0F, 145.0F, 0.7F, 0.8F, 120.0F, 140.0F},
        FrictionTorqueMod{0.996194F, 0.961261F, 1.0F, 1.0F, 150.0F, 115.0F, 0.7F, 0.8F, 100.0F, 120.0F}};
    std::array<FrictionTorqueMod, 2> frictionTorqueNoAssists = frictionTorqueWithAssists;
};

struct CarSpec {
    int id = 0;
    std::string mediaName;
    int year = 0;
    /// Display strings are string table references (`_&<id>`); see
    /// StringTable.
    std::string displayNameRef;
    std::string makeNameRef;
    int classId = 0;
    float performanceIndex = 0.0F;

    /// List_UpgradeCarBodyWeight.Mass of the stock body (loader 0x82CA9A10);
    /// Data_Car.CurbWeight (in 100 kg) is the fallback.
    float massKg = 1400.0F;
    /// Share of the weight on the front axle.
    float frontWeight = 0.55F;
    float wheelbase = 2.6F;
    float frontTrack = 1.5F;
    float rearTrack = 1.5F;
    /// Bounds of the body in car space (x right, y up, z forward), metres.
    std::array<float, 3> boundsMin{-0.9F, 0.0F, -2.2F};
    std::array<float, 3> boundsMax{0.9F, 1.3F, 2.2F};
    /// Height of the centre of mass above the ground at ride height
    /// (List_UpgradeCarBodyWeight.CMHeight).
    float cgHeight = 0.5F;
    /// Size of the box the game gives the body for its moments of inertia
    /// (List_UpgradeCarBodyWeight.BlockDimX/Y/Z: width, height, length).
    std::array<float, 3> inertiaBlock{1.9F, 0.9F, 4.5F};

    /// A body sphere from `physics/maxdata.xml`, in car space (the file's
    /// coordinates less BottomCenterWheelbasePos). Flags 2 marks the body
    /// spheres; flags 1 the 5 cm corner points.
    struct CollisionSphere {
        std::array<float, 3> centre{};
        float radius = 0.0F;
        int flags = 0;
    };
    std::vector<CollisionSphere> collisionSpheres;

    // Engine
    std::string engineName;
    /// Full-throttle torque in N·m against engine speed, sampled from 0 rpm
    /// to `torqueCurveMaxRpm`.
    SampledCurve torqueCurve;
    float torqueCurveMaxRpm = 8000.0F;
    /// Braking torque with the throttle shut at the redline, N·m. Scaled
    /// linearly with engine speed: the game's spread is not known.
    float engineBrakingTorque = 50.0F;
    float redlineRpm = 7000.0F;
    float idleRpm = 800.0F;
    float launchRpm = 4000.0F;
    float engineInertia = 0.15F;
    /// The game's overall power trim for the car.
    float gameTorqueScale = 1.0F;
    bool diesel = false;
    bool rotary = false;

    // Drivetrain
    DriveType drive = DriveType::Rwd;
    /// Forward gear ratios; reverse separately (a negative ratio).
    std::vector<float> gears;
    float reverseRatio = -3.5F;
    float finalDrive = 3.5F;
    float shiftTime = 0.2F;
    float drivetrainInertia = 0.01F;
    float clutchMaxTorque = 800.0F;
    float clutchInTime = 0.05F;
    float clutchOutTime = 0.05F;
    Differential frontDiff;
    Differential rearDiff;
    Differential centreDiff;
    /// Share of the drive torque sent to the rear axle of an all-wheel-drive
    /// car.
    float rearTorqueSplit = 0.5F;

    // Wheels and tyres
    AxleTires frontTires;
    AxleTires rearTires;
    TireCompound compound;
    AxleSuspension front;
    AxleSuspension rear;

    // Brakes
    float brakeFrictionScale = 1.0F;
    /// The brake torque and bias tuning sliders, 0 to 1 (0.5 is stock).
    float brakeTorqueSlider = 0.5F;
    float brakeBias = 0.5F;
    float handbrakeScale = 1.5F;
    float absReleasePoint = 0.7F;
    /// ABS (List_UpgradeBrakes, loader 0x82CAA740): normalised slip at which
    /// a wheel's brake lets go, straight and at full lock (blended by steering
    /// at 0x82DF8630), and how long it stays released (seconds).
    float absDuration = 0.05F;
    float absSlipFrontNoSteer = 3.0F;
    float absSlipFrontFullSteer = 0.5F;
    float absSlipRearNoSteer = 2.5F;
    float absSlipRearFullSteer = 2.0F;
    int frontBrakeSize = 12;
    int rearBrakeSize = 12;

    // Aerodynamics: pounds-force at 100 mph, as the game tabulates them.
    float dragAt100MphLbf = 200.0F;
    float frontDownforceAt100MphLbf = 0.0F;
    float rearDownforceAt100MphLbf = 0.0F;
    float lateralDragFront = 250.0F;
    float lateralDragRear = 250.0F;
    float verticalDrag = 500.0F;
    float gameDragScale = 1.0F;

    // Steering
    float steerMaxAngleDeg = 35.0F;
    float steerMaxAngleFilteredDeg = 30.0F;
    /// Degrees per second the wheel turns in and returns.
    float steerRateTurning = 100.0F;
    float steerRateStraighten = 100.0F;
    float steerSpeedSensitiveMaxGees = 1.0F;
    float steerSpeedSensitiveMinAngleDeg = 5.0F;
    float steerSpeedSensitiveSlowMph = 1.0F;
    float steerSpeedSensitiveFastMph = 120.0F;
    /// Scale on the turning rate at and above the fast speed.
    float steerSpeedSensitiveFastRateScale = 1.0F;
    /// Degrees per second when countersteering and when searching for the
    /// front tyres' peak at full lock.
    float steerRateCountersteer = 105.0F;
    float steerRateDynFindPeak = 10.0F;
    /// Seconds for the steering to reach its full rate from rest.
    float steerAccelTimeToMaxRate = 0.01F;
    /// physicssettings.ini: the normalised rear slip above which the steering
    /// countersteers, and the front slip above which it stops adding lock.
    float normRearSlipAngleToCountersteer = 3.0F;
    float steerFrictionClamp = 1.2F;
    /// Data_Car.AssistsTCSSlipDefTakeoff / Moving: the driven wheels'
    /// normalised slip traction control allows below and above
    /// TractionControlSpeed.
    float tcsSlipTakeoff = 33.0F;
    float tcsSlipMoving = 0.5F;
    /// AutoSteerOverrides.STMSlipMin / STMSlipMax (by AutoSteerOverrideId):
    /// rear normalised slip angle where stability management starts and fully
    /// brakes.
    float stmSlipMin = 1.5F;
    float stmSlipMax = 3.0F;
    AssistSettings assistSettings;

    /// The game's own simulation results for the stock car, kept to check
    /// this engine's car model against.
    struct Reference {
        float timeTo60Mph = 0.0F;
        float timeTo100Mph = 0.0F;
        float quarterMileTime = 0.0F;
        float quarterMileSpeed = 0.0F;
        float topSpeed = 0.0F;
        float brakeDistance60Mph = 0.0F;
        float brakeDistance100Mph = 0.0F;
        float lateralG60Mph = 0.0F;
        float lateralG120Mph = 0.0F;
        float peakPowerW = 0.0F;
        float peakTorqueNm = 0.0F;
    } reference;

    float totalDriveRatio(int gear) const { return gears.at(static_cast<std::size_t>(gear)) * finalDrive; }
    /// Engine torque at full throttle, N·m, including the game's trim.
    float fullThrottleTorque(float rpm) const;
};

/// Collision settings from `media/physics/physicssettings.ini` (loader
/// 0x82CA0CE0, defaults from the constructor at 0x82E08010). The disc's file
/// misspells "CarCarResitution", so the game keeps its default of -0.2.
struct CollisionSettings {
    float carCarRestitution = -0.2F;
    float defaultRestitution = 0.0F;
    float playerTorqueScale = 0.0F;
    float trafficTorqueScale = 0.3F;
    float raceCarTorqueScale = 0.3F;
    float raceCarTorqueScaleInRace = 0.2F;
    float raceCarTorqueScaleVsTrafficInRace = 0.2F;
    /// Car against the static world, read by the contact response at
    /// 0x82DD2750 (copied each step at 0x82E09BC0).
    float carWorldTorqueScale = 0.2F;
    float lowSpeedCarWorldTorqueScale = 1.0F;
    float collisionLowSpeedMph = 20.0F;
    float collisionHighSpeedMph = 50.0F;
    float frictionLowSpeedMph = 10.0F;
    float frictionHighSpeedMph = 30.0F;

    /// Share of a world contact's impulse that turns a car at `speed` m/s
    /// (0x82DD2750): LowSpeedCarWorldTorqueScale up to CollisionLowSpeedMph,
    /// CarWorldTorqueScale from CollisionHighSpeedMph, linear between.
    float carWorldTorqueScaleAt(float speed) const;
    /// Where a car at `speed` m/s sits between LowSpeedFrictionCoefficient
    /// (0) and FrictionCoefficient (1), by FrictionLow/HighSpeedMph
    /// (0x82E04258, passed to the pair lookup at 0x82E0B890).
    float frictionBlendAt(float speed) const;
};

/// Reads the collision settings; missing keys keep the executable's defaults.
CollisionSettings loadCollisionSettings(const Vfs& vfs);

/// Stand-in body spheres against the world, sized from the maxdata.xml box:
/// rows along each side at bumper height and two near the roof. Not the
/// game's shapes, which are untraced.
std::vector<CarSpec::CollisionSphere> worldContactSpheres(const CarSpec& spec);

/// Spheres for contacts between cars: maxdata.xml's body spheres (flags 2),
/// or, where none are listed, denser closed rows like worldContactSpheres().
std::vector<CarSpec::CollisionSphere> carContactSpheres(const CarSpec& spec);

/// Whether a car meets other cars with its maxdata.xml BoundingBox: those
/// listing no body spheres, so a truck's box reaches the ground.
bool carContactIsBox(const CarSpec& spec);

/// Reads the stock specification of car `carId` (`Data_Car.Id`) from the
/// game database, and the body dimensions from the car's own archive when
/// `vfs` is given. Throws DataError when a required row is missing.
CarSpec loadCarSpec(const Database& db, int carId, const Vfs* vfs = nullptr);

/// Car ids of every drivable car, in database order.
std::vector<int> drivableCarIds(const Database& db);

/// The id of the car whose media name is `mediaName` (case-insensitive), or
/// -1.
int carIdByMediaName(const Database& db, const std::string& mediaName);

} // namespace openhorizon
