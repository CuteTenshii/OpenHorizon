#pragma once

#include "SurfaceEffects.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>

namespace fh::particles {

class ParticleManager;
class ParticleSystem;

/// A wheel's frame as the game hands it to the tyre effects: the rows of the
/// matrix the car presentation passes (0x825159F0 to 0x82E96C48). Row 0 is
/// the axle direction (front wheels are offset 5 cm along it, grass and dirt
/// sprays are tilted about it); row 2 is the direction whose horizontal part
/// the sprays leave against; row 3 is the wheel position.
struct WheelFrame {
    glm::vec3 axle{1.0F, 0.0F, 0.0F};
    glm::vec3 row1{0.0F, 1.0F, 0.0F};
    glm::vec3 row2{0.0F, 0.0F, 1.0F};
    glm::vec3 position{0.0F};
};

/// Per-wheel inputs of the game's tyre effect code.
struct TyreWheelInput {
    bool onGround = false;
    /// Surface type id of the contact (index into surfaceTypes.xml).
    std::size_t surface = 0;
    WheelFrame frame;
    /// Tyre radius in metres (the physics wheel's +0x340).
    float radius = 0.0F;
    /// Tread speed, radius times wheel angular speed, m/s; positive when the
    /// wheel rolls forward.
    float treadSpeed = 0.0F;
    /// The wheel state value the game gates and scales smoke and dust by
    /// (physics wheel +0x28; one of three values the replay stores together
    /// with scale 10). Its exact derivation in the game's tyre model is not
    /// decoded.
    float slide = 0.0F;
    /// Combined tyre slip relative to the friction circle, the quantity the
    /// SkidGraphicsFricCircleMin/Max constants (0.95, 2) are compared with.
    float frictionCircle = 0.0F;
    /// Wheel load over a quarter of the car's weight (4 * load / (mass * g),
    /// 0x82DDFEB0).
    float normalisedLoad = 0.0F;
    /// Velocity of the wheel (the MinIntensity floor applies only when it
    /// exceeds sqrt(0.1) m/s and the wheel turns faster than 0.1 rpm).
    glm::vec3 wheelVelocity{0.0F};
    float wheelRpm = 0.0F;
};

struct TyreCarInput {
    /// Car velocity: its length is the speed the effect ramps use, and
    /// smoke, dust, leaves and litter inherit it.
    glm::vec3 velocity{0.0F};
    /// The car's frame from its presentation (+912 right, +928 up, +944
    /// forward, +960 position). Bumper dust is offset along right and
    /// forward and aimed with forward.
    glm::vec3 right{1.0F, 0.0F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    glm::vec3 position{0.0F};
    /// The presentation's flag at +0x162C/+0x8070 (a byte of the car render
    /// object) that gates grass, dirt, gravel and bumper dust and raises the
    /// close dust. What sets it is not decoded; it is on for a car drawn
    /// with its full model.
    bool detailed = true;
    /// The caller's own condition (0x825159F0, r26) for emitting at all.
    bool enabled = true;
    std::array<TyreWheelInput, 4> wheels{};
};

/// The skid intensity of a wheel (0x82621790): how far the friction circle
/// value is between the thresholds, scaled by the wheel's load, floored at
/// the surface's MinIntensity while the wheel moves.
float skidIntensity(const TyreWheelInput& wheel, const SurfaceEffects& surface);

/// The tyre smoke, dust, grass, dirt, gravel, leaves and litter of the
/// game's car presentation: per wheel and frame, the rules of 0x825159F0
/// and CCarPresentation::EmitTireSmoke (0x82E96C48) decide which systems
/// emit and with what count, life, size and speed scales. The rim smoke
/// (SmokeRim0-3) is left out: it is positioned from presentation data that
/// is not decoded.
class TyreEffects {
public:
    TyreEffects(ParticleManager& manager, SurfaceEffectTable surfaces);

    /// Emits for every wheel of one car. Call once per frame before
    /// ParticleManager::update().
    void emit(const TyreCarInput& car);

    const SurfaceEffectTable& surfaces() const { return m_surfaces; }

private:
    void emitWheel(const TyreCarInput& car, std::size_t wheel, float intensity, const SurfaceEffects& surface);
    ParticleSystem* channel(TyreChannel c) const { return m_channels[static_cast<std::size_t>(c)]; }

    ParticleManager* m_manager;
    SurfaceEffectTable m_surfaces;
    std::array<ParticleSystem*, kTyreChannelCount> m_channels{};
};

} // namespace fh::particles
