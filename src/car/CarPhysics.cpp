#include "CarPhysics.h"


#include <algorithm>
#include <cmath>

namespace openhorizon {

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
    m_wheelBrake.fill(0.0F);
    m_absTimer.fill(0.0F);
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

float CarPhysics::resolveWorldContacts(std::span<const WorldContact> contacts, const CollisionSettings& settings)
{
    // After the game's response to a car's contacts with the static world
    // (0x82DD2750, called for the car through 0x82DD5C20 from 0x82DF8D50).
    // Verified there: one pass over the contacts in order, each seeing the
    // velocities the ones before it left; a normal impulse that only stops
    // the approach (its bounce term multiplies by a zero register, so the
    // surface pair's Elasticity does not enter); Coulomb friction against
    // the sliding velocity from before that contact's normal impulse, up to
    // the pair's friction times the normal impulse; both impulses turning
    // the car by only a share, the torque scale, which also enters their
    // effective masses; and the overlap removed by moving the car, not by
    // velocity, along each normal as far as the moves before it have not
    // already covered. Inferred: the vector argument it compares with each
    // contact's depth less its approach speed is the step's length; for the
    // overlapping contacts given here that test always passes.
    // Missing: a flag in the contact's packed normal that gives a contact
    // the full turning share (what sets it has not been traced), and the
    // per-surface CarWorldCollFricMods, which only differ from 1 for a car
    // tipped past 30 degrees and whose use has not been traced.
    const float torqueScale = settings.carWorldTorqueScaleAt(speed());
    const float frictionBlend = settings.frictionBlendAt(speed());
    const glm::mat3 inverseInertia = worldInverseInertia();
    const auto response = [&](const glm::vec3& r, const glm::vec3& direction) {
        const glm::vec3 angular = glm::cross(inverseInertia * glm::cross(r, direction), r);
        return 1.0F / m_mass + torqueScale * glm::dot(direction, angular);
    };
    // The game's threshold below which a contact is not sliding: 13 << 26,
    // FLT_EPSILON, built with vspltisw and vslw at 0x82DD28F8.
    constexpr float kSlidingEpsilon = 1.1920929e-7F;
    float largest = 0.0F;
    glm::vec3 pushOut(0.0F);
    for (const WorldContact& contact : contacts) {
        const glm::vec3 r = contact.point - m_cgPosition;
        const glm::vec3 velocity = worldPointVelocity(contact.point);
        const float approach = glm::dot(velocity, contact.normal);
        const float normalImpulse = std::max(0.0F, -approach / response(r, contact.normal));
        applyImpulse(contact.normal * normalImpulse, contact.point, torqueScale);

        const glm::vec3 sliding = velocity - contact.normal * approach;
        const float slide = glm::length(sliding);
        if (slide >= kSlidingEpsilon) {
            const glm::vec3 tangent = -sliding / slide;
            const float friction
                = contact.lowSpeedFriction + frictionBlend * (contact.friction - contact.lowSpeedFriction);
            const float frictionImpulse = std::min(slide / response(r, tangent), friction * normalImpulse);
            applyImpulse(tangent * frictionImpulse, contact.point, torqueScale);
        }

        const float uncovered = contact.depth - glm::dot(pushOut, contact.normal);
        if (uncovered >= 0.0F) {
            pushOut += contact.normal * uncovered;
        }
        largest = std::max(largest, normalImpulse);
    }
    m_cgPosition += pushOut;
    return largest;
}

namespace {

/// Appends where `spheres` of `car` overlap the bounding box of `boxCar`,
/// as contacts on the box: the normal out of the box towards the sphere.
void spheresAgainstBox(const CarPhysics& car, std::span<const CarSpec::CollisionSphere> spheres,
    const CarPhysics& boxCar, std::vector<CarContact>& out)
{
    const glm::mat4 boxToWorld = boxCar.transform();
    const glm::mat4 toBox = glm::inverse(boxToWorld) * car.transform();
    const CarSpec& spec = boxCar.spec();
    const glm::vec3 lo(spec.boundsMin[0], spec.boundsMin[1], spec.boundsMin[2]);
    const glm::vec3 hi(spec.boundsMax[0], spec.boundsMax[1], spec.boundsMax[2]);
    for (const CarSpec::CollisionSphere& sphere : spheres) {
        const glm::vec3 centre(toBox * glm::vec4(sphere.centre[0], sphere.centre[1], sphere.centre[2], 1.0F));
        glm::vec3 surface = glm::clamp(centre, lo, hi);
        glm::vec3 normal = centre - surface;
        float depth = 0.0F;
        const float outside = glm::length(normal);
        if (outside > 1e-5F) {
            if (outside >= sphere.radius) {
                continue;
            }
            normal /= outside;
            depth = sphere.radius - outside;
        } else {
            // The centre is inside: out through the nearest face.
            const glm::vec3 below = centre - lo;
            const glm::vec3 above = hi - centre;
            int axis = 0;
            float nearest = std::min(below.x, above.x);
            for (int i = 1; i < 3; ++i) {
                const float distance = std::min(below[i], above[i]);
                if (distance < nearest) {
                    nearest = distance;
                    axis = i;
                }
            }
            normal = glm::vec3(0.0F);
            normal[axis] = below[axis] < above[axis] ? -1.0F : 1.0F;
            surface[axis] = below[axis] < above[axis] ? lo[axis] : hi[axis];
            depth = sphere.radius + nearest;
        }
        out.push_back({glm::vec3(boxToWorld * glm::vec4(surface, 1.0F)),
            glm::normalize(glm::vec3(boxToWorld * glm::vec4(normal, 0.0F))), depth});
    }
}

} // namespace

void findCarContacts(const CarPhysics& a, std::span<const CarSpec::CollisionSphere> spheresA, bool boxA,
    const CarPhysics& b, std::span<const CarSpec::CollisionSphere> spheresB, bool boxB, std::vector<CarContact>& out)
{
    if (boxB) {
        spheresAgainstBox(a, spheresA, b, out);
        return;
    }
    if (boxA) {
        // Found on the first car's box, then turned round to be on the
        // second's spheres: the point moves to the sphere's deepest point.
        const std::size_t first = out.size();
        spheresAgainstBox(b, spheresB, a, out);
        for (std::size_t i = first; i < out.size(); ++i) {
            CarContact& contact = out[i];
            contact.normal = -contact.normal;
            contact.point += contact.normal * contact.depth;
        }
        return;
    }
    const glm::mat4 ta = a.transform();
    const glm::mat4 tb = b.transform();
    for (const CarSpec::CollisionSphere& sa : spheresA) {
        const glm::vec3 ca(ta * glm::vec4(sa.centre[0], sa.centre[1], sa.centre[2], 1.0F));
        for (const CarSpec::CollisionSphere& sb : spheresB) {
            const glm::vec3 cb(tb * glm::vec4(sb.centre[0], sb.centre[1], sb.centre[2], 1.0F));
            const glm::vec3 offset = ca - cb;
            const float distance = glm::length(offset);
            const float reach = sa.radius + sb.radius;
            if (distance >= reach || distance < 1e-4F) {
                continue;
            }
            const glm::vec3 normal = offset / distance;
            out.push_back({cb + normal * sb.radius, normal, reach - distance});
        }
    }
}

float resolveCarContacts(
    CarPhysics& a, CarPhysics& b, std::span<const CarContact> contacts, float turnA, float turnB, float restitution)
{
    // After 0x82E04A60, which the physics world runs for each pair of
    // bodies in contact (0x82E054F0). Verified there: one pass over the
    // contacts in order; the first body's contact point lies `depth` back
    // along the normal from the second's; an impulse only while the depth
    // less the approach over the step is not negative, of -(1 + e) times
    // the approach over both bodies' effective masses, whose rotational
    // parts are scaled by each car's torque scale (two virtuals on the car
    // dynamics, 0x82E04AC0 on); no friction; and the overlap removed by
    // moving both bodies along each normal as far as the moves before it
    // have not covered, each by the other's share of their summed mass
    // (+0x160 is the mass, +0x164 its inverse, set together at 0x82E0B3D4).
    // Missing: a per-direction test on each body (virtual +0x4C) that
    // treats it as immovable; what makes it true has not been traced.
    constexpr float kStep = 1.0F / CarPhysics::kStepRate;
    const float shareA = b.mass() / (a.mass() + b.mass());
    glm::vec3 moveA(0.0F);
    glm::vec3 moveB(0.0F);
    float largest = 0.0F;
    for (const CarContact& contact : contacts) {
        const glm::vec3 pointA = contact.point - contact.normal * contact.depth;
        const float approach
            = glm::dot(a.worldPointVelocity(pointA) - b.worldPointVelocity(contact.point), contact.normal);
        if (contact.depth - approach * kStep >= 0.0F) {
            const float response = a.impulseResponse(pointA, contact.normal, turnA)
                + b.impulseResponse(contact.point, contact.normal, turnB);
            const float impulse = std::max(0.0F, -(1.0F + restitution) * approach / response);
            a.applyImpulse(contact.normal * impulse, pointA, turnA);
            b.applyImpulse(-contact.normal * impulse, contact.point, turnB);
            largest = std::max(largest, impulse);
        }
        const float uncovered = glm::dot((contact.point + moveB) - (pointA + moveA), contact.normal);
        if (uncovered >= 0.0F) {
            moveA += contact.normal * (uncovered * shareA);
            moveB -= contact.normal * (uncovered * (1.0F - shareA));
        }
    }
    a.translate(moveA);
    b.translate(moveB);
    return largest;
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
    // The game's automatic gearbox holds its gear while any driven wheel's
    // normalised slip is past 1.1 (0x82DF3BC8), so wheelspin does not
    // shift it up.
    constexpr float kUpshiftSlipLimit = 1.1F;
    bool spinning = false;
    for (int w = 0; w < kWheels; ++w) {
        spinning = spinning || (isDriven(w) && normalisedSlipRatio(w) > kUpshiftSlipLimit);
    }
    if (m_gear < topGear && rpm > m_spec.redlineRpm * 0.97F && coupledRpm > m_spec.redlineRpm * 0.9F && grounded
        && !spinning) {
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
    float brake = m_wheelBrake[static_cast<std::size_t>(wheel)] * total * (front ? frontShare : 1.0F - frontShare) * 0.5F;
    if (!front) {
        brake += controls.handbrake * total * 0.5F * m_spec.handbrakeScale;
    }
    return brake;
}

float CarPhysics::normalisedSlipRatio(int wheel) const
{
    // The assists and the gearbox read one per-wheel slip (+0x1F0 of the
    // wheel). Its writer has not been traced; that it is the slip ratio over
    // the tyre's peak is inferred from what it is compared with: 1 for ABS
    // without ABS (NoABSSlipDef*NoSteer), up to 3 on both sides of zero with
    // it (ReleasePointABSFrontNoSteer, beyond what a braking slip ratio can
    // reach), and 1.1 for the gearbox's wheelspin test.
    const std::size_t w = static_cast<std::size_t>(wheel);
    return m_wheels[w].slipRatio / m_peakSlipRatio[w];
}

float CarPhysics::applyAssists(const CarControls& controls, float dt)
{
    // 0x82DF8630 starts each wheel from the brake pedal and runs stability
    // management (0x82DEEF80), then ABS, then traction control, all on the
    // tyre state the previous step left.
    for (int w = 0; w < kWheels; ++w) {
        m_wheelBrake[static_cast<std::size_t>(w)] = controls.brake;
        m_absTimer[static_cast<std::size_t>(w)] += dt;
    }
    m_assistActivity.stabilityControl = m_assists.stabilityControl && applyStabilityManagement();
    m_assistActivity.abs = applyAbs(controls);
    m_tcsCut = m_assists.tractionControl ? tractionControlCut() : 0.0F;
    m_assistActivity.tractionControl = m_tcsCut > 0.0F && controls.throttle > 0.0F;
    return controls.throttle * (1.0F - m_tcsCut);
}

bool CarPhysics::applyStabilityManagement()
{
    // 0x82DEEF80: above StabilityManageSpeed while rolling forwards, the
    // rear tyres' average normalised slip angle past STMSlipMin brakes one
    // diagonal pair of wheels, fully at STMSlipMax, and eases the other pair
    // by half as much. The surface's TCandStabilitySpeedMultiplier (1 on
    // tarmac) scales the speed; surfaces are not told apart here.
    const float threshold = m_spec.assistSettings.stabilityManageSpeedMph * kMphToMs;
    if (glm::dot(m_velocity, m_velocity) < threshold * threshold || forwardSpeed() < 0.0F) {
        return false;
    }
    const float rearSlip = 0.5F * (normalisedSlip(RearLeft) + normalisedSlip(RearRight));
    const float magnitude = std::abs(rearSlip);
    const float lower = std::min(m_spec.stmSlipMin, m_spec.stmSlipMax);
    const float upper = std::max(m_spec.stmSlipMin, m_spec.stmSlipMax);
    if (magnitude < m_spec.stmSlipMin || upper <= lower) {
        return false;
    }
    const float amount = std::clamp((magnitude - lower) / (upper - lower), 0.0F, 1.0F);
    // The game brakes its wheels 0 and 3 when the average is positive and 1
    // and 2 otherwise (0x82DE2F58). Which of its wheels is left is not
    // traced; the pair is taken so that the outer front wheel brakes, the
    // one that turns the car out of the slide: a positive slip here is the
    // rear pushing the car right, which a right turn gives.
    const bool rightTurn = rearSlip > 0.0F;
    const std::array<int, 2> braked
        = rightTurn ? std::array<int, 2>{FrontLeft, RearRight} : std::array<int, 2>{FrontRight, RearLeft};
    const std::array<int, 2> eased
        = rightTurn ? std::array<int, 2>{FrontRight, RearLeft} : std::array<int, 2>{FrontLeft, RearRight};
    for (const int w : braked) {
        float& brake = m_wheelBrake[static_cast<std::size_t>(w)];
        brake = std::min(brake + amount, 1.0F);
    }
    for (const int w : eased) {
        float& brake = m_wheelBrake[static_cast<std::size_t>(w)];
        brake = std::max(brake - amount * 0.5F, 0.0F);
    }
    return amount > 0.0F;
}

bool CarPhysics::applyAbs(const CarControls& controls)
{
    // 0x82DF8630: with ABS on, or with it off and the pedal at
    // NoABSBrakeTravel, a braked wheel whose normalised slip passes the
    // release point against its direction of roll lets go of its brake for
    // the release duration. The release points blend from the no-steer to
    // the full-steer value by the steering input.
    const AssistSettings& settings = m_spec.assistSettings;
    if (!m_assists.abs && controls.brake < settings.noAbsBrakeTravel) {
        return false;
    }
    const float steer = std::min(std::abs(controls.steer), 1.0F);
    const auto blend = [steer](float noSteer, float fullSteer) { return noSteer + (fullSteer - noSteer) * steer; };
    float front = 0.0F;
    float rear = 0.0F;
    float duration = 0.0F;
    if (m_assists.abs) {
        front = blend(m_spec.absSlipFrontNoSteer, m_spec.absSlipFrontFullSteer);
        rear = blend(m_spec.absSlipRearNoSteer, m_spec.absSlipRearFullSteer);
        duration = m_spec.absDuration;
    } else {
        front = blend(settings.noAbsSlipFrontNoSteer, settings.noAbsSlipFrontFullSteer);
        rear = blend(settings.noAbsSlipRearNoSteer, settings.noAbsSlipRearFullSteer);
        duration = settings.noAbsDisableTime;
    }
    bool released = false;
    for (int w = 0; w < kWheels; ++w) {
        const auto index = static_cast<std::size_t>(w);
        float& brake = m_wheelBrake[index];
        float& timer = m_absTimer[index];
        if (timer < duration) {
            released = released || brake > 0.0F;
            brake = 0.0F;
            continue;
        }
        if (brake <= 0.0F) {
            continue;
        }
        const float release = w < 2 ? front : rear;
        const float slip = normalisedSlipRatio(w);
        const float roll = m_wheels[index].spin;
        if ((slip > release && roll <= 0.0F) || (slip < -release && roll >= 0.0F)) {
            brake = 0.0F;
            timer = 0.0F;
            released = true;
        } else if (!m_wheels[index].onGround) {
            // A wheel in the air restarts the release time, so its brake
            // lets go from the next step. On this step the game also caps
            // the brake by a ratio of wheel fields (+0x37C, +0x384, +0x340
            // and the mass) that have not been identified; the brake is kept.
            timer = 0.0F;
        }
    }
    return released;
}

float CarPhysics::tractionControlCut() const
{
    // 0x82DF8630: the driven wheels' average forward normalised slip against
    // the allowed slip (AssistsTCSSlipDefTakeoff below TractionControlSpeed,
    // AssistsTCSSlipDefMoving above it, held within the CarTuning range),
    // raised towards TCSFullSteerSlipScale as the driven tyres near their
    // peak slip angle. The cut grows from nothing at the allowed slip to a
    // closed throttle TCSFullEffectFricDiff beyond it. The surface's TCS
    // multipliers (1 on tarmac) are not applied, as surfaces are not told
    // apart here.
    const AssistSettings& settings = m_spec.assistSettings;
    // The game takes the sign from the gear index at +0x1390 (the gearbox at
    // 0x82DF3B90 indexes its shift table by it): negative for index 0 only.
    // That index 0 is reverse is inferred.
    const float direction = m_gear < 0 ? -1.0F : 1.0F;
    const glm::vec3 f = forward();
    const float along = forwardSpeed() * direction;
    // Speed against the direction of travel does not count.
    const glm::vec3 velocity = m_velocity + f * (along < 0.0F ? -along * direction : 0.0F);
    const float threshold = settings.tractionControlSpeedMph * kMphToMs;
    const float lowerLimit = std::min(settings.tuneTcsSlipDefMin, settings.tuneTcsSlipDefMax);
    const float upperLimit = std::max(settings.tuneTcsSlipDefMin, settings.tuneTcsSlipDefMax);
    const float allowedSlip
        = std::clamp(glm::dot(velocity, velocity) > threshold * threshold ? m_spec.tcsSlipMoving : m_spec.tcsSlipTakeoff,
            lowerLimit, upperLimit);

    float slip = 0.0F;
    float slipAngle = 0.0F;
    int driven = 0;
    for (int w = 0; w < kWheels; ++w) {
        if (isDriven(w)) {
            slip += std::max(0.0F, normalisedSlipRatio(w) * direction);
            slipAngle += normalisedSlip(w);
            ++driven;
        }
    }
    if (driven == 0) {
        return 0.0F;
    }
    slip /= static_cast<float>(driven);
    slipAngle /= static_cast<float>(driven);
    float fullSteerScale = settings.tcsFullSteerSlipScaleAwd;
    if (m_spec.drive == DriveType::Rwd) {
        fullSteerScale = settings.tcsFullSteerSlipScaleRwd;
    } else if (m_spec.drive == DriveType::Fwd) {
        fullSteerScale = settings.tcsFullSteerSlipScaleFwd;
    }
    const float steerScale = 1.0F + (fullSteerScale - 1.0F) * std::min(std::abs(slipAngle), 1.0F);
    const float start = allowedSlip * steerScale;
    const float full = start + settings.tcsFullEffectFricDiff;
    const float lower = std::min(start, full);
    const float upper = std::max(start, full);
    if (slip >= upper) {
        return 1.0F;
    }
    if (slip <= lower) {
        return 0.0F;
    }
    return (slip - lower) / (upper - lower);
}

glm::vec3 CarPhysics::modifyFrictionTorque(const glm::vec3& tyreTorque) const
{
    // 0x82DEFF88, run on the summed torque of the tyre forces before the
    // body integrates it. The settings block is WithAssists when traction
    // control or stability management is on and NoAssists otherwise (the
    // game's simulation steering has a block of its own, not modelled
    // here). Its per-drive-layout modifiers are always the AWD ones, as
    // 0x82DE3AC0 falls through its cases, and those are all 1 on the disc;
    // the off-road modifiers belong to a car state (+0x5071 == 4) not
    // modelled here.
    const AssistSettings& settings = m_spec.assistSettings;
    const bool assisted = m_assists.tractionControl || m_assists.stabilityControl;
    const std::array<FrictionTorqueMod, 2>& blocks
        = assisted ? settings.frictionTorqueWithAssists : settings.frictionTorqueNoAssists;

    // The two blocks blend by the car's mass. The game compares its own
    // mass with the Mass0KG and Mass1KG keys times 0.01, which is taken to
    // mean it keeps the mass in 100 kg.
    const float lightMass = std::min(settings.frictionTorqueMass0Kg, settings.frictionTorqueMass1Kg);
    const float heavyMass = std::max(settings.frictionTorqueMass0Kg, settings.frictionTorqueMass1Kg);
    float massBlend = 0.0F;
    if (m_mass >= heavyMass) {
        massBlend = 1.0F;
    } else if (m_mass > lightMass) {
        massBlend = (m_mass - lightMass) / (heavyMass - lightMass);
    }
    const auto byMass = [&](float FrictionTorqueMod::* field) {
        return blocks[0].*field + (blocks[1].*field - blocks[0].*field) * massBlend;
    };

    // How far the velocity points away from the heading, as the cosine of
    // the angle in the ground plane of the car.
    const glm::vec3 up = this->up();
    glm::vec3 flat = m_velocity - up * glm::dot(m_velocity, up);
    const float flatLength = glm::length(flat);
    if (flatLength > 0.0F) {
        flat /= flatLength;
    }
    const float cosine = glm::dot(forward(), flat);
    const float cos0 = byMass(&FrictionTorqueMod::cosYawAngle0);
    const float cos1 = byMass(&FrictionTorqueMod::cosYawAngle1);
    const float cosLow = std::min(cos0, cos1);
    const float cosHigh = std::max(cos0, cos1);
    float angleBlend = 1.0F;
    if (cosine >= cosHigh) {
        angleBlend = 0.0F;
    } else if (cosine > cosLow) {
        angleBlend = 1.0F - (cosine - cosLow) / (cosHigh - cosLow);
    }

    const float yawTorque = glm::dot(tyreTorque, up);
    const bool turning = glm::dot(m_angularVelocity, up) * yawTorque > 0.0F;
    const auto byAngle = [&](float FrictionTorqueMod::* at0, float FrictionTorqueMod::* at1) {
        const float value0 = byMass(at0);
        return value0 + (byMass(at1) - value0) * angleBlend;
    };
    const float scale = turning ? byAngle(&FrictionTorqueMod::turnScale0, &FrictionTorqueMod::turnScale1)
                                : byAngle(&FrictionTorqueMod::straightenScale0, &FrictionTorqueMod::straightenScale1);
    const float clampDeg = turning ? byAngle(&FrictionTorqueMod::turnClamp0, &FrictionTorqueMod::turnClamp1)
                                   : byAngle(&FrictionTorqueMod::straightenClamp0, &FrictionTorqueMod::straightenClamp1);

    // Both take full effect from 15 m/s and none below 8 m/s, where the
    // clamp is ten times as wide (squared speeds 64 and 225 in the code).
    constexpr float kSlowSpeedSquared = 64.0F;
    constexpr float kFastSpeedSquared = 225.0F;
    constexpr float kSlowClampFactor = 10.0F;
    const float speedBlend = std::clamp(
        (glm::dot(m_velocity, m_velocity) - kSlowSpeedSquared) / (kFastSpeedSquared - kSlowSpeedSquared), 0.0F, 1.0F);
    const float effectiveScale = 1.0F + (scale - 1.0F) * speedBlend;
    // The clamp is an angular acceleration; the game multiplies it by the
    // body's inertia about its vertical axis (+0x104 of the car).
    const float limit = clampDeg * kDegToRad * m_inertiaLocal.y
        * (kSlowClampFactor + (1.0F - kSlowClampFactor) * speedBlend);
    const float modified = std::clamp(yawTorque * effectiveScale, -limit, limit);
    return tyreTorque + up * (modified - yawTorque);
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
    // while the clutch slips. It lets go entirely when the car is held
    // stopped without throttle.
    float engagement = 0.0F;
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
    }
    m_clutchEngagement = engagement;
    if (launching && m_engineSpeed > launchSpeed) {
        const float over = (m_engineSpeed - launchSpeed) / (launchSpeed * 0.05F);
        engine = engineTorque(rpm, throttle * std::clamp(1.0F - over, 0.0F, 1.0F));
    }
    const float capacity = m_spec.clutchMaxTorque * engagement;

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
    glm::vec3 tyreTorque(0.0F);
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
        tyreTorque += glm::cross(wheel.ground.point - m_cgPosition, applied);
    }
    torque += modifyFrictionTorque(tyreTorque);
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

    float throttle = applyAssists(controls, dt);
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

} // namespace openhorizon
