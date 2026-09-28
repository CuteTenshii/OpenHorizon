#include "CarSpec.h"

#include "base/Error.h"
#include "base/Log.h"
#include "data/Database.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cmath>
#include <format>

namespace fh {

namespace {

constexpr float kKgfToNewton = 9.80665F;
/// The database stores curb weight in units of 100 kg.
constexpr float kCurbWeightUnit = 100.0F;
/// Springs are stored in N/mm and dampers in kN·s/m.
constexpr float kSpringUnit = 1000.0F;
constexpr float kDamperUnit = 1000.0F;
/// Anti-roll bars are stored in N/mm of opposite wheel travel.
constexpr float kAntiRollUnit = 1000.0F;

std::string id(int value)
{
    return std::to_string(value);
}

SampledCurve readCurve(const Row& row, int count, float minInput, float maxInput, float scale)
{
    SampledCurve curve;
    curve.minInput = minInput;
    curve.maxInput = maxInput;
    curve.scale = scale;
    curve.values.reserve(static_cast<std::size_t>(std::max(count, 0)));
    for (int i = 0; i < count; ++i) {
        curve.values.push_back(row.f(std::format("v{}", i)));
    }
    return curve;
}

SampledCurve readAffectCurve(const Database& db, int curveId)
{
    const std::optional<Row> row = db.queryOne("SELECT * FROM List_TireAffectCurve WHERE AffectCurveID = ?", {id(curveId)});
    if (!row) {
        return {};
    }
    return readCurve(*row, row->i("NumValues"), row->f("MinInput"), row->f("MaxInput"), row->f("OutputScale", 1.0F));
}

TireFrictionCurve readFrictionCurve(const Database& db, int multiCurveId)
{
    const std::optional<Row> multi = db.queryOne(
        "SELECT * FROM List_TireFrictionMultiCurve WHERE FrictionMultiCurveID = ?", {id(multiCurveId)});
    if (!multi) {
        throw DataError("gamedb: no tyre friction curve {}", multiCurveId);
    }
    TireFrictionCurve curve;
    curve.maxSlip = multi->f("MaxSlip", 1.0F);
    curve.lightLoadKgf = multi->f("MinLoadCurve", 10.1972F);
    curve.heavyLoadKgf = multi->f("MaxLoadCurve", 1019.72F);
    curve.loadClampKgf = multi->f("LoadClamp", 5000.0F);
    const auto readOne = [&](int curveId) {
        const std::optional<Row> row
            = db.queryOne("SELECT * FROM List_TireFrictionCurve WHERE FrictionCurveID = ?", {id(curveId)});
        if (!row) {
            throw DataError("gamedb: no tyre friction samples {}", curveId);
        }
        return readCurve(*row, row->i("NumCurveValues"), 0.0F, curve.maxSlip, row->f("FrictionScale", 1.0F));
    };
    curve.light = readOne(multi->i("TireFrictionCurveID0"));
    curve.heavy = readOne(multi->i("TireFrictionCurveID1"));
    return curve;
}

TireCompound readCompound(const Database& db, int compoundId)
{
    const std::optional<Row> row
        = db.queryOne("SELECT * FROM List_TireCompound WHERE TireCompoundID = ?", {id(compoundId)});
    if (!row) {
        throw DataError("gamedb: no tyre compound {}", compoundId);
    }
    TireCompound c;
    c.id = compoundId;
    c.name = row->text("DisplayName");
    c.lateralFrictionScale = row->f("TorqueFreeLatFrictionScale", 1.0F);
    c.brakingFrictionScale = row->f("TorqueFreeLongFrictionScaleBrake", 1.0F);
    c.accelFrictionScale0 = row->f("TorqueFreeLongFrictionScaleAccel0", 1.0F);
    c.accelFrictionScale1 = row->f("TorqueFreeLongFrictionScaleAccel1", 1.0F);
    c.accelScaleSpeed0 = row->f("TorqueFreeLongFrictionScaleAccelSpeed0", 5.0F);
    c.accelScaleSpeed1 = row->f("TorqueFreeLongFrictionScaleAccelSpeed1", 15.0F);
    c.wetFrictionScale = row->f("WetFrictionModFrictionScale", 0.8F);
    c.wetSlipScale = row->f("WetFrictionModSlipScale", 1.5F);
    c.momentOfInertia = row->f("MomentInertia", 1.6F);
    c.width0 = row->f("TireFricWidth0", 150.0F);
    c.width1 = row->f("TireFricWidth1", 650.0F);
    c.widthScale0 = row->f("TireFricScale0", 1.0F);
    c.widthScale1 = row->f("TireFricScale1", 1.0F);
    c.overheatedTempF = row->f("OverheatedTemp", 240.0F);
    for (std::size_t i = 0; i < c.hudTempsF.size(); ++i) {
        c.hudTempsF[i] = row->f(std::format("HUDTemps{}", i));
    }
    c.lateral = readFrictionCurve(db, row->i("FrictionMultiCurveLateralID"));
    c.longitudinal = readFrictionCurve(db, row->i("FrictionMultiCurveLongitudinalID"));
    c.tempAffectFriction = readAffectCurve(db, row->i("AffectCurveTempAffectFrictionID"));
    c.speedAffectFriction = readAffectCurve(db, row->i("AffectCurveSpeedAffectFrictionID"));
    c.camberAffectLateral = readAffectCurve(db, row->i("AffectCurveCamberAffectLatFrictionID"));
    return c;
}

AxleSuspension readSuspension(const Database& db, int springDamperId, int antiSwayId, float track)
{
    AxleSuspension s;
    const std::optional<Row> row = db.queryOne(
        "SELECT * FROM List_SpringDamperPhysics WHERE SpringDamperPhysicsID = ?", {id(springDamperId)});
    if (!row) {
        throw DataError("gamedb: no spring and damper row {}", springDamperId);
    }
    s.rideHeight = row->f("DefRideHeight", 0.12F);
    s.maxCompression = row->f("MaxCompressHeight", 0.06F);
    s.maxExtension = row->f("MaxStretchDeltaFromRideHeight", 0.1F);
    s.springRate = row->f("DefSpringRate", 50.0F) * kSpringUnit;
    s.bumpDamping = row->f("DefDampenBumpRate", 3.0F) * kDamperUnit;
    s.reboundDamping = row->f("DefDampenReboundRate", 5.0F) * kDamperUnit;
    s.bumpStopStiffness = row->f("BumpstopStiffness", 200.0F) * kSpringUnit;
    s.bumpStopDamping = row->f("BumpstopDamping", 2.0F) * kDamperUnit;
    s.staticCamberDeg = row->f("StaticCamber");
    s.staticToeDeg = row->f("StaticToe");
    s.casterDeg = row->f("Caster", 5.0F);
    s.camberGainCompression = row->f("CamberChangeRatioCompression");
    s.camberGainExpansion = row->f("CamberChangeRatioExpansion");
    if (const std::optional<Row> bar
        = db.queryOne("SELECT * FROM List_AntiSwayPhysics WHERE AntiSwayPhysicsID = ?", {id(antiSwayId)})) {
        // A bar resisting opposite wheel travel with rate k gives a roll
        // stiffness of k·track²/2 per radian.
        const float rate = bar->f("DefSwaybarStiffness") * kAntiRollUnit;
        s.antiRollStiffness = rate * track * track * 0.5F;
        s.antiRollDamping = bar->f("SwaybarDamping") * kDamperUnit * track * track * 0.5F * 0.01F;
    }
    return s;
}

Differential readDifferential(const Row& row, const char* axle)
{
    Differential d;
    d.lockAccel = row.f(std::format("{}LimitedSlipTorqueAccel", axle));
    d.lockDecel = row.f(std::format("{}LimitedSlipTorqueDecel", axle));
    d.relativeSpeedClamp = row.f(std::format("{}LimitedSlipRelVelClamp", axle), 50.0F);
    d.accelReferenceTorque = row.f(std::format("{}LimitedSlipAccelDefInputTorque", axle), 50.0F);
    return d;
}

/// Stock row of an upgrade table keyed by the car id; the lowest level when
/// no row is flagged stock.
std::optional<Row> stockRow(const Database& db, const std::string& table, const std::string& keyColumn, int key)
{
    return db.queryOne(std::format("SELECT * FROM {} WHERE {} = ? ORDER BY IsStock DESC, Level ASC LIMIT 1", table,
                           keyColumn),
        {id(key)});
}

void readBodyFromArchive(const Vfs& vfs, CarSpec& spec)
{
    const std::string path = std::format("media/cars/{}/physics/maxdata.xml", spec.mediaName);
    const std::optional<std::string> text = vfs.readText(path);
    if (!text) {
        return;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        logWarning("car {}: cannot parse {}", spec.mediaName, path);
        return;
    }
    const pugi::xml_node box = doc.child("MAXData").child("Collision").child("BoundingBox");
    if (box) {
        spec.boundsMin = {box.attribute("MinX").as_float(), box.attribute("MinY").as_float(),
            box.attribute("MinZ").as_float()};
        spec.boundsMax = {box.attribute("MaxX").as_float(), box.attribute("MaxY").as_float(),
            box.attribute("MaxZ").as_float()};
    }
    const pugi::xml_node misc = doc.child("MAXData").child("Misc");
    if (misc) {
        spec.wheelbase = misc.attribute("Wheelbase").as_float(spec.wheelbase);
    }
}

} // namespace

float SampledCurve::operator()(float input) const
{
    if (values.empty()) {
        return scale;
    }
    if (values.size() == 1 || maxInput <= minInput) {
        return values.front() * scale;
    }
    const float t = (input - minInput) / (maxInput - minInput) * static_cast<float>(values.size() - 1);
    if (t <= 0.0F) {
        return values.front() * scale;
    }
    const auto last = static_cast<float>(values.size() - 1);
    if (t >= last) {
        return values.back() * scale;
    }
    const auto i = static_cast<std::size_t>(t);
    const float f = t - static_cast<float>(i);
    return (values[i] + (values[i + 1] - values[i]) * f) * scale;
}

float TireFrictionCurve::friction(float slip, float loadN) const
{
    const float loadKgf = std::min(loadN / kKgfToNewton, loadClampKgf);
    const float s = std::min(std::abs(slip), maxSlip);
    const float t = std::clamp((loadKgf - lightLoadKgf) / (heavyLoadKgf - lightLoadKgf), 0.0F, 1.0F);
    return light(s) + (heavy(s) - light(s)) * t;
}

float TireFrictionCurve::peakSlip(float loadN) const
{
    constexpr int kSteps = 200;
    float best = 0.0F;
    float bestSlip = maxSlip * 0.1F;
    for (int i = 1; i <= kSteps; ++i) {
        const float slip = maxSlip * static_cast<float>(i) / kSteps;
        const float f = friction(slip, loadN);
        if (f > best) {
            best = f;
            bestSlip = slip;
        }
    }
    return bestSlip;
}

float TireCompound::widthScale(float widthMm) const
{
    if (width1 <= width0) {
        return widthScale0;
    }
    const float t = std::clamp((widthMm - width0) / (width1 - width0), 0.0F, 1.0F);
    return widthScale0 + (widthScale1 - widthScale0) * t;
}

float CarSpec::fullThrottleTorque(float rpm) const
{
    return torqueCurve(rpm) * gameTorqueScale;
}

CarSpec loadCarSpec(const Database& db, int carId, const Vfs* vfs)
{
    const std::optional<Row> car = db.queryOne("SELECT * FROM Data_Car WHERE Id = ?", {id(carId)});
    if (!car) {
        throw DataError("gamedb: no car {}", carId);
    }
    CarSpec spec;
    spec.id = carId;
    spec.mediaName = car->text("MediaName");
    spec.year = car->i("Year");
    spec.displayNameRef = car->text("DisplayName");
    spec.makeNameRef = car->text("MakeName");
    spec.classId = car->i("ClassID");
    spec.performanceIndex = car->f("PerformanceIndex");
    spec.massKg = car->f("CurbWeight", 14.0F) * kCurbWeightUnit;
    spec.frontWeight = car->f("WeightDistribution", 0.55F);
    spec.gameTorqueScale = car->f("GameTorqueScale", 1.0F);
    spec.gameDragScale = car->f("GameDragScale", 1.0F);
    spec.drive = static_cast<DriveType>(std::clamp(car->i("DriveTypeID", 2), 1, 3));

    spec.frontTires = {car->f("FrontTireWidthMM", 225.0F), car->f("FrontTireAspect", 45.0F),
        car->f("FrontWheelDiameterIN", 17.0F)};
    spec.rearTires
        = {car->f("RearTireWidthMM", 225.0F), car->f("RearTireAspect", 45.0F), car->f("RearWheelDiameterIN", 17.0F)};

    spec.dragAt100MphLbf = car->f("BodyAeroLongitudinalDrag", 200.0F);
    spec.verticalDrag = car->f("BodyAeroVerticalDrag", 500.0F);
    spec.lateralDragFront = car->f("BodyAeroLateralDragFront", 250.0F);
    spec.lateralDragRear = car->f("BodyAeroLateralDragRear", 250.0F);
    spec.frontDownforceAt100MphLbf = car->f("BodyAeroForwardDownforceFront");
    spec.rearDownforceAt100MphLbf = car->f("BodyAeroForwardDownforceRear");

    spec.steerMaxAngleDeg = car->f("SteerMaxAngle", 35.0F);
    spec.steerMaxAngleFilteredDeg = car->f("SteerMaxAngleFiltered", spec.steerMaxAngleDeg);
    spec.steerRateTurning = car->f("SteerMaxAngVelTurning", 100.0F);
    spec.steerRateStraighten = car->f("SteerMaxAngVelStraighten", 100.0F);
    spec.steerSpeedSensitiveMaxGees = car->f("SteerSpeedSensitiveMaxGees", 1.0F);
    spec.steerSpeedSensitiveMinAngleDeg = car->f("SteerSpeedSensitiveMinMaxAngle", 5.0F);
    spec.steerSpeedSensitiveSlowMph = car->f("SteerSpeedSensitiveSlowSpeed", 1.0F);
    spec.steerSpeedSensitiveFastMph = car->f("SteerSpeedSensitiveFastSpeed", 120.0F);
    spec.tcsSlipTakeoff = car->f("AssistsTCSSlipDefTakeoff", 33.0F);
    spec.tcsSlipMoving = car->f("AssistsTCSSlipDefMoving", 0.5F);

    spec.reference.timeTo60Mph = car->f("SimTimeTo60MPH");
    spec.reference.timeTo100Mph = car->f("SimTimeTo100MPH");
    spec.reference.quarterMileTime = car->f("SimTimeQuarterMile");
    spec.reference.quarterMileSpeed = car->f("SimSpeedQuarterMile");
    spec.reference.topSpeed = car->f("SimTopSpeed");
    spec.reference.brakeDistance60Mph = car->f("SimBrakeDistance60MPH");
    spec.reference.brakeDistance100Mph = car->f("SimBrakeDistance100MPH");
    spec.reference.lateralG60Mph = car->f("SimLatGees60MPH");
    spec.reference.lateralG120Mph = car->f("SimLatGees120MPH");
    // Power is stored in units of 100 W and torque in units of 100 N·m.
    spec.reference.peakPowerW = car->f("SimPeakPower") * 100.0F;
    spec.reference.peakTorqueNm = car->f("SimPeakTorque") * 100.0F;

    const int bodyId = [&] {
        const std::optional<Row> body = stockRow(db, "List_UpgradeCarBody", "Ordinal", carId);
        return body ? body->i("CarBodyID") : carId * 1000;
    }();
    if (const std::optional<Row> body = db.queryOne("SELECT * FROM Data_CarBody WHERE Id = ?", {id(bodyId)})) {
        spec.wheelbase = body->f("ModelWheelbase", spec.wheelbase);
        spec.frontTrack = body->f("ModelFrontTrackOuter", spec.frontTrack);
        spec.rearTrack = body->f("ModelRearTrackOuter", spec.rearTrack);
        spec.boundsMin = {body->f("PristineBoundingBoxMinX"), body->f("PristineBoundingBoxMinY"),
            body->f("PristineBoundingBoxMinZ")};
        spec.boundsMax = {body->f("PristineBoundingBoxMaxX"), body->f("PristineBoundingBoxMaxY"),
            body->f("PristineBoundingBoxMaxZ")};
    }
    // The track is measured across the outer tyre walls; the suspension works
    // at the tyre centres.
    spec.frontTrack -= spec.frontTires.widthMm * 0.001F;
    spec.rearTrack -= spec.rearTires.widthMm * 0.001F;
    if (vfs) {
        readBodyFromArchive(*vfs, spec);
    }

    // Engine
    const std::optional<Row> engineUpgrade = stockRow(db, "List_UpgradeEngine", "Ordinal", carId);
    if (!engineUpgrade) {
        throw DataError("gamedb: car {} has no engine", carId);
    }
    const int engineId = engineUpgrade->i("EngineID");
    const std::optional<Row> engine = db.queryOne("SELECT * FROM Data_Engine WHERE EngineID = ?", {id(engineId)});
    if (!engine) {
        throw DataError("gamedb: no engine {}", engineId);
    }
    spec.engineName = engine->text("EngineName");
    spec.engineInertia = engine->f("MomentInertia", 0.15F);
    spec.diesel = engine->i("Diesel") != 0;
    spec.rotary = engine->i("Rotary") != 0;
    const std::optional<Row> camshaft = stockRow(db, "List_UpgradeEngineCamshaft", "EngineID", engineId);
    if (!camshaft) {
        throw DataError("gamedb: engine {} has no camshaft row", engineId);
    }
    spec.redlineRpm = camshaft->f("RedlineRPM", 7000.0F);
    spec.idleRpm = camshaft->f("StallRPM", 800.0F);
    spec.launchRpm = camshaft->f("StartRPM", 4000.0F);
    spec.torqueCurveMaxRpm = camshaft->f("TorqueCurveMaxRPM", 9000.0F);
    const int torqueCurveId = camshaft->i("TorqueCurveFullThrottleID");
    const std::optional<Row> torque
        = db.queryOne("SELECT * FROM List_TorqueCurve WHERE TorqueCurveID = ?", {id(torqueCurveId)});
    if (!torque) {
        throw DataError("gamedb: no torque curve {}", torqueCurveId);
    }
    spec.torqueCurve = readCurve(*torque, torque->i("NumTorqueValues"), 0.0F, spec.torqueCurveMaxRpm,
        torque->f("TorqueScale", 100.0F));
    // Samples past the end of the curve hold the closed-throttle torque at
    // the top of the range as a negative fraction; keep only the powered
    // part, and take engine braking from ZeroThrottleTorqueScale.
    while (spec.torqueCurve.values.size() > 2 && spec.torqueCurve.values.back() <= 0.0F) {
        spec.torqueCurve.values.pop_back();
        spec.torqueCurve.maxInput -= spec.torqueCurveMaxRpm / static_cast<float>(torque->i("NumTorqueValues") - 1);
    }
    spec.engineBrakingTorque = torque->f("ZeroThrottleTorqueScale", 50.0F);
    if (const std::optional<Row> flywheel = stockRow(db, "List_UpgradeEngineFlywheel", "EngineID", engineId)) {
        spec.engineInertia = flywheel->f("MomentInertia", spec.engineInertia);
    }

    // Drivetrain
    const std::optional<Row> drivetrain = stockRow(db, "List_UpgradeDrivetrain", "Ordinal", carId);
    if (!drivetrain) {
        throw DataError("gamedb: car {} has no drivetrain", carId);
    }
    const int drivetrainId = drivetrain->i("DrivetrainID");
    if (const std::optional<Row> row = db.queryOne("SELECT * FROM Data_Drivetrain WHERE DrivetrainID = ?",
            {id(drivetrainId)})) {
        spec.drive = static_cast<DriveType>(std::clamp(row->i("DrivetypeID", static_cast<int>(spec.drive)), 1, 3));
    }
    const std::optional<Row> gearbox
        = stockRow(db, "List_UpgradeDrivetrainTransmission", "DrivetrainID", drivetrainId);
    if (!gearbox) {
        throw DataError("gamedb: drivetrain {} has no transmission", drivetrainId);
    }
    spec.finalDrive = gearbox->f("FinalDriveRatio", 3.5F);
    spec.shiftTime = gearbox->f("GearShiftTime", 0.2F);
    spec.drivetrainInertia = gearbox->f("MomentInertia", 0.01F);
    spec.reverseRatio = gearbox->f("GearRatio0", -3.5F);
    const int gearCount = gearbox->i("NumGears", 6);
    // GearRatio0 is reverse; the forward gears follow.
    for (int g = 1; g < gearCount && g <= 8; ++g) {
        const float ratio = gearbox->f(std::format("GearRatio{}", g), -1.0F);
        if (ratio > 0.0F) {
            spec.gears.push_back(ratio);
        }
    }
    if (spec.gears.empty()) {
        throw DataError("gamedb: drivetrain {} has no forward gears", drivetrainId);
    }
    if (const std::optional<Row> clutch = stockRow(db, "List_UpgradeDrivetrainClutch", "DrivetrainID", drivetrainId)) {
        spec.clutchMaxTorque = clutch->f("ClutchMaxTorque", 800.0F);
        spec.clutchInTime = clutch->f("ClutchInTime", 0.05F);
        spec.clutchOutTime = clutch->f("ClutchOutTime", 0.05F);
    }
    if (const std::optional<Row> diff
        = stockRow(db, "List_UpgradeDrivetrainDifferential", "DrivetrainID", drivetrainId)) {
        spec.frontDiff = readDifferential(*diff, "Front");
        spec.rearDiff = readDifferential(*diff, "Rear");
        spec.centreDiff = readDifferential(*diff, "Center");
        spec.rearTorqueSplit = diff->f("RearToqueSplit", 0.5F);
    }

    // Tyres
    const std::optional<Row> tires = stockRow(db, "List_UpgradeTireCompound", "Ordinal", carId);
    spec.compound = readCompound(db, tires ? tires->i("TireCompoundID") : 5);

    // Suspension
    const std::optional<Row> springs = stockRow(db, "List_UpgradeSpringDamper", "Ordinal", carId);
    if (!springs) {
        throw DataError("gamedb: car {} has no suspension", carId);
    }
    const std::optional<Row> barFront = stockRow(db, "List_UpgradeAntiSwayFront", "Ordinal", carId);
    const std::optional<Row> barRear = stockRow(db, "List_UpgradeAntiSwayRear", "Ordinal", carId);
    spec.front = readSuspension(db, springs->i("FrontSpringDamperPhysicsID"),
        barFront ? barFront->i("AntiSwayPhysicsID") : 0, spec.frontTrack);
    spec.rear = readSuspension(db, springs->i("RearSpringDamperPhysicsID"),
        barRear ? barRear->i("AntiSwayPhysicsID") : 0, spec.rearTrack);

    // The database gives no centre-of-mass height. Road cars carry it at
    // roughly a third of the body height above the ground, plus the ride
    // height the suspension adds.
    const float bodyHeight = spec.boundsMax[1] - spec.boundsMin[1];
    spec.cgHeight = std::max(0.25F, 0.5F * (spec.front.rideHeight + spec.rear.rideHeight) + bodyHeight * 0.3F);

    // Brakes
    if (const std::optional<Row> brakes = stockRow(db, "List_UpgradeBrakes", "Ordinal", carId)) {
        spec.brakeFrictionScale = brakes->f("GameFrictionScaleBraking", 1.0F);
        spec.brakeTorqueSlider = brakes->f("BrakeTorqueSlider", 0.5F);
        spec.brakeBias = brakes->f("BrakeBiasSlider", 0.5F);
        spec.handbrakeScale = brakes->f("BiasHandbrake", 1.5F);
        spec.absReleasePoint = brakes->f("ReleasePointABS", 0.7F);
        spec.frontBrakeSize = brakes->i("FrontBrakeSize", 12);
        spec.rearBrakeSize = brakes->i("RearBrakeSize", 12);
    }

    // Aero parts add to the body's own figures.
    const auto addAero = [&](const std::optional<Row>& part, bool rear) {
        if (!part) {
            return;
        }
        const std::optional<Row> aero = db.queryOne(
            "SELECT * FROM List_AeroPhysics WHERE AeroPhysicsID = ?", {id(part->i("AeroPhysicsID"))});
        if (!aero) {
            return;
        }
        const float slider = aero->f("DefaultTuneSlider", 0.5F);
        const float drag = aero->f("Drag0") + (aero->f("Drag1") - aero->f("Drag0")) * slider;
        const float downforce = aero->f("Downforce0") + (aero->f("Downforce1") - aero->f("Downforce0")) * slider;
        spec.dragAt100MphLbf += drag;
        (rear ? spec.rearDownforceAt100MphLbf : spec.frontDownforceAt100MphLbf) += downforce;
    };
    addAero(stockRow(db, "List_UpgradeCarBodyFrontBumper", "CarBodyID", bodyId), false);
    addAero(stockRow(db, "List_UpgradeRearWing", "Ordinal", carId), true);

    return spec;
}

std::vector<int> drivableCarIds(const Database& db)
{
    std::vector<int> ids;
    for (const Row& row : db.query("SELECT Id FROM Data_Car WHERE IsDrivable = 1 ORDER BY Id")) {
        ids.push_back(row.i("Id"));
    }
    return ids;
}

int carIdByMediaName(const Database& db, const std::string& mediaName)
{
    const std::optional<Row> row
        = db.queryOne("SELECT Id FROM Data_Car WHERE MediaName = ? COLLATE NOCASE", {mediaName});
    return row ? row->i("Id") : -1;
}

} // namespace fh
