#include "TyreEffects.h"

#include "ParticleManager.h"
#include "ParticleSystem.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace fh::particles {
namespace {

float saturate(float v)
{
    return std::clamp(v, 0.0F, 1.0F);
}

/// The friction circle thresholds the caller passes to 0x82621790
/// (constants 0x82000C98 and 0x82000CE0; physicssettings.ini's
/// SkidGraphicsFricCircleMin/Max hold the same numbers).
constexpr float kFrictionCircleMin = 0.949999988079071F;
constexpr float kFrictionCircleMax = 2.0F;
/// Wheels further than this from the car are ignored (0x82E96D08).
constexpr float kMaxWheelDistance = 6.0F;
/// The grass spray is tilted by -20 degrees and the dirt and gravel
/// sprays by -30 degrees about the axle (0x82255A7C, 0x82255A78).
constexpr float kGrassTilt = -0.3490658402442932F;
constexpr float kDirtTilt = -0.5235987901687622F;

struct Emission {
    float count = 1.0F;
    float life = 1.0F;
    float size = 1.0F;
    float speed = 1.0F;
};

} // namespace

float skidIntensity(const TyreWheelInput& wheel, const SurfaceEffects& surface)
{
    const float s = wheel.normalisedLoad;
    float k = s - 1.0F >= 0.0F ? 1.0F : 2.0F - s;
    if (-s >= 0.0F) {
        k = 2.0F;
    }
    const float a = k * kFrictionCircleMax;
    const float b = k * kFrictionCircleMin;
    const float lo = std::min(a, b);
    const float hi = std::max(a, b);

    float loadWeight = s - 1.3F >= 0.0F ? 1.0F : (s - 0.05F) * 0.8F;
    if (0.05F - s >= 0.0F) {
        loadWeight = 0.0F;
    }
    const float x = wheel.frictionCircle;
    float t = x - hi >= 0.0F ? 1.0F : (x - lo) / (hi - lo);
    if (lo - x >= 0.0F) {
        t = 0.0F;
    }
    float intensity = loadWeight * t;
    if (glm::dot(wheel.wheelVelocity, wheel.wheelVelocity) > 0.1F && std::fabs(wheel.wheelRpm) > 0.1F) {
        intensity = std::max(intensity, surface.minIntensity);
    }
    return intensity;
}

TyreEffects::TyreEffects(ParticleManager& manager, SurfaceEffectTable surfaces)
    : m_manager(&manager)
    , m_surfaces(std::move(surfaces))
{
    for (std::size_t i = 0; i < kTyreChannelCount; ++i) {
        m_channels[i] = manager.find(tyreChannelName(static_cast<TyreChannel>(i)));
    }
}

void TyreEffects::emit(const TyreCarInput& car)
{
    if (!car.enabled || m_manager->frameDt() <= 0.0F) {
        return;
    }
    for (std::size_t w = 0; w < car.wheels.size(); ++w) {
        const TyreWheelInput& wheel = car.wheels[w];
        if (!wheel.onGround) {
            continue;
        }
        const SurfaceEffects* surface = m_surfaces.find(wheel.surface);
        if (surface == nullptr) {
            continue;
        }
        const float intensity = skidIntensity(wheel, *surface);
        float scatter = 0.0F;
        for (std::size_t i = static_cast<std::size_t>(TyreChannel::Leaf1);
            i <= static_cast<std::size_t>(TyreChannel::Leaf6); ++i) {
            scatter += surface->weights[i];
        }
        scatter += surface->weight(TyreChannel::Litter);
        if (intensity > 0.01F || wheel.slide > 0.01F || scatter > 0.01F) {
            emitWheel(car, w, intensity, *surface);
        }
    }
}

void TyreEffects::emitWheel(const TyreCarInput& car, std::size_t w, float intensity, const SurfaceEffects& surface)
{
    const TyreWheelInput& wheel = car.wheels[w];
    const WheelFrame& f = wheel.frame;
    glm::vec3 position = f.position;
    if (glm::length(position - car.position) > kMaxWheelDistance) {
        return;
    }
    const bool rear = w >= 2;
    const float speed = glm::length(car.velocity);
    const float slide = wheel.slide;
    const float tread = wheel.treadSpeed;

    // The spray leaves against row 2's horizontal part, flipped when the
    // wheel turns backwards; particles are spread along the unflipped
    // vector.
    const glm::vec3 segment(-f.row2.x, f.row2.y, -f.row2.z);
    const float sign = tread >= 0.0F ? 1.0F : -1.0F;
    const glm::vec3 direction(segment.x * sign, segment.y, segment.z * sign);
    const glm::vec3 zero(0.0F);

    const auto emitFrom = [&](ParticleSystem* system, const Emission& e, const glm::vec3& at, const glm::vec3& dir,
                              const glm::vec3& inherit) {
        if (system == nullptr) {
            return;
        }
        EmitRequest request;
        request.countScale = e.count;
        request.lifeScale = e.life;
        request.sizeScale = e.size;
        request.speedScale = e.speed;
        request.position = at;
        request.direction = dir;
        request.inheritVelocity = inherit;
        request.segment = segment;
        m_manager->emit(*system, request);
    };

    const float smoke = surface.weight(TyreChannel::Smoke);
    if (smoke > 0.0F && slide > 0.3F) {
        const float fast = saturate((speed - 8.0F) * 0.5F);
        emitFrom(channel(TyreChannel::Smoke), {(1.0F - fast) * smoke * slide}, position, direction, car.velocity);
    }

    const auto dustBlock = [&](TyreChannel weightChannel, TyreChannel dust, TyreChannel close, TyreChannel bumper) {
        const float weight = surface.weight(weightChannel);
        if (weight <= 0.0F || slide <= 0.0F) {
            return;
        }
        const float dustScale = car.detailed ? 0.25F : 0.75F;
        const float closeScale = car.detailed ? 1.0F : 0.3F;
        const float fast = saturate((speed - 10.0F) * 0.1F);
        const float life = std::max(std::min(1.0F - fast * 0.7F, 1.0F), 0.3F);
        emitFrom(channel(dust), {saturate(fast + slide) * dustScale * weight, life, fast + 1.0F, 1.0F}, position,
            direction, car.velocity);
        emitFrom(channel(close), {closeScale * weight * slide}, position, direction, car.velocity);
        if (!car.detailed) {
            return;
        }
        const float bumperSpeed = std::clamp((speed - 4.0F) * 0.06666667014360428F, 0.0F, 3.0F) + 1.0F;
        const float bumperCount = saturate((speed - 7.0F) * 0.06666667014360428F + slide);
        position += (w == 2 ? -0.3F : 0.3F) * car.right;
        position.y -= wheel.radius * 0.800000011920929F;
        position += (rear ? -0.3F : 0.3F) * car.forward;
        const float tilt = saturate(car.forward.y * 10.0F) * 0.1F - 0.1F;
        glm::vec3 bumperDirection;
        if (rear && tread > -1.0F) {
            bumperDirection = glm::vec3(car.forward.x, 1.0F - car.forward.y + tilt, car.forward.z);
        } else if (!rear && tread < -1.0F) {
            bumperDirection = glm::vec3(-car.forward.x, 1.0F - car.forward.y + tilt, -car.forward.z);
        } else {
            return;
        }
        emitFrom(
            channel(bumper), {bumperCount * weight, 1.0F, 1.0F, bumperSpeed}, position, bumperDirection, car.velocity);
    };
    dustBlock(TyreChannel::Dust, TyreChannel::Dust, TyreChannel::DustClose, TyreChannel::DustBumper);
    dustBlock(TyreChannel::Dust2, TyreChannel::Dust2, TyreChannel::DustClose2, TyreChannel::DustBumper2);
    dustBlock(TyreChannel::Dust3, TyreChannel::Dust3, TyreChannel::DustClose3, TyreChannel::DustBumper3);

    const float grass = surface.weight(TyreChannel::Grass);
    ParticleSystem* grassSystem = channel(TyreChannel::Grass);
    if (grass > 0.0F && intensity > 0.0F && car.detailed && grassSystem != nullptr) {
        const glm::vec3 tilted = transformRow(direction, rotationNormal(f.axle, kGrassTilt));
        position.y -= wheel.radius * 0.800000011920929F;
        const float throwSpeed = std::min(std::fabs(tread), grassSystem->definition().velocity.value);
        emitFrom(grassSystem, {grass * intensity}, position, tilted, tilted * throwSpeed);
    }

    const auto dirtBlock = [&](TyreChannel c) {
        ParticleSystem* system = channel(c);
        const float weight = surface.weight(c);
        if (system == nullptr || weight <= 0.0F || tread <= 8.0F || !car.detailed) {
            return;
        }
        const glm::vec3 tilted = transformRow(direction, rotationNormal(f.axle, kDirtTilt));
        position.y -= wheel.radius;
        const float amount = slide < intensity ? intensity : slide;
        emitFrom(system, {weight * amount}, position, tilted, zero);
    };
    dirtBlock(TyreChannel::Dirt);
    dirtBlock(TyreChannel::Dirt2);

    for (std::size_t i = static_cast<std::size_t>(TyreChannel::Leaf1);
        i <= static_cast<std::size_t>(TyreChannel::Leaf6); ++i) {
        const auto c = static_cast<TyreChannel>(i);
        const float weight = surface.weight(c);
        if (channel(c) == nullptr || weight <= 0.0F) {
            continue;
        }
        if ((rear && tread > -1.0F) || (!rear && tread < -1.0F)) {
            const float fast = saturate((speed - 4.0F) * 0.043478261679410934F);
            emitFrom(channel(c),
                {fast * 0.8999999761581421F * weight, 1.2999999523162842F - fast * 0.30000001192092896F, 1.0F,
                    fast * 0.20000000298023224F + 0.800000011920929F},
                position, direction, car.velocity);
        }
    }

    ParticleSystem* gravel = channel(TyreChannel::GravelBits);
    const float gravelWeight = surface.weight(TyreChannel::GravelBits);
    if (gravel != nullptr && gravelWeight > 0.0F && tread > 8.0F && car.detailed) {
        const glm::vec3 tilted = transformRow(direction, rotationNormal(f.axle, kDirtTilt));
        position.y -= wheel.radius;
        const float amount = slide < intensity ? intensity : slide;
        const float fast = saturate((speed - 7.0F) * 0.05000000074505806F);
        emitFrom(gravel, {(fast + 1.0F) * gravelWeight * amount}, position, tilted, zero);
    }

    const float litter = surface.weight(TyreChannel::Litter);
    if (channel(TyreChannel::Litter) != nullptr && litter > 0.0F
        && ((rear && tread > 0.0F) || (!rear && tread < 0.0F))) {
        const float fast = saturate((speed - 13.5F) * 0.07407407462596893F);
        emitFrom(channel(TyreChannel::Litter),
            {(fast + 1.0F) * litter, fast * 0.11999999731779099F + 1.0F, 1.0F, fast * 0.4300000071525574F + 1.0F},
            position, direction, car.velocity);
    }
}

} // namespace fh::particles
