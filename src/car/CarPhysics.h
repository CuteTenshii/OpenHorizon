#pragma once

#include "CarSpec.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <span>
#include <vector>

namespace fh {

/// What a suspension ray found below a wheel.
struct GroundHit {
    glm::vec3 point{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float distance = 0.0F;
    /// Surface grip multiplier (1 for tarmac) and the game's surface type.
    float grip = 1.0F;
    int surface = 0;
    /// Rolling resistance and roughness of loose surfaces, 0 on tarmac.
    float drag = 0.0F;
};

/// The world as the car's wheels see it.
class GroundQuery {
public:
    virtual ~GroundQuery() = default;
    /// Casts a ray from `origin` along the unit vector `direction`; returns
    /// true with the nearest hit within `maxDistance`.
    virtual bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, GroundHit& hit) const
        = 0;
};

/// Endless flat tarmac at height 0, for tests and the performance harness.
class FlatGround final : public GroundQuery {
public:
    bool raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, GroundHit& hit) const override;
};

struct CarControls {
    /// 0 to 1.
    float throttle = 0.0F;
    float brake = 0.0F;
    float handbrake = 0.0F;
    /// -1 (full left) to 1 (full right).
    float steer = 0.0F;
    /// Held down by the player, 0 to 1; only read with a manual clutch.
    float clutch = 0.0F;
    bool shiftUp = false;
    bool shiftDown = false;
};

/// The player's assist settings. ABS, traction control and stability
/// management follow CCarDynamics' assist step at 0x82DF8630 (see
/// CarPhysics::applyAssists).
struct DrivingAssists {
    bool abs = true;
    bool tractionControl = true;
    bool stabilityControl = true;
    bool automaticGearbox = true;
    /// The game's pad steering (CCarDynamics, 0x82DF9620): the stick asks
    /// for a share of a speed-dependent lock the tyres can use. Off, the
    /// stick maps straight onto SteerMaxAngleFiltered, which the AI drivers
    /// and the performance harness rely on.
    bool speedSensitiveSteering = true;
};

/// A point where the car's body overlaps the static world.
struct WorldContact {
    glm::vec3 point{0.0F};
    /// Out of the surface.
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    /// Penetration, metres.
    float depth = 0.0F;
    /// The surface pair's FrictionCoefficient and LowSpeedFrictionCoefficient
    /// (surfaceTypes.xml, car against the surface's category). The defaults
    /// are the friction the game uses for a pair it does not find
    /// (0x82E0B9DC).
    float friction = 0.5F;
    float lowSpeedFriction = 0.5F;
};

/// A point where two cars' bodies overlap: `point` on the second car's
/// surface, `normal` out of the second car towards the first, and `depth`
/// how far the first reaches into the second along it.
struct CarContact {
    glm::vec3 point{0.0F};
    glm::vec3 normal{0.0F, 1.0F, 0.0F};
    float depth = 0.0F;
};

/// Which assists acted on the last physics step, for the HUD's assist
/// lights: the game lights them while an assist is working, not while it
/// is merely switched on.
struct AssistActivity {
    /// A braked wheel's brake was let go of.
    bool abs = false;
    /// Traction control cut the throttle.
    bool tractionControl = false;
    /// Stability management braked a pair of wheels.
    bool stabilityControl = false;
};

/// The rigid-body simulation of one car: chassis, four suspensions and tyres,
/// engine, clutch, gearbox and differentials, driven by a CarSpec.
///
/// Car space has x to the right, y up and z forward, with its origin on the
/// ground below the middle of the wheelbase, as the car models and
/// maxdata.xml use. World space is the game's: x east, y up, z north. Both
/// are left-handed, so a positive rotation about y (as glm computes it)
/// turns +z towards +x: clockwise seen from above, a right turn.
class CarPhysics {
public:
    static constexpr int kWheels = 4;
    enum WheelIndex { FrontLeft, FrontRight, RearLeft, RearRight };
    /// The simulation rate; Forza games run their car physics at 360 Hz.
    static constexpr float kStepRate = 360.0F;

    struct WheelState {
        /// Wheel centre in world space and the ground contact, if any.
        glm::vec3 hubWorld{0.0F};
        bool onGround = false;
        GroundHit ground;
        /// Positive is compressed from ride height, metres.
        float compression = 0.0F;
        float compressionSpeed = 0.0F;
        float load = 0.0F;
        /// Angular speed (rad/s, positive rolls forward) and accumulated
        /// angle for drawing.
        float spin = 0.0F;
        float spinAngle = 0.0F;
        float steerAngle = 0.0F;
        float slipRatio = 0.0F;
        /// Radians.
        float slipAngle = 0.0F;
        /// Combined slip normalised to the tyre's peak (1 = at the limit).
        float slipMagnitude = 0.0F;
        float longitudinalForce = 0.0F;
        float lateralForce = 0.0F;
        float driveTorque = 0.0F;
        float brakeTorque = 0.0F;
        float relaxedSlipAngle = 0.0F;
    };

    explicit CarPhysics(const CarSpec& spec);

    /// Places the car at rest with its wheels on the ground at `position`
    /// (car-space origin), facing `heading` radians clockwise from +Z seen
    /// from above.
    void reset(const glm::vec3& position, float heading, const GroundQuery& ground);

    /// Advances by `dt` seconds in fixed steps; leftover time carries over.
    void update(float dt, const CarControls& controls, const GroundQuery& ground);
    /// One fixed step.
    void step(const CarControls& controls, const GroundQuery& ground);

    const CarSpec& spec() const { return m_spec; }
    DrivingAssists& assists() { return m_assists; }
    const AssistActivity& assistActivity() const { return m_assistActivity; }

    /// Car-space origin and orientation in the world.
    glm::vec3 position() const;
    const glm::quat& orientation() const { return m_orientation; }
    glm::mat4 transform() const;
    const glm::vec3& velocity() const { return m_velocity; }
    const glm::vec3& angularVelocity() const { return m_angularVelocity; }
    float speed() const { return glm::length(m_velocity); }
    /// Speed along the car's forward axis; negative when reversing.
    float forwardSpeed() const;
    glm::vec3 forward() const { return m_orientation * glm::vec3(0.0F, 0.0F, 1.0F); }
    glm::vec3 up() const { return m_orientation * glm::vec3(0.0F, 1.0F, 0.0F); }
    glm::vec3 right() const { return m_orientation * glm::vec3(1.0F, 0.0F, 0.0F); }

    float engineRpm() const;
    /// 0 is neutral, -1 reverse, 1 and up the forward gears.
    int gear() const { return m_gear; }
    bool shifting() const { return m_shiftTimer > 0.0F; }
    float throttleApplied() const { return m_throttleApplied; }
    const std::array<WheelState, kWheels>& wheels() const { return m_wheels; }
    /// Wheel centre at rest in car space.
    glm::vec3 wheelRestPosition(int wheel) const;
    float wheelRadius(int wheel) const { return wheel < 2 ? m_spec.frontTires.radius() : m_spec.rearTires.radius(); }
    bool allWheelsOffGround() const;

    /// Adds an impulse at a world point, for collisions. `angularScale`
    /// scales the turning part, as the game's collision torque scales do.
    void applyImpulse(const glm::vec3& impulse, const glm::vec3& worldPoint, float angularScale = 1.0F);
    /// Resolves the body's overlaps with the static world after a step as
    /// the game does: impulses that stop the approach and resist sliding,
    /// turning the car only by the settings' car-world torque scale at its
    /// speed, and a move out of the overlap. Returns the largest normal
    /// impulse applied (N·s), for damage and sound.
    float resolveWorldContacts(std::span<const WorldContact> contacts, const CollisionSettings& settings);
    /// Moves the car without changing its motion, to resolve penetration.
    void translate(const glm::vec3& offset) { m_cgPosition += offset; }
    /// Sets the car upright and still where it is.
    void recover(const GroundQuery& ground);
    /// Places the chassis directly, for replays and network updates.
    void setState(const glm::vec3& position, const glm::quat& orientation, const glm::vec3& velocity,
        const glm::vec3& angularVelocity);

    /// How much a unit impulse along `direction` at `worldPoint` changes the
    /// point's velocity along it: 1/m plus the rotational term. Two-body
    /// contacts divide by the sum of both bodies' responses.
    float impulseResponse(const glm::vec3& worldPoint, const glm::vec3& direction, float angularScale = 1.0F) const;
    float mass() const { return m_mass; }
    /// Centre of mass in car space.
    const glm::vec3& centreOfMass() const { return m_cgLocal; }
    glm::vec3 worldPointVelocity(const glm::vec3& worldPoint) const;

private:
    /// A wheel's contact for the current step.
    struct Contact {
        bool active = false;
        glm::vec3 heading{0.0F, 0.0F, 1.0F};
        glm::vec3 side{1.0F, 0.0F, 0.0F};
        /// Contact patch velocity along the wheel's heading and to its left.
        float vx = 0.0F;
        float vy = 0.0F;
        float grip = 1.0F;
        /// Longitudinal force at the step's starting spin, and its slope
        /// against spin times the wheel radius.
        float fx0 = 0.0F;
        float slope = 0.0F;
    };

    /// Entries of the speed-sensitive lock table: the game samples it at
    /// 32 speeds from 0 to 150 mph (0x82DE7E70).
    static constexpr int kLockTableSize = 32;
    static constexpr float kLockTableTopSpeed = 67.056F;

    void updateSteering(const CarControls& controls, float dt);
    void buildLockTable();
    float lateralCapability(float speed) const;
    float lockAt(float speed) const;
    float normalisedSlip(int wheel) const;
    /// Moves the steering's angular velocity towards `target` within `rate`
    /// and `acceleration` (0x82DE4990) and integrates the angle.
    void steerTowards(float target, float rate, float acceleration, float dt);
    void updateGearbox(const CarControls& controls, float dt);
    void computeSuspension(const GroundQuery& ground, float dt, glm::vec3& force, glm::vec3& torque);
    void computeAero(glm::vec3& force, glm::vec3& torque) const;
    float engineTorque(float rpm, float throttle) const;
    float brakeTorque(int wheel, const CarControls& controls) const;
    /// The game's assist step: sets each wheel's share of the brake pedal
    /// (stability management, then ABS) and returns the throttle traction
    /// control leaves.
    float applyAssists(const CarControls& controls, float dt);
    /// Returns true when it braked.
    bool applyStabilityManagement();
    /// Returns true when it let go of a braked wheel.
    bool applyAbs(const CarControls& controls);
    float tractionControlCut() const;
    /// Wheel slip ratio over the tyre's peak slip ratio.
    float normalisedSlipRatio(int wheel) const;
    /// FrictionTorqueMod: scales and clamps the yaw part of the tyres'
    /// torque about the centre of mass.
    glm::vec3 modifyFrictionTorque(const glm::vec3& tyreTorque) const;
    void tyreForces(int wheel, float spin, float& fx, float& fy, float& slipRatio, float& magnitude) const;
    void prepareTyres(float dt);
    float effectiveWheelInertia(int wheel, float dt) const;
    void solveDrivetrain(float throttle, const CarControls& controls, float dt);
    void integrateWheels(float dt, glm::vec3& force, glm::vec3& torque);
    bool isDriven(int wheel) const;
    float drivenWheelSpeed() const;
    float currentRatio() const;
    glm::mat3 worldInverseInertia() const;

    CarSpec m_spec;
    DrivingAssists m_assists;

    float m_mass = 1400.0F;
    glm::vec3 m_inertiaLocal{1.0F};
    glm::vec3 m_cgLocal{0.0F};
    std::array<glm::vec3, kWheels> m_wheelRest{};
    std::array<float, kWheels> m_staticLoad{};
    std::array<float, kWheels> m_wheelInertia{};
    /// Peak slip ratio and peak slip angle (degrees) of the tyres, at the
    /// static load.
    std::array<float, kWheels> m_peakSlipRatio{};
    std::array<float, kWheels> m_peakSlipAngle{};

    glm::vec3 m_cgPosition{0.0F};
    glm::quat m_orientation{1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 m_velocity{0.0F};
    glm::vec3 m_angularVelocity{0.0F};

    std::array<WheelState, kWheels> m_wheels{};
    std::array<Contact, kWheels> m_contacts{};
    bool m_clutchLocked = false;
    float m_steerAngle = 0.0F;
    /// Steering angular velocity (rad/s) and the find-peak state: the time
    /// spent at full lock and the lock added beyond the table.
    float m_steerRate = 0.0F;
    float m_findPeakTime = 0.0F;
    float m_findPeakExtra = 0.0F;
    std::array<float, kLockTableSize> m_lockTable{};

    int m_gear = 1;
    int m_pendingGear = 1;
    float m_shiftTimer = 0.0F;
    float m_engineSpeed = 0.0F;
    float m_clutchEngagement = 1.0F;
    float m_throttleApplied = 0.0F;
    float m_tcsCut = 0.0F;
    /// Each wheel's brake pedal after the assists (0 to 1), and the time
    /// since ABS last released it.
    std::array<float, kWheels> m_wheelBrake{};
    std::array<float, kWheels> m_absTimer{};
    AssistActivity m_assistActivity;
    float m_accumulator = 0.0F;
};

/// Appends where car `a` overlaps car `b`, as contacts on `b`: each car's
/// shape is its spheres (in car space), or with `boxA` / `boxB` its
/// bounding box (see carContactIsBox()). Two boxes meet through the first
/// car's spheres.
void findCarContacts(const CarPhysics& a, std::span<const CarSpec::CollisionSphere> spheresA, bool boxA,
    const CarPhysics& b, std::span<const CarSpec::CollisionSphere> spheresB, bool boxB, std::vector<CarContact>& out);

/// Resolves two cars' overlaps after a step as the game's response to a
/// pair of bodies does (0x82E04A60): impulses along the normals only, which
/// turn each car by only `turnA` or `turnB` of their share, and remove
/// (1 + `restitution`) of the approach speed, so a negative restitution
/// leaves some of it; and moves that take both cars out of the overlap,
/// the lighter one further. Returns the largest impulse applied (N·s).
float resolveCarContacts(
    CarPhysics& a, CarPhysics& b, std::span<const CarContact> contacts, float turnA, float turnB, float restitution);

} // namespace fh
