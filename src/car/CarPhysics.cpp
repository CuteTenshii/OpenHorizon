#include "CarPhysics.h"


#include <algorithm>
#include <cmath>

namespace fh {

namespace {

constexpr float kGravity = 9.80665F;
constexpr float kPi = 3.14159265F;
constexpr float kRadToDeg = 180.0F / kPi;
constexpr float kDegToRad = kPi / 180.0F;
constexpr float kRpmPerRadPerSec = 60.0F / (2.0F * kPi);
constexpr float kLbfToNewton = 4.44822F;
/// The game tabulates aerodynamic forces at 100 mph.
constexpr float kAeroReferenceSpeed = 44.704F;
constexpr float kMphToMs = 0.44704F;
/// Extra length above the compressed wheel where suspension rays start, so a
/// wheel pushed past its travel into a kerb still finds the ground.
constexpr float kRayHeadroom = 0.3F;
/// Distance over which a tyre builds up its slip angle, metres.
constexpr float kRelaxationLength = 0.25F;
/// Below this speed tyre slip is ill-defined and a damping model holds the
/// car instead.
constexpr float kLowSpeed = 2.0F;
constexpr float kRollingResistance = 0.013F;
/// Lateral grip multiplier fitted so the stock cars match the game's
/// recorded skidpad figures (Data_Car.SimLatGees60MPH) across the whole car
/// list; braking figures already match without one. The game scales each
/// tyre's friction by a per-tyre factor times the tyre-width factor (see the
/// tyre setup at 0x82DD47E0 in the title update); that per-tyre factor has
/// not been traced to its source yet, and this constant stands in for it.
constexpr float kLateralGripCalibration = 1.10F;
/// Brake torque at full pedal as a multiple of what stops the car at 1 g,
/// enough to lock the wheels on any stock tyre.
constexpr float kBrakeCapacityG = 1.4F;

float smoothSign(float value, float width)
{
    return std::clamp(value / width, -1.0F, 1.0F);
}

float moveTowards(float current, float target, float maxDelta)
{
    if (current < target) {
        return std::min(current + maxDelta, target);
    }
    return std::max(current - maxDelta, target);
}

} // namespace

bool FlatGround::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, GroundHit& hit) const
{
    if (direction.y >= -1e-6F || origin.y < 0.0F) {
        return false;
    }
    const float t = -origin.y / direction.y;
    if (t > maxDistance) {
        return false;
    }
    hit.point = origin + direction * t;
    hit.normal = {0.0F, 1.0F, 0.0F};
    hit.distance = t;
    hit.grip = 1.0F;
    hit.surface = 0;
    hit.drag = 0.0F;
    return true;
}

CarPhysics::CarPhysics(const CarSpec& spec)
    : m_spec(spec)
{
    m_mass = m_spec.massKg;
    const float wheelbase = m_spec.wheelbase;
    m_cgLocal = {0.0F, m_spec.cgHeight, wheelbase * (m_spec.frontWeight - 0.5F)};

    // The game sizes the body's inertia by a box of its own
    // (List_UpgradeCarBodyWeight.BlockDimX/Y/Z, read next to the mass and
    // centre of mass); taking it as a uniform solid box is inferred from the
    // columns' names, as the use of the block has not been traced.
    const float width = m_spec.inertiaBlock[0];
    const float height = m_spec.inertiaBlock[1];
    const float length = m_spec.inertiaBlock[2];
    const float k = m_mass / 12.0F;
    m_inertiaLocal = {k * (height * height + length * length), k * (width * width + length * length),
        k * (width * width + height * height)};

    const float frontRadius = m_spec.frontTires.radius();
    const float rearRadius = m_spec.rearTires.radius();
    m_wheelRest[FrontLeft] = {-m_spec.frontTrack * 0.5F, frontRadius, wheelbase * 0.5F};
    m_wheelRest[FrontRight] = {m_spec.frontTrack * 0.5F, frontRadius, wheelbase * 0.5F};
    m_wheelRest[RearLeft] = {-m_spec.rearTrack * 0.5F, rearRadius, -wheelbase * 0.5F};
    m_wheelRest[RearRight] = {m_spec.rearTrack * 0.5F, rearRadius, -wheelbase * 0.5F};

    const float weight = m_mass * kGravity;
    for (int w = 0; w < kWheels; ++w) {
        const bool front = w < 2;
        m_staticLoad[w] = weight * (front ? m_spec.frontWeight : 1.0F - m_spec.frontWeight) * 0.5F;
        m_wheelInertia[w] = m_spec.compound.momentOfInertia;
        m_peakSlipRatio[w] = std::max(0.02F, m_spec.compound.longitudinal.peakSlip(m_staticLoad[w]));
        m_peakSlipAngle[w] = std::max(1.0F, m_spec.compound.lateral.peakSlip(m_staticLoad[w]));
    }
    m_engineSpeed = m_spec.idleRpm / kRpmPerRadPerSec;
    buildLockTable();
}

glm::vec3 CarPhysics::wheelRestPosition(int wheel) const
{
    return m_wheelRest[static_cast<std::size_t>(wheel)];
}

glm::vec3 CarPhysics::position() const
{
    return m_cgPosition - m_orientation * m_cgLocal;
}

glm::mat4 CarPhysics::transform() const
{
    glm::mat4 m = glm::mat4_cast(m_orientation);
    m[3] = glm::vec4(position(), 1.0F);
    return m;
}

float CarPhysics::forwardSpeed() const
{
    return glm::dot(m_velocity, forward());
}

float CarPhysics::engineRpm() const
{
    return m_engineSpeed * kRpmPerRadPerSec;
}

bool CarPhysics::allWheelsOffGround() const
{
    return std::none_of(m_wheels.begin(), m_wheels.end(), [](const WheelState& w) { return w.onGround; });
}

glm::vec3 CarPhysics::worldPointVelocity(const glm::vec3& worldPoint) const
{
    return m_velocity + glm::cross(m_angularVelocity, worldPoint - m_cgPosition);
}

glm::mat3 CarPhysics::worldInverseInertia() const
{
    const glm::mat3 r = glm::mat3_cast(m_orientation);
    const glm::mat3 inverseLocal(glm::vec3(1.0F / m_inertiaLocal.x, 0.0F, 0.0F),
        glm::vec3(0.0F, 1.0F / m_inertiaLocal.y, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F / m_inertiaLocal.z));
    return r * inverseLocal * glm::transpose(r);
}

void CarPhysics::reset(const glm::vec3& position, float heading, const GroundQuery& ground)
{
    m_orientation = glm::angleAxis(heading, glm::vec3(0.0F, 1.0F, 0.0F));
    glm::vec3 origin = position;
    GroundHit hit;
    if (ground.raycast(position + glm::vec3(0.0F, 50.0F, 0.0F), glm::vec3(0.0F, -1.0F, 0.0F), 200.0F, hit)) {
        origin.y = hit.point.y;
    }
    m_cgPosition = origin + m_orientation * m_cgLocal;
    m_velocity = glm::vec3(0.0F);
    m_angularVelocity = glm::vec3(0.0F);
    for (WheelState& wheel : m_wheels) {
        wheel = WheelState{};
    }
    m_steerAngle = 0.0F;
    m_steerRate = 0.0F;
    m_findPeakTime = 0.0F;
    m_findPeakExtra = 0.0F;
    m_gear = 1;
    m_pendingGear = 1;
    m_shiftTimer = 0.0F;
    m_engineSpeed = m_spec.idleRpm / kRpmPerRadPerSec;
    m_tcsCut = 0.0F;
    m_accumulator = 0.0F;
}

void CarPhysics::recover(const GroundQuery& ground)
{
    const glm::vec3 f = forward();
    const float heading = std::atan2(f.x, f.z);
    reset(position(), heading, ground);
}

void CarPhysics::setState(const glm::vec3& position, const glm::quat& orientation, const glm::vec3& velocity,
    const glm::vec3& angularVelocity)
{
    m_orientation = glm::normalize(orientation);
    m_cgPosition = position + m_orientation * m_cgLocal;
    m_velocity = velocity;
    m_angularVelocity = angularVelocity;
}

void CarPhysics::applyImpulse(const glm::vec3& impulse, const glm::vec3& worldPoint, float angularScale)
{
    m_velocity += impulse / m_mass;
    m_angularVelocity += worldInverseInertia() * glm::cross(worldPoint - m_cgPosition, impulse) * angularScale;
}

float CarPhysics::impulseResponse(const glm::vec3& worldPoint, const glm::vec3& direction, float angularScale) const
{
    const glm::vec3 r = worldPoint - m_cgPosition;
    const glm::vec3 angular = glm::cross(worldInverseInertia() * glm::cross(r, direction), r);
    return 1.0F / m_mass + glm::dot(direction, angular) * angularScale;
}

float CarPhysics::resolveContact(
    const glm::vec3& point, const glm::vec3& normal, float depth, float restitution, float friction)
{
    const glm::vec3 r = point - m_cgPosition;
    const glm::mat3 inverseInertia = worldInverseInertia();
    const auto effectiveMass = [&](const glm::vec3& direction) {
        const glm::vec3 angular = glm::cross(inverseInertia * glm::cross(r, direction), r);
        return 1.0F / (1.0F / m_mass + glm::dot(direction, angular));
    };
    const glm::vec3 velocity = worldPointVelocity(point);
    const float approach = glm::dot(velocity, normal);
    float normalImpulse = 0.0F;
    if (approach < 0.0F) {
        // Slow contacts do not bounce, so a car resting against a wall stays
        // there instead of jittering.
        const float bounce = approach < -2.0F ? restitution : 0.0F;
        normalImpulse = -(1.0F + bounce) * approach * effectiveMass(normal);
        applyImpulse(normal * normalImpulse, point);

        const glm::vec3 after = worldPointVelocity(point);
        glm::vec3 tangent = after - normal * glm::dot(after, normal);
        const float slide = glm::length(tangent);
        if (slide > 1e-4F) {
            tangent /= slide;
            const float frictionImpulse = std::min(slide * effectiveMass(tangent), friction * normalImpulse);
            applyImpulse(-tangent * frictionImpulse, point);
        }
    }
    // Remove most of the overlap directly; the rest goes next step, which
    // avoids the car popping out of deep contacts.
    constexpr float kSlop = 0.005F;
    if (depth > kSlop) {
        m_cgPosition += normal * ((depth - kSlop) * 0.6F);
    }
    return normalImpulse;
}

void CarPhysics::update(float dt, const CarControls& controls, const GroundQuery& ground)
{
    constexpr float kStep = 1.0F / kStepRate;
    // Bounded so a long stall does not make the car run hundreds of steps.
    m_accumulator = std::min(m_accumulator + dt, 0.1F);
    while (m_accumulator >= kStep) {
        step(controls, ground);
        m_accumulator -= kStep;
    }
}

bool CarPhysics::isDriven(int wheel) const
{
    switch (m_spec.drive) {
    case DriveType::Fwd:
        return wheel < 2;
    case DriveType::Rwd:
        return wheel >= 2;
    case DriveType::Awd:
        return true;
    }
    return false;
}

float CarPhysics::drivenWheelSpeed() const
{
    const auto axle = [this](int left) { return 0.5F * (m_wheels[left].spin + m_wheels[left + 1].spin); };
    switch (m_spec.drive) {
    case DriveType::Fwd:
        return axle(FrontLeft);
    case DriveType::Rwd:
        return axle(RearLeft);
    case DriveType::Awd:
        return axle(FrontLeft) * (1.0F - m_spec.rearTorqueSplit) + axle(RearLeft) * m_spec.rearTorqueSplit;
    }
    return 0.0F;
}

float CarPhysics::currentRatio() const
{
    if (m_gear > 0) {
        return m_spec.totalDriveRatio(m_gear - 1);
    }
    if (m_gear < 0) {
        return m_spec.reverseRatio * m_spec.finalDrive;
    }
    return 0.0F;
}

float CarPhysics::engineTorque(float rpm, float throttle) const
{
    const float full = m_spec.fullThrottleTorque(std::max(rpm, 0.0F));
    const float braking = m_spec.engineBrakingTorque * std::clamp(rpm / m_spec.redlineRpm, 0.0F, 1.0F);
    float torque = throttle * full - (1.0F - throttle) * braking;
    // An idle controller keeps the engine running when it is not driving.
    if (rpm < m_spec.idleRpm * 1.05F) {
        const float deficit = std::clamp((m_spec.idleRpm * 1.05F - rpm) / (m_spec.idleRpm * 0.2F), 0.0F, 1.0F);
        torque = std::max(torque, deficit * full * 0.6F);
    }
    return torque;
}

float CarPhysics::lateralCapability(float speed) const
{
    // After CCarDynamics' virtual at 0x82DE61B8: the axle loads from the
    // weight split plus the aerodynamic downforce at this speed, then six
    // rounds of summing each tyre's peak lateral force at its load and
    // moving load to the outer tyre by that force times the height over the
    // track. The game's height is the centre of mass less a per-axle value
    // (vehicle +0x1E24, +0x2644) that has not been traced; the centre of
    // mass height stands in for it.
    const float q = (speed / kAeroReferenceSpeed) * (speed / kAeroReferenceSpeed);
    const float weight = m_mass * kGravity;
    const float frontAxle
        = std::max(0.0F, weight * m_spec.frontWeight + m_spec.frontDownforceAt100MphLbf * kLbfToNewton * q);
    const float rearAxle
        = std::max(0.0F, weight * (1.0F - m_spec.frontWeight) + m_spec.rearDownforceAt100MphLbf * kLbfToNewton * q);
    const auto tyreScale = [this](int wheel) {
        const float width = wheel < 2 ? m_spec.frontTires.widthMm : m_spec.rearTires.widthMm;
        return m_spec.compound.widthScale(width) * kLateralGripCalibration;
    };
    const auto force = [&](int wheel, float load) {
        return m_spec.compound.lateral.peakFriction(load) * tyreScale(wheel) * load;
    };
    float frontOuter = frontAxle * 0.5F;
    float frontInner = frontOuter;
    float rearOuter = rearAxle * 0.5F;
    float rearInner = rearOuter;
    constexpr int kRounds = 6;
    for (int round = 0; round < kRounds; ++round) {
        const float frontForce = force(FrontLeft, frontOuter) + force(FrontRight, frontInner);
        const float rearForce = force(RearLeft, rearOuter) + force(RearRight, rearInner);
        const float frontShift = frontForce * m_spec.cgHeight / m_spec.frontTrack;
        const float rearShift = rearForce * m_spec.cgHeight / m_spec.rearTrack;
        frontOuter = frontAxle * 0.5F + frontShift;
        frontInner = std::max(0.0F, frontAxle * 0.5F - frontShift);
        rearOuter = rearAxle * 0.5F + rearShift;
        rearInner = std::max(0.0F, rearAxle * 0.5F - rearShift);
    }
    const float total = force(FrontLeft, frontOuter) + force(FrontRight, frontInner) + force(RearLeft, rearOuter)
        + force(RearRight, rearInner);
    return total / m_mass;
}

void CarPhysics::buildLockTable()
{
    // 0x82DE7E70: at each sample speed, the kinematic steering angle for a
    // turn at SteerSpeedSensitiveMaxGees times the tyres' lateral capability
    // (tan = wheelbase * acceleration / speed^2), kept between
    // SteerSpeedSensitiveMinMaxAngle and SteerMaxAngle.
    const float maxAngle = m_spec.steerMaxAngleDeg * kDegToRad;
    const float minAngle = m_spec.steerSpeedSensitiveMinAngleDeg * kDegToRad;
    const float lower = std::min(minAngle, maxAngle);
    const float upper = std::max(minAngle, maxAngle);
    const float spacing = kLockTableTopSpeed / static_cast<float>(kLockTableSize - 1);
    for (int i = 0; i < kLockTableSize; ++i) {
        const float speed = std::max(static_cast<float>(i) * spacing, 0.01F);
        const float angle = std::atan(
            m_spec.steerSpeedSensitiveMaxGees * m_spec.wheelbase * lateralCapability(speed) / (speed * speed));
        m_lockTable[static_cast<std::size_t>(i)] = std::clamp(angle, lower, upper);
    }
}

float CarPhysics::lockAt(float speed) const
{
    // 0x82DF21B8: Catmull-Rom through the table, the end samples extended
    // linearly, capped at the full lock.
    const float spacing = kLockTableTopSpeed / static_cast<float>(kLockTableSize - 1);
    const float position = std::clamp(speed, 0.0F, kLockTableTopSpeed) / spacing;
    const int last = kLockTableSize - 1;
    const int i0 = std::min(static_cast<int>(position), last);
    const int i1 = std::min(i0 + 1, last);
    const float t = position - std::floor(position);
    const auto at = [this](int i) { return m_lockTable[static_cast<std::size_t>(i)]; };
    const float p1 = at(i0);
    const float p2 = at(i1);
    const float p0 = i0 > 0 ? at(i0 - 1) : 2.0F * p1 - p2;
    const float p3 = i1 < last ? at(i1 + 1) : 2.0F * p2 - p1;
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float value = p0 * (-0.5F * t3 + t2 - 0.5F * t) + p1 * (1.5F * t3 - 2.5F * t2 + 1.0F)
        + p2 * (-1.5F * t3 + 2.0F * t2 + 0.5F * t) + p3 * (0.5F * t3 - 0.5F * t2);
    return std::min(value, m_spec.steerMaxAngleDeg * kDegToRad);
}

float CarPhysics::normalisedSlip(int wheel) const
{
    // Slip angle over the tyre's peak slip angle; positive when the tyre
    // pushes the car to the right, the way a right turn (positive steer)
    // loads it. The game weighs a per-wheel value by load that its tunable
    // NormRearSlipAngleToCountersteer names a normalised slip angle; that
    // the value is exactly this ratio is inferred from that name and the
    // 1.05 threshold the steering compares it with.
    const WheelState& state = m_wheels[static_cast<std::size_t>(wheel)];
    return state.relaxedSlipAngle * kRadToDeg / m_peakSlipAngle[static_cast<std::size_t>(wheel)];
}

void CarPhysics::steerTowards(float target, float rate, float acceleration, float dt)
{
    const float difference = target - m_steerAngle;
    if ((m_steerRate > 0.0F) != (difference > 0.0F)) {
        m_steerRate = 0.0F;
    }
    m_steerRate += (difference >= 0.0F ? 1.0F : -1.0F) * acceleration * dt;
    m_steerRate = std::clamp(m_steerRate, -rate, rate);
    if (std::abs(m_steerRate) * dt > std::abs(difference)) {
        m_steerRate = difference / dt;
    }
    m_steerAngle += m_steerRate * dt;
}

void CarPhysics::updateSteering(const CarControls& controls, float dt)
{
    const float input = std::clamp(controls.steer, -1.0F, 1.0F);
    const float turnRate = m_spec.steerRateTurning * kDegToRad;
    const float straightenRate = m_spec.steerRateStraighten * kDegToRad;
    const float accelerationTime = std::max(m_spec.steerAccelTimeToMaxRate, 1e-4F);
    const float current = m_steerAngle;

    if (!m_assists.speedSensitiveSteering) {
        const float target = input * m_spec.steerMaxAngleFilteredDeg * kDegToRad;
        const bool turningIn = std::abs(target) > std::abs(current) && target * current >= 0.0F;
        m_steerAngle = moveTowards(current, target, (turningIn ? turnRate : straightenRate) * dt);
        m_steerRate = 0.0F;
    } else {
        // Wheel loads weight the per-axle slips (0x82DF9620).
        const auto axleSlip = [this](int left) {
            const float loadLeft = std::max(0.0F, m_wheels[left].load);
            const float loadRight = std::max(0.0F, m_wheels[left + 1].load);
            constexpr float kMinLoad = 1e-7F;
            return (normalisedSlip(left) * loadLeft + normalisedSlip(left + 1) * loadRight)
                / std::max(loadLeft + loadRight, kMinLoad);
        };
        const float frontSlip = axleSlip(FrontLeft);
        const float rearSlip = axleSlip(RearLeft);
        const float speed = forwardSpeed();
        const float maxAngle = m_spec.steerMaxAngleDeg * kDegToRad;
        // The game checks the second front wheel when steering right and the
        // first otherwise; which of its wheels is left is not traced, so the
        // wheel on the inside of the turn is taken, with its slip opposing
        // the steering.
        const bool frontOpposes = current > 0.0F ? normalisedSlip(FrontRight) <= 0.0F : normalisedSlip(FrontLeft) > 0.0F;
        const bool countersteer = std::abs(rearSlip) > m_spec.normRearSlipAngleToCountersteer;
        constexpr float kMinAssistSpeed = 2.0F;
        const bool releasing = (current > 0.0F && input <= 0.0F) || (current < 0.0F && input >= 0.0F)
            || (current == 0.0F && input == 0.0F);

        if (speed < kMinAssistSpeed || frontOpposes || countersteer || releasing) {
            // The plain path: the stick's share of the full lock.
            const float target = input * maxAngle;
            if (countersteer) {
                const float rate = m_spec.steerRateCountersteer * kDegToRad;
                const float difference = target - current;
                m_steerRate = std::abs(difference) > rate * dt ? std::copysign(rate, difference) : difference / dt;
                m_steerAngle += m_steerRate * dt;
            } else {
                const bool turningIn
                    = (current >= 0.0F && target > current) || (current <= 0.0F && target < current);
                const float rate = turningIn ? turnRate : straightenRate;
                steerTowards(target, rate, rate / accelerationTime, dt);
            }
            m_findPeakTime = 0.0F;
            m_findPeakExtra = 0.0F;
        } else {
            const float slow = m_spec.steerSpeedSensitiveSlowMph * kMphToMs;
            const float fast = m_spec.steerSpeedSensitiveFastMph * kMphToMs;
            const float lowSpeed = std::min(slow, fast);
            const float highSpeed = std::max(slow, fast);
            const float minAngle = m_spec.steerSpeedSensitiveMinAngleDeg * kDegToRad;
            const float lock
                = std::clamp(lockAt(speed), std::min(minAngle, maxAngle), std::max(minAngle, maxAngle));
            const float target = input * lock;

            // The turning rate shrinks with the lock the table allows, and
            // by SteerSpeedSensitiveFastRateScale towards the fast speed.
            const float lockRatio = lockAt(std::clamp(speed, lowSpeed, highSpeed)) / lockAt(slow);
            float rateScale = 1.0F;
            if (speed >= highSpeed) {
                rateScale = m_spec.steerSpeedSensitiveFastRateScale;
            } else if (speed > lowSpeed) {
                rateScale = 1.0F
                    + (speed - lowSpeed) / (highSpeed - lowSpeed) * (m_spec.steerSpeedSensitiveFastRateScale - 1.0F);
            }
            float rate = lockRatio * turnRate * rateScale;

            // Holding the stick at full lock with the front tyres short of
            // their peak adds lock at SteerAngVelDynFindPeak.
            constexpr float kFullInput = 0.976F;
            constexpr float kFrontPeak = 1.05F;
            const bool fullInput = std::abs(input) >= kFullInput;
            if (fullInput) {
                const bool atLock = std::abs(current) >= lock - rate * dt;
                if (atLock && std::abs(frontSlip) < kFrontPeak) {
                    m_findPeakTime += dt;
                    rate = std::min(rate, m_spec.steerRateDynFindPeak * kDegToRad);
                    m_findPeakExtra += (target >= 0.0F ? 1.0F : -1.0F) * rate * dt;
                } else {
                    m_findPeakTime = 0.0F;
                }
            } else {
                m_findPeakTime = 0.0F;
                m_findPeakExtra = 0.0F;
            }
            float goal = std::clamp(m_findPeakExtra + target, -maxAngle, maxAngle);
            if (fullInput && ((input > 0.0F && goal < current) || (input < 0.0F && goal > current))) {
                goal = current;
            }
            // Past SteerFrictionClamp the front tyres get no more lock than
            // they have.
            if (std::abs(frontSlip) >= m_spec.steerFrictionClamp && current * frontSlip > 0.0F) {
                const float hold = std::max(std::abs(current), minAngle);
                goal = std::clamp(goal, -hold, hold);
            }
            steerTowards(goal, rate, rate / accelerationTime, dt);
        }
    }

    // Ackermann geometry: the inner wheel turns tighter.
    const float angle = m_steerAngle;
    if (std::abs(angle) < 1e-4F) {
        m_wheels[FrontLeft].steerAngle = angle;
        m_wheels[FrontRight].steerAngle = angle;
    } else {
        const float radius = m_spec.wheelbase / std::tan(std::abs(angle));
        const float halfTrack = m_spec.frontTrack * 0.5F;
        const float inner = std::atan(m_spec.wheelbase / std::max(radius - halfTrack, 0.1F));
        const float outer = std::atan(m_spec.wheelbase / (radius + halfTrack));
        const float sign = angle > 0.0F ? 1.0F : -1.0F;
        // Positive steer turns right, so the right wheel is inner.
        m_wheels[FrontRight].steerAngle = sign > 0.0F ? inner : -outer;
        m_wheels[FrontLeft].steerAngle = sign > 0.0F ? outer : -inner;
    }
    m_wheels[RearLeft].steerAngle = 0.0F;
    m_wheels[RearRight].steerAngle = 0.0F;
}

void CarPhysics::updateGearbox(const CarControls& controls, float dt)
{
    if (m_shiftTimer > 0.0F) {
        m_shiftTimer -= dt;
        if (m_shiftTimer <= 0.0F) {
            m_shiftTimer = 0.0F;
            m_gear = m_pendingGear;
        }
        return;
    }
    const int topGear = static_cast<int>(m_spec.gears.size());
    const auto shiftTo = [this](int gear) {
        if (gear == m_gear) {
            return;
        }
        m_pendingGear = gear;
        m_shiftTimer = m_spec.shiftTime;
    };

    if (!m_assists.automaticGearbox) {
        if (controls.shiftUp && m_gear < topGear) {
            shiftTo(m_gear + 1);
        } else if (controls.shiftDown && m_gear > -1) {
            shiftTo(m_gear - 1);
        }
        return;
    }

    const float speed = forwardSpeed();
    // As in the game, holding the brake at a standstill selects reverse and
    // the throttle selects first again.
    if (m_gear >= 0 && speed < 0.5F && controls.brake > 0.5F && controls.throttle < 0.1F) {
        m_gear = -1;
        m_pendingGear = -1;
        return;
    }
    if (m_gear < 0) {
        if (speed > -0.5F && controls.throttle > 0.1F) {
            m_gear = 1;
            m_pendingGear = 1;
        }
        return;
    }
    if (m_gear == 0) {
        m_gear = 1;
        m_pendingGear = 1;
        return;
    }
    const float rpm = engineRpm();
    const bool grounded = std::count_if(m_wheels.begin(), m_wheels.end(), [](const WheelState& w) {
        return w.onGround;
    }) >= 2;
    const float coupledRpm = drivenWheelSpeed() * currentRatio() * kRpmPerRadPerSec;
    if (m_gear < topGear && rpm > m_spec.redlineRpm * 0.97F && coupledRpm > m_spec.redlineRpm * 0.9F && grounded) {
        shiftTo(m_gear + 1);
        return;
    }
    if (m_gear > 1) {
        const float ratioDown = m_spec.gears[static_cast<std::size_t>(m_gear - 2)];
        const float ratioNow = m_spec.gears[static_cast<std::size_t>(m_gear - 1)];
        const float rpmAfter = coupledRpm * ratioDown / ratioNow;
        // Downshift earlier under braking, to have power on the way out of
        // the corner, but never into the limiter.
        const float threshold = controls.brake > 0.3F ? 0.65F : 0.5F;
        if (rpmAfter < m_spec.redlineRpm * 0.88F && coupledRpm < m_spec.redlineRpm * threshold) {
            shiftTo(m_gear - 1);
        }
    }
}

void CarPhysics::computeSuspension(const GroundQuery& ground, float dt, glm::vec3& force, glm::vec3& torque)
{
    const glm::vec3 up = this->up();
    std::array<float, kWheels> springForce{};
    for (int w = 0; w < kWheels; ++w) {
        WheelState& wheel = m_wheels[w];
        const AxleSuspension& axle = w < 2 ? m_spec.front : m_spec.rear;
        const float radius = wheelRadius(w);
        const glm::vec3 restWorld = position() + m_orientation * m_wheelRest[w];
        // The spring is preloaded so the car sits at its ride height; it
        // cannot extend past its free length, where that preload is spent.
        const float extension = std::min(axle.maxExtension, m_staticLoad[w] / axle.springRate + 0.02F);
        const float headroom = axle.maxCompression + kRayHeadroom;
        const glm::vec3 origin = restWorld + up * headroom;
        const float reach = headroom + extension + radius;

        GroundHit hit;
        const float previous = wheel.compression;
        if (ground.raycast(origin, -up, reach, hit)) {
            wheel.onGround = true;
            wheel.ground = hit;
            wheel.compression = headroom - (hit.distance - radius);
        } else {
            wheel.onGround = false;
            wheel.compression = -extension;
        }
        wheel.compressionSpeed = (wheel.compression - previous) / dt;
        wheel.hubWorld = restWorld + up * wheel.compression;

        if (!wheel.onGround) {
            wheel.load = 0.0F;
            continue;
        }
        float f = m_staticLoad[w] + axle.springRate * wheel.compression;
        const float v = wheel.compressionSpeed;
        f += v > 0.0F ? axle.bumpDamping * v : axle.reboundDamping * v;
        if (wheel.compression > axle.maxCompression) {
            f += axle.bumpStopStiffness * (wheel.compression - axle.maxCompression) + axle.bumpStopDamping * v;
        }
        springForce[w] = f;
    }

    // Anti-roll bars push against the difference in travel across an axle.
    const auto antiRoll = [&](int left, const AxleSuspension& axle, float track) {
        if (!m_wheels[left].onGround && !m_wheels[left + 1].onGround) {
            return;
        }
        const float difference = m_wheels[left].compression - m_wheels[left + 1].compression;
        const float speedDifference = m_wheels[left].compressionSpeed - m_wheels[left + 1].compressionSpeed;
        const float barForce = (axle.antiRollStiffness * difference + axle.antiRollDamping * speedDifference)
            / (track * track);
        if (m_wheels[left].onGround) {
            springForce[left] += barForce;
        }
        if (m_wheels[left + 1].onGround) {
            springForce[left + 1] -= barForce;
        }
    };
    antiRoll(FrontLeft, m_spec.front, m_spec.frontTrack);
    antiRoll(RearLeft, m_spec.rear, m_spec.rearTrack);

    for (int w = 0; w < kWheels; ++w) {
        WheelState& wheel = m_wheels[w];
        if (!wheel.onGround) {
            continue;
        }
        const float f = std::max(0.0F, springForce[w]);
        const glm::vec3 applied = up * f;
        const glm::vec3 at = wheel.ground.point;
        force += applied;
        torque += glm::cross(at - m_cgPosition, applied);
        wheel.load = f * std::max(0.0F, glm::dot(up, wheel.ground.normal));
    }
}

float CarPhysics::brakeTorque(int wheel, const CarControls& controls) const
{
    // Capacity split front to rear by the static weight shift under hard
    // braking, adjusted by the bias slider.
    const float averageRadius = 0.5F * (m_spec.frontTires.radius() + m_spec.rearTires.radius());
    const float total = kBrakeCapacityG * m_mass * kGravity * averageRadius * m_spec.brakeFrictionScale
        * (0.5F + m_spec.brakeTorqueSlider);
    const float idealFront = std::clamp(m_spec.frontWeight + m_spec.cgHeight / m_spec.wheelbase * 0.9F, 0.4F, 0.8F);
    const float frontShare = std::clamp(idealFront + (m_spec.brakeBias - 0.5F) * 0.4F, 0.2F, 0.9F);
    const bool front = wheel < 2;
    float brake = controls.brake * total * (front ? frontShare : 1.0F - frontShare) * 0.5F;
    if (!front) {
        brake += controls.handbrake * total * 0.5F * m_spec.handbrakeScale;
    }
    // ABS eases a wheel's brake as it passes its peak slip.
    const WheelState& state = m_wheels[static_cast<std::size_t>(wheel)];
    if (m_assists.abs && controls.handbrake < 0.1F && state.slipRatio < -m_peakSlipRatio[wheel] * 1.2F) {
        brake *= 0.3F;
    }
    return brake;
}

void CarPhysics::tyreForces(int w, float spin, float& fx, float& fy, float& slipRatio, float& magnitude) const
{
    const WheelState& wheel = m_wheels[static_cast<std::size_t>(w)];
    const Contact& c = m_contacts[static_cast<std::size_t>(w)];
    const float radius = wheelRadius(w);
    const float denominator = std::max(std::abs(c.vx), 1.0F);
    slipRatio = (spin * radius - c.vx) / denominator;
    const float sx = slipRatio / m_peakSlipRatio[w];
    const float sy = wheel.relaxedSlipAngle * kRadToDeg / m_peakSlipAngle[w];
    magnitude = std::sqrt(sx * sx + sy * sy);
    if (magnitude < 1e-6F) {
        fx = 0.0F;
        fy = 0.0F;
        return;
    }
    // Combined slip: the resultant slip, measured against each direction's
    // peak, picks a point on both friction curves, and the force splits
    // along the slip direction.
    const TireCompound& compound = m_spec.compound;
    const float muX = compound.longitudinal.friction(magnitude * m_peakSlipRatio[w], wheel.load) * c.grip;
    const float muY = compound.lateral.friction(magnitude * m_peakSlipAngle[w], wheel.load) * c.grip
        * kLateralGripCalibration;
    fx = sx / magnitude * muX * wheel.load;
    fy = -sy / magnitude * muY * wheel.load;
}

void CarPhysics::prepareTyres(float dt)
{
    const glm::vec3 carUp = up();
    for (int w = 0; w < kWheels; ++w) {
        WheelState& wheel = m_wheels[w];
        Contact& c = m_contacts[w];
        c = Contact{};
        if (!wheel.onGround || wheel.load <= 0.0F) {
            continue;
        }
        c.active = true;
        const glm::vec3 normal = wheel.ground.normal;
        const glm::quat steer = glm::angleAxis(wheel.steerAngle, carUp);
        glm::vec3 heading = m_orientation * (steer * glm::vec3(0.0F, 0.0F, 1.0F));
        heading = glm::normalize(heading - normal * glm::dot(heading, normal));
        c.heading = heading;
        c.side = glm::normalize(glm::cross(normal, heading));
        const glm::vec3 contactVelocity = worldPointVelocity(wheel.ground.point);
        c.vx = glm::dot(contactVelocity, c.heading);
        c.vy = glm::dot(contactVelocity, -c.side);
        const bool front = w < 2;
        c.grip = m_spec.compound.widthScale(front ? m_spec.frontTires.widthMm : m_spec.rearTires.widthMm)
            * wheel.ground.grip;

        // The slip angle builds up over the relaxation length rather than
        // jumping, which models the carcass twisting and keeps the lateral
        // force stable at low speed.
        const float steadyAngle = std::atan2(c.vy, std::max(std::abs(c.vx), 0.5F));
        const float relaxRate = std::min(1.0F, (std::abs(c.vx) + 0.5F) * dt / kRelaxationLength);
        wheel.relaxedSlipAngle += (steadyAngle - wheel.relaxedSlipAngle) * relaxRate;

        float fy = 0.0F;
        float slip = 0.0F;
        float magnitude = 0.0F;
        tyreForces(w, wheel.spin, c.fx0, fy, slip, magnitude);
        constexpr float kProbe = 0.05F;
        float fxProbe = 0.0F;
        tyreForces(w, wheel.spin + kProbe, fxProbe, fy, slip, magnitude);
        // How fast the tyre's grip pushes back on wheel spin, used to
        // integrate the spin implicitly; the tyre is too stiff for an
        // explicit step at low speed. Past the peak the local slope flattens
        // and a step would overshoot to the far side of free rolling, so the
        // chord back to free rolling (where the force is zero) is used when
        // it is steeper.
        float slope = std::max(0.0F, (fxProbe - c.fx0) / kProbe);
        const float rollingSpin = c.vx / wheelRadius(w);
        const float fromRolling = wheel.spin - rollingSpin;
        if (std::abs(fromRolling) > kProbe) {
            slope = std::max(slope, c.fx0 / fromRolling);
        }
        c.slope = slope * wheelRadius(w);
    }
}

float CarPhysics::effectiveWheelInertia(int w, float dt) const
{
    return m_wheelInertia[w] + dt * m_contacts[static_cast<std::size_t>(w)].slope;
}

void CarPhysics::solveDrivetrain(float throttle, const CarControls& controls, float dt)
{
    for (WheelState& wheel : m_wheels) {
        wheel.driveTorque = 0.0F;
    }
    const float ratio = currentRatio();
    const float rpm = engineRpm();
    float engine = engineTorque(rpm, rpm >= m_spec.redlineRpm ? 0.0F : throttle);
    const float idleSpeed = m_spec.idleRpm / kRpmPerRadPerSec;
    const float launchSpeed = std::max(m_spec.launchRpm, m_spec.idleRpm * 1.5F) / kRpmPerRadPerSec;
    const float engineInertia = m_spec.engineInertia + m_spec.drivetrainInertia;
    const float coupledSpeed = drivenWheelSpeed() * ratio;

    // An automatic clutch stays fully in once the wheels turn the engine
    // above idle. Below that, at a standing start, it works like launch
    // control: the engine is held at the launch speed (Camshaft StartRPM)
    // while the clutch slips, and with traction control on the clutch passes
    // no more torque than the driven tyres can put down. It lets go entirely
    // when the car is held stopped without throttle.
    float engagement = 0.0F;
    float launchLimit = m_spec.clutchMaxTorque;
    bool launching = false;
    if (m_shiftTimer > 0.0F || m_gear == 0) {
        engagement = 0.0F;
    } else if (!m_assists.automaticGearbox) {
        engagement = 1.0F - std::clamp(controls.clutch, 0.0F, 1.0F);
    } else if (std::abs(coupledSpeed) >= idleSpeed * 1.05F) {
        engagement = 1.0F;
    } else if (controls.throttle > 0.02F) {
        launching = true;
        engagement = std::clamp((m_engineSpeed - idleSpeed) / (launchSpeed - idleSpeed), 0.0F, 1.0F);
        if (m_assists.tractionControl && ratio != 0.0F) {
            float tyreTorque = 0.0F;
            for (int w = 0; w < kWheels; ++w) {
                if (isDriven(w) && m_contacts[w].active) {
                    const float mu = m_spec.compound.longitudinal.friction(m_peakSlipRatio[w], m_wheels[w].load)
                        * m_contacts[w].grip;
                    tyreTorque += mu * m_wheels[w].load * wheelRadius(w);
                }
            }
            launchLimit = std::min(launchLimit, tyreTorque / std::abs(ratio));
        }
    }
    m_clutchEngagement = engagement;
    if (launching && m_engineSpeed > launchSpeed) {
        const float over = (m_engineSpeed - launchSpeed) / (launchSpeed * 0.05F);
        engine = engineTorque(rpm, throttle * std::clamp(1.0F - over, 0.0F, 1.0F));
    }
    const float capacity = std::min(m_spec.clutchMaxTorque * engagement, launchLimit);

    // The driven wheels as one body seen from the engine: their effective
    // inertia and the torque the road and brakes put on them.
    float wheelInertia = 0.0F;
    float wheelLoad = 0.0F;
    int driven = 0;
    for (int w = 0; w < kWheels; ++w) {
        if (!isDriven(w)) {
            continue;
        }
        ++driven;
        const float inertia = effectiveWheelInertia(w, dt);
        wheelInertia += inertia;
        wheelLoad += m_contacts[w].fx0 * wheelRadius(w);
        const float brake = m_wheels[w].brakeTorque;
        wheelLoad += brake * smoothSign(m_wheels[w].spin, 0.5F);
    }

    // The clutch torque that makes engine and wheels turn together at the
    // end of the step, given both inertias; if it exceeds the clutch's
    // capacity the clutch slips at capacity instead.
    float clutch = 0.0F;
    bool locked = false;
    if (ratio != 0.0F && capacity > 0.0F && driven > 0) {
        const float reflected = ratio * ratio / wheelInertia;
        const float wheelAcceleration = -ratio * wheelLoad / wheelInertia;
        const float numerator = m_engineSpeed - coupledSpeed + dt * (engine / engineInertia - wheelAcceleration);
        const float needed = numerator / (dt * (1.0F / engineInertia + reflected));
        clutch = std::clamp(needed, -capacity, capacity);
        locked = std::abs(needed) <= capacity;
    }
    m_clutchLocked = locked;
    m_engineSpeed += dt * (engine - clutch) / engineInertia;

    const float wheelTorque = clutch * ratio;
    const auto splitAxle = [&](int left, float torque, const Differential& diff) {
        // Limited-slip: the faster wheel hands torque to the slower one, up
        // to the locking torque the table gives for driving or coasting.
        // How the game shapes the lock against speed difference is not
        // known; it is taken as full lock beyond 2 rad/s of difference.
        const float lock = torque >= 0.0F ? diff.lockAccel : diff.lockDecel;
        const float difference = m_wheels[left].spin - m_wheels[left + 1].spin;
        const float transfer = lock * smoothSign(difference, 2.0F);
        m_wheels[left].driveTorque += 0.5F * (torque - transfer);
        m_wheels[left + 1].driveTorque += 0.5F * (torque + transfer);
    };
    switch (m_spec.drive) {
    case DriveType::Fwd:
        splitAxle(FrontLeft, wheelTorque, m_spec.frontDiff);
        break;
    case DriveType::Rwd:
        splitAxle(RearLeft, wheelTorque, m_spec.rearDiff);
        break;
    case DriveType::Awd: {
        const float rearShare = m_spec.rearTorqueSplit;
        const float frontSpeed = 0.5F * (m_wheels[FrontLeft].spin + m_wheels[FrontRight].spin);
        const float rearSpeed = 0.5F * (m_wheels[RearLeft].spin + m_wheels[RearRight].spin);
        const float centreLock = wheelTorque >= 0.0F ? m_spec.centreDiff.lockAccel : m_spec.centreDiff.lockDecel;
        const float transfer = centreLock * smoothSign(frontSpeed - rearSpeed, 2.0F);
        splitAxle(FrontLeft, wheelTorque * (1.0F - rearShare) - 0.5F * transfer, m_spec.frontDiff);
        splitAxle(RearLeft, wheelTorque * rearShare + 0.5F * transfer, m_spec.rearDiff);
        break;
    }
    }
}

void CarPhysics::integrateWheels(float dt, glm::vec3& force, glm::vec3& torque)
{
    const float speed = glm::length(m_velocity);
    for (int w = 0; w < kWheels; ++w) {
        WheelState& wheel = m_wheels[w];
        const Contact& c = m_contacts[w];
        const float inertia = effectiveWheelInertia(w, dt);
        float spin = wheel.spin + dt * (wheel.driveTorque - c.fx0 * wheelRadius(w)) / inertia;
        const float brakeStep = dt * wheel.brakeTorque / inertia;
        spin = std::abs(spin) <= brakeStep ? 0.0F : spin - std::copysign(brakeStep, spin);
        wheel.spin = spin;
        wheel.spinAngle = std::fmod(wheel.spinAngle + wheel.spin * dt, 2.0F * kPi);

        if (!c.active) {
            wheel.longitudinalForce = 0.0F;
            wheel.lateralForce = 0.0F;
            wheel.slipMagnitude = 0.0F;
            continue;
        }
        float fx = 0.0F;
        float fy = 0.0F;
        float slipRatio = 0.0F;
        float magnitude = 0.0F;
        tyreForces(w, wheel.spin, fx, fy, slipRatio, magnitude);

        // At walking pace slip is meaningless: hold the car with a damped
        // friction force instead, within the same friction limit.
        const float load = wheel.load;
        if (speed < kLowSpeed) {
            const float blend = 1.0F - speed / kLowSpeed;
            const float limit
                = m_spec.compound.lateral.friction(m_peakSlipAngle[w], load) * c.grip * kLateralGripCalibration * load;
            const float lateralHold = std::clamp(-c.vy * load / 0.2F, -limit, limit);
            fy = fy * (1.0F - blend) + lateralHold * blend;
            if (std::abs(wheel.spin) < 0.5F && wheel.brakeTorque > 0.0F) {
                const float longitudinalHold = std::clamp(-c.vx * load / 0.2F, -limit, limit);
                fx = fx * (1.0F - blend) + longitudinalHold * blend;
            }
        }
        const float rolling = -smoothSign(c.vx, 0.5F) * load * (kRollingResistance + wheel.ground.drag);

        wheel.slipRatio = slipRatio;
        wheel.slipAngle = wheel.relaxedSlipAngle;
        wheel.slipMagnitude = magnitude;
        wheel.longitudinalForce = fx;
        wheel.lateralForce = fy;

        const glm::vec3 applied = c.heading * (fx + rolling) + c.side * -fy;
        force += applied;
        torque += glm::cross(wheel.ground.point - m_cgPosition, applied);
    }
    // A locked clutch turns the engine with the wheels; matching the speeds
    // exactly stops the two drifting apart through rounding.
    if (m_clutchLocked) {
        m_engineSpeed = drivenWheelSpeed() * currentRatio();
    }
    const float idleSpeed = m_spec.idleRpm / kRpmPerRadPerSec;
    m_engineSpeed = std::clamp(m_engineSpeed, idleSpeed * 0.5F, m_spec.redlineRpm * 1.05F / kRpmPerRadPerSec);
}

void CarPhysics::computeAero(glm::vec3& force, glm::vec3& torque) const
{
    const float speed = glm::length(m_velocity);
    if (speed < 0.1F) {
        return;
    }
    const float q = (speed / kAeroReferenceSpeed) * (speed / kAeroReferenceSpeed);
    const glm::vec3 drag = -m_velocity / speed * (m_spec.dragAt100MphLbf * kLbfToNewton * m_spec.gameDragScale * q);
    force += drag;

    // Downforce only builds with forward flow over the body.
    const float forwardFlow = std::max(0.0F, forwardSpeed());
    const float qForward = (forwardFlow / kAeroReferenceSpeed) * (forwardFlow / kAeroReferenceSpeed);
    const glm::vec3 down = -up();
    const glm::vec3 frontPoint = position() + m_orientation * glm::vec3(0.0F, 0.0F, m_spec.wheelbase * 0.5F);
    const glm::vec3 rearPoint = position() + m_orientation * glm::vec3(0.0F, 0.0F, -m_spec.wheelbase * 0.5F);
    const glm::vec3 frontForce = down * (m_spec.frontDownforceAt100MphLbf * kLbfToNewton * qForward);
    const glm::vec3 rearForce = down * (m_spec.rearDownforceAt100MphLbf * kLbfToNewton * qForward);
    force += frontForce + rearForce;
    torque += glm::cross(frontPoint - m_cgPosition, frontForce) + glm::cross(rearPoint - m_cgPosition, rearForce);
}

void CarPhysics::step(const CarControls& input, const GroundQuery& ground)
{
    constexpr float dt = 1.0F / kStepRate;
    CarControls controls = input;
    controls.throttle = std::clamp(controls.throttle, 0.0F, 1.0F);
    controls.brake = std::clamp(controls.brake, 0.0F, 1.0F);
    controls.handbrake = std::clamp(controls.handbrake, 0.0F, 1.0F);
    // In reverse the pedals swap roles, so the brake trigger backs the car
    // up as it does in the game.
    if (m_gear < 0 && m_assists.automaticGearbox) {
        std::swap(controls.throttle, controls.brake);
    }

    updateSteering(controls, dt);
    updateGearbox(input, dt);

    // Traction control trims the throttle while a driven wheel spins past
    // its peak slip; stability control while the car yaws faster than its
    // steering asks for.
    float throttle = controls.throttle;
    if (m_assists.tractionControl) {
        float excess = 0.0F;
        for (int w = 0; w < kWheels; ++w) {
            if (isDriven(w) && m_wheels[w].onGround) {
                // The database's TCS slip settings hold the same values for
                // every car, so they say nothing about a car's tyres; the
                // system holds each driven tyre at its own peak slip.
                const float allowed = m_peakSlipRatio[w];
                excess = std::max(excess, m_wheels[w].slipRatio * (m_gear < 0 ? -1.0F : 1.0F) - allowed);
            }
        }
        const float target = std::clamp(excess * 4.0F, 0.0F, 1.0F);
        m_tcsCut += (target - m_tcsCut) * std::min(1.0F, dt * (target > m_tcsCut ? 200.0F : 8.0F));
        throttle *= 1.0F - m_tcsCut;
    }
    if (m_assists.stabilityControl) {
        const float speed = forwardSpeed();
        if (speed > 8.0F) {
            const float expected = speed * std::tan(m_steerAngle) / m_spec.wheelbase;
            const float actual = glm::dot(m_angularVelocity, up());
            const float excess = std::abs(actual) - std::abs(expected) - 0.25F;
            if (excess > 0.0F) {
                throttle *= std::max(0.2F, 1.0F - excess * 1.5F);
            }
        }
    }
    if (m_shiftTimer > 0.0F) {
        throttle = 0.0F;
    }
    m_throttleApplied = throttle;

    glm::vec3 force(0.0F, -m_mass * kGravity, 0.0F);
    glm::vec3 torque(0.0F);
    computeSuspension(ground, dt, force, torque);
    for (int w = 0; w < kWheels; ++w) {
        m_wheels[w].brakeTorque = brakeTorque(w, controls);
    }
    prepareTyres(dt);
    solveDrivetrain(throttle, controls, dt);
    integrateWheels(dt, force, torque);
    computeAero(force, torque);

    m_velocity += force / m_mass * dt;
    const glm::mat3 r = glm::mat3_cast(m_orientation);
    const glm::vec3 localOmega = glm::transpose(r) * m_angularVelocity;
    const glm::vec3 localMomentum = m_inertiaLocal * localOmega;
    const glm::vec3 gyroscopic = r * glm::cross(localOmega, localMomentum);
    m_angularVelocity += worldInverseInertia() * (torque - gyroscopic) * dt;

    m_cgPosition += m_velocity * dt;
    const glm::quat spin(0.0F, m_angularVelocity.x, m_angularVelocity.y, m_angularVelocity.z);
    m_orientation = glm::normalize(m_orientation + spin * m_orientation * (0.5F * dt));
}

} // namespace fh
