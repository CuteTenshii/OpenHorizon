#include "ParticleSystem.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace fh::particles {
namespace {

/// The executable's float constants for pi and pi/2 (0x82000D24,
/// 0x8200C908).
constexpr float kPi = 3.1415927410125732F;
constexpr float kHalfPi = 1.5707963705062866F;
constexpr float kTwoPi = 6.2831854820251465F;

/// Row-vector product of two rotations: apply `a`, then `b`.
glm::mat3 multiply(const glm::mat3& a, const glm::mat3& b)
{
    return {transformRow(a[0], b), transformRow(a[1], b), transformRow(a[2], b)};
}

glm::mat3 rotationZ(float angle)
{
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    return {glm::vec3(c, s, 0.0F), glm::vec3(-s, c, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F)};
}

/// Normalises like the game's vector code: a zero vector stays zero.
glm::vec3 safeNormalize(const glm::vec3& v)
{
    const float len2 = glm::dot(v, v);
    return len2 > 0.0F ? v / std::sqrt(len2) : v;
}

/// The angles the renderer turns a travel direction into for
/// DirectionOfTravel and UserDefined spin axes: (0, -y, z) of the unit
/// vector times pi/2 (0x82DC5F84, 0x82DC60F0).
glm::vec3 axisAngles(const glm::vec3& direction)
{
    const glm::vec3 n = safeNormalize(direction);
    return glm::vec3(0.0F, -n.y, n.z) * (kPi * 0.5F);
}

/// The emitter angles 0x82DC7090 derives from an emission direction:
/// the heading of (dx, -dz) through the executable's polynomial arcsine
/// (coefficients at 0x82004740) and the y component scaled linearly to
/// an angle.
glm::vec3 emitterAngles(const glm::vec3& d)
{
    const glm::vec2 horizontal = safeNormalize(glm::vec3(-d.z, d.x, 0.0F));
    const float n0 = horizontal.x;
    const float n1 = horizontal.y;
    const float a = std::fabs(n1);
    const float oneMinus = 1.0000001192092896F - a;
    constexpr float kC0 = -1.3617826700210571F;
    constexpr float kC1 = 2.3794949054718018F;
    constexpr float kC2 = -0.8082285523414612F;
    constexpr float kC3 = 0.2784401476383209F;
    const float arcsine = std::sqrt(oneMinus) * n1 * kC0 + n1 * kC1 + a * n1 * kC2 + oneMinus * a * (a * n1) * kC3;
    const float heading = (n0 >= 0.0F ? 1.0F : -1.0F) * (kHalfPi - arcsine);
    return {0.0F, heading, d.y * -kHalfPi};
}

} // namespace

glm::mat3 rotationX(float angle)
{
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    return {glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, c, s), glm::vec3(0.0F, -s, c)};
}

glm::mat3 rotationY(float angle)
{
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    return {glm::vec3(c, 0.0F, -s), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(s, 0.0F, c)};
}

glm::mat3 rollPitchYaw(float pitch, float yaw, float roll)
{
    return multiply(multiply(rotationZ(roll), rotationX(pitch)), rotationY(yaw));
}

glm::mat3 rotationNormal(const glm::vec3& axis, float angle)
{
    const float s = std::sin(angle);
    const float c = std::cos(angle);
    const float t = 1.0F - c;
    const float x = axis.x;
    const float y = axis.y;
    const float z = axis.z;
    return {glm::vec3(c + t * x * x, t * x * y + s * z, t * x * z - s * y),
        glm::vec3(t * x * y - s * z, c + t * y * y, t * y * z + s * x),
        glm::vec3(t * x * z + s * y, t * y * z - s * x, c + t * z * z)};
}

glm::vec3 transformRow(const glm::vec3& p, const glm::mat3& m)
{
    return p.x * m[0] + p.y * m[1] + p.z * m[2];
}

ParticleSystem::ParticleSystem(const EffectDefinition& definition, std::uint32_t seed)
    : m_definition(&definition)
    , m_capacity(definition.budget)
    , m_particles(definition.budget)
    , m_bubble(definition.budget)
    , m_random(seed)
{
}

glm::mat3 ParticleSystem::orientation(const Particle& p)
{
    return multiply(rotationX(p.spinAngle), rollPitchYaw(p.axis.x, p.axis.y, p.axis.z));
}

void ParticleSystem::clear()
{
    m_head = 0;
    m_count = 0;
    m_rateAccumulator = 0.0F;
}

glm::vec3 ParticleSystem::randomUnit()
{
    // 0x82DBE5C0: a height uniform in [-1, 1] and an angle uniform in
    // [0, 2pi). Which output component takes the height is not decoded; it
    // does not change the distribution.
    const float z = m_random.uniform() * 2.0F - 1.0F;
    const float angle = m_random.uniform() * kTwoPi;
    const float r = std::sqrt(std::max(0.0F, 1.0F - z * z));
    return {r * std::cos(angle), r * std::sin(angle), z};
}

void ParticleSystem::emit(const EmitRequest& request, float frameDt, GameRandom& crt)
{
    const EffectDefinition& d = *m_definition;
    if (m_capacity == 0) {
        return;
    }
    const float q = d.quantity.value;
    const float drawn = m_random.range(q - d.quantity.variation * q, (d.quantity.variation + 1.0F) * q);
    m_rateAccumulator += drawn * m_budgetScale * frameDt * request.countScale;
    const float whole = std::trunc(m_rateAccumulator);
    m_rateAccumulator -= whole;
    const auto cap = static_cast<float>(m_capacity);
    m_rateAccumulator = std::clamp(m_rateAccumulator, 0.0F, cap);
    if (whole <= 0.0F) {
        return;
    }
    const std::size_t free = m_capacity - m_count;
    const std::size_t count = std::min(free, static_cast<std::size_t>(whole));
    if (count == 0) {
        return;
    }
    generate(count, request.position, emitterAngles(request.direction), request, crt);
}

bool ParticleSystem::generate(
    std::size_t count, const glm::vec3& position, const glm::vec3& angles, const EmitRequest& request, GameRandom& crt)
{
    const EffectDefinition& d = *m_definition;
    // The shape generators (0x824A9B80 SuperSpray, 0x82DC1568 Plane) fill
    // position and direction; spawn() turns the direction into a velocity.
    const std::size_t first = m_count;
    if (d.shape == EmitterShape::SuperSpray) {
        for (std::size_t k = 0; k < count; ++k) {
            Particle& p = slot(first + k);
            p.position = position + request.segment * crt.uniform();
            const float axis = crt.range(d.offAxis.value - d.offAxis.variation, d.offAxis.value + d.offAxis.variation);
            const float plane
                = crt.range(d.offPlane.value - d.offPlane.variation, d.offPlane.value + d.offPlane.variation);
            const glm::mat3 m = rollPitchYaw(angles.x, angles.y + axis, angles.z + plane);
            p.velocity = m[0];
        }
    } else if (d.shape == EmitterShape::Plane) {
        const glm::mat3 m = rollPitchYaw(angles.x, angles.y, angles.z);
        const float halfLength = d.size.box.x * request.shapeScale.x * 0.5F;
        const float halfWidth = d.size.box.z * request.shapeScale.z * 0.5F;
        for (std::size_t k = 0; k < count; ++k) {
            Particle& p = slot(first + k);
            p.velocity = m[1];
            const float z = crt.range(-halfWidth, halfWidth);
            const float x = crt.range(-halfLength, halfLength);
            const float r = crt.uniform();
            p.position = position + request.segment * r + transformRow(glm::vec3(x, 0.0F, z), m);
        }
    } else {
        // Box emitters (0x82DC1398) are only used by debris effects and
        // their generator is not decoded.
        return false;
    }
    m_count += count;
    m_spawnedThisFrame += static_cast<std::uint32_t>(count);
    spawn(first, count, request, crt);
    return true;
}

SystemEmitter ParticleSystem::makeEmitter(int id, const glm::vec3& position, const glm::vec3& angles) const
{
    const EffectDefinition& d = *m_definition;
    SystemEmitter e;
    e.id = id;
    e.position = position;
    e.angles = angles;
    // 0x82dc3a60 passes quantity (+368) times the budget scale (+376), the
    // emit times (+380, +384), and afterwards copies the infinite flag
    // (+532) over the one 0x82dc31f0 derived from a negative end.
    e.rate = d.quantity.value * m_budgetScale;
    e.start = d.emitStart;
    e.end = d.emitEnd;
    e.infinite = d.emitInfinite;
    return e;
}

void ParticleSystem::addEmitter(const SystemEmitter& emitter)
{
    m_emitters.push_back(emitter);
}

bool ParticleSystem::hasEmitter(int id) const
{
    return std::ranges::any_of(m_emitters, [id](const SystemEmitter& e) { return e.id == id; });
}

void ParticleSystem::stopEmitters(int id, std::optional<float> finishTime)
{
    for (SystemEmitter& e : m_emitters) {
        if (e.id != id) {
            continue;
        }
        if (e.infinite) {
            e.stopping = true;
        } else if (finishTime) {
            e.start = 0.0F;
            e.end = *finishTime;
        }
    }
}

void ParticleSystem::updateEmitters(float dt, GameRandom& crt)
{
    // 0x82414440, per emitter; `remove` is its out flag, which has the
    // update loop delete the emitter. The emitters placed here are neither
    // kept past their end (+154 clear) nor moving (their velocity, +96, is
    // the zero vector 0x82001340 that 0x82dc3a60 passes).
    constexpr float kMinimumRate = 0.5F;
    // Particles take no velocity from these emitters: inferred, the vector
    // the loop passes to the spawner (0x82417124) comes from VMX128 code the
    // disassembler does not decode.
    const EmitRequest request;
    std::erase_if(m_emitters, [&](SystemEmitter& e) {
        e.age += dt;
        if (e.rate < kMinimumRate) {
            return true;
        }
        if (!e.infinite) {
            e.start -= dt;
            e.end -= dt;
            if (e.end <= 0.0F) {
                return true;
            }
        }
        if (e.stopping) {
            return true;
        }
        if (!e.infinite && e.start > 0.0F) {
            return false;
        }
        const float due = e.rate * dt * m_budgetScale + e.accumulator;
        const float whole = std::trunc(due);
        e.accumulator = due - whole;
        const std::size_t count = std::min(m_capacity - m_count, static_cast<std::size_t>(whole));
        if (count > 0) {
            generate(count, e.position, e.angles, request, crt);
        }
        return false;
    });
}

void ParticleSystem::spawn(std::size_t first, std::size_t count, const EmitRequest& request, GameRandom& crt)
{
    const EffectDefinition& d = *m_definition;
    const SizeCurve& s = d.size;
    const float spinBase = d.spinTime.value > 0.01F ? kPi / d.spinTime.value : 0.0F;
    const float speedValue = d.velocity.value;
    const float speedVar = d.velocity.variation;

    for (std::size_t k = 0; k < count; ++k) {
        Particle& p = slot(first + k);
        p.alive = true;

        const float lifeVar = d.life.variation * request.lifeScale;
        const float lifeBase = d.life.value * request.lifeScale;
        p.life = m_random.range(lifeBase - lifeVar * lifeBase, (lifeVar + 1.0F) * lifeBase);
        p.remaining = (lifeVar + 1.0F) * lifeBase;
        p.age = 0.0F;
        p.random = static_cast<float>(m_random.next15() & 0xFFU);

        const auto drawSize = [this](float value, float variation) {
            return m_random.range(value - variation * value, (variation + 1.0F) * value);
        };
        p.maxSize.x = drawSize(s.maxValue.x, s.variation.x) * request.sizeScale;
        p.maxSize.y = drawSize(s.maxValue.y, s.variation.y) * request.sizeScale;
        if (s.growFor.x > 0.0F) {
            p.startSize.x = s.value.x;
            p.size.x = s.value.x;
        } else {
            p.size.x = p.maxSize.x;
        }
        if (s.growFor.y > 0.0F) {
            p.startSize.y = s.value.y;
            p.size.y = s.value.y;
        } else {
            p.size.y = p.maxSize.y;
        }
        p.endSize.x = s.shrinkFor.x > 0.0F ? drawSize(s.endValue.x, s.variation.x) : p.maxSize.x;
        p.endSize.y = s.shrinkFor.y != 0.0F ? drawSize(s.endValue.y, s.variation.y) : p.maxSize.y;

        const float speed = m_random.range(speedValue - speedVar * speedValue, (speedVar + 1.0F) * speedValue);
        p.velocity = p.velocity * request.speedScale * speed;

        const MotionInheritance& mi = d.motionInheritance;
        if (mi.influence != 0) {
            const float chance = m_random.range(0.0F, 1.0F);
            if (chance < static_cast<float>(mi.influence) * 0.01F) {
                const float m = m_random.range(
                    mi.multiplier - mi.variation * mi.multiplier, (mi.variation + 1.0F) * mi.multiplier);
                p.velocity += request.inheritVelocity * m;
            }
        }

        if (d.render.type != ParticleType::BillboardedQuad) {
            switch (d.spinAxis) {
            case SpinAxis::Random:
                p.axis = randomUnit();
                break;
            case SpinAxis::DirectionOfTravel:
                p.axis = axisAngles(p.velocity);
                break;
            case SpinAxis::PerpToDirOfTravel:
                // These quads face along their velocity in the vertex shader
                // (quad type 1), which does not read the angles.
                p.axis = glm::vec3(0.0F);
                break;
            case SpinAxis::UserDefined:
                p.axis = axisAngles(d.spinAxisUser);
                break;
            }
        }

        float sign = d.spinPhase.value >= 0.0F ? 1.0F : -1.0F;
        if (d.spinAlternate) {
            sign = m_random.range(-1.0F, 1.0F) >= 0.0F ? 1.0F : -1.0F;
        }
        p.spinRate
            = m_random.range(spinBase - d.spinTime.variation * spinBase, (d.spinTime.variation + 1.0F) * spinBase)
            * sign;
        p.spinAngle
            = m_random.range(d.spinPhase.value - d.spinPhase.variation, d.spinPhase.value + d.spinPhase.variation);

        const float push = m_random.range(0.0F, 0.015F) + mi.offset;
        p.position += p.velocity * push;
    }

    for (const Behaviour& b : d.behaviours) {
        if (b.kind != Behaviour::Kind::BubbleMotion) {
            continue;
        }
        const BubbleMotion& bm = b.bubble;
        for (std::size_t k = 0; k < count; ++k) {
            BubbleState& st = bubbleSlot(first + k);
            st.amplitude = crt.range(bm.amplitude.value - bm.amplitude.variation * bm.amplitude.value,
                (bm.amplitude.variation + 1.0F) * bm.amplitude.value);
            st.period = crt.range(bm.period.value - bm.period.variation * bm.period.value,
                            (bm.period.variation + 1.0F) * bm.period.value)
                + 0.1F;
            st.time = crt.range(bm.phase.value - bm.phase.variation * kPi, bm.phase.value + bm.phase.variation * kPi);
            const float ax = crt.range(-1.0F, 1.0F);
            const float ay = crt.range(-1.0F, 1.0F);
            const float az = crt.range(-1.0F, 1.0F);
            st.axis = safeNormalize(glm::vec3(az, ay, ax));
            st.offset = glm::vec3(0.0F);
        }
    }
}

void ParticleSystem::applyBehaviours(float dt)
{
    const EffectDefinition& d = *m_definition;
    for (const Behaviour& b : d.behaviours) {
        switch (b.kind) {
        case Behaviour::Kind::Gravity: {
            // 0x82DBFFC0 subtracts strength * dt * dt from the vertical
            // velocity each update.
            const float dv = b.gravity.strength * dt * dt;
            for (std::size_t i = 0; i < m_count; ++i) {
                Particle& p = slot(i);
                if (p.alive) {
                    p.velocity.y -= dv;
                }
            }
            break;
        }
        case Behaviour::Kind::Friction:
            for (std::size_t i = 0; i < m_count; ++i) {
                Particle& p = slot(i);
                if (!p.alive || p.age < b.friction.startFrom) {
                    continue;
                }
                const float speed = glm::length(p.velocity);
                if (speed <= b.friction.velocityKept) {
                    continue;
                }
                const float keep = std::clamp(1.0F - b.friction.strength * speed * dt, 0.0F, 1.0F);
                p.velocity *= keep;
            }
            break;
        case Behaviour::Kind::BubbleMotion:
            // 0x82DC0430: undo last frame's offset, advance the phase and
            // offset the particle sideways to its velocity by a sine
            // (0x82AB7D00, the sine series at 0x820011A0).
            for (std::size_t i = 0; i < m_count; ++i) {
                Particle& p = slot(i);
                if (!p.alive) {
                    continue;
                }
                BubbleState& st = bubbleSlot(i);
                p.position -= st.offset;
                st.time += dt;
                const float wave = std::sin(st.time / st.period) * st.amplitude;
                st.offset = safeNormalize(glm::cross(p.velocity, st.axis)) * wave;
                p.position += st.offset;
            }
            break;
        case Behaviour::Kind::Wind:
        case Behaviour::Kind::WheelAttractor:
        case Behaviour::Kind::Collision:
            // Not decoded: wind (ambient effects), the rim smoke's wheel
            // attractor and debris collision planes.
            break;
        }
    }
}

void ParticleSystem::update(float dt, float rateScale)
{
    const EffectDefinition& d = *m_definition;
    if (m_capacity == 0) {
        return;
    }

    m_budgetScale = std::min(rateScale, m_budgetScale);
    const float sustainable = static_cast<float>(m_capacity) / d.life.value * dt * 0.9F;
    if (static_cast<float>(m_spawnedThisFrame) > sustainable) {
        m_budgetScale = static_cast<float>(static_cast<double>(m_budgetScale) * 0.95);
    } else if (m_budgetScale < rateScale) {
        m_budgetScale *= 1.025F;
    }
    m_spawnedThisFrame = 0;

    applyBehaviours(dt);

    const float maxLife = d.maxLife();
    const SizeCurve& s = d.size;
    for (std::size_t i = 0; i < m_count; ++i) {
        Particle& p = slot(i);
        if (!p.alive) {
            continue;
        }
        p.position += p.velocity * dt;
        p.spinAngle += p.spinRate * dt;
        const float t = p.life / maxLife * p.age;

        const auto grow = [t](float start, float max, float from, float span) {
            return start + (max - start) * std::clamp((t - from) / span, 0.0F, 1.0F);
        };
        const auto shrink = [t, &s](float end, float max, float span) {
            return end + (max - end) * std::clamp((s.shrinkTo - t) / span, 0.0F, 1.0F);
        };
        if (s.growFor.x > 0.0F && t >= s.growFrom && t <= s.growFrom + s.growFor.x) {
            p.size.x = grow(p.startSize.x, p.maxSize.x, s.growFrom, s.growFor.x);
        }
        if (s.shrinkFor.x > 0.0F && t >= s.shrinkTo - s.shrinkFor.x && t <= s.shrinkTo) {
            p.size.x = shrink(p.endSize.x, p.maxSize.x, s.shrinkFor.x);
        }
        if (s.growFor.y > 0.0F && t >= s.growFrom && t <= s.growFrom + s.growFor.y) {
            p.size.y = grow(p.startSize.y, p.maxSize.y, s.growFrom, s.growFor.y);
        }
        if (s.shrinkFor.y > 0.0F && t >= s.shrinkTo - s.shrinkFor.y && t <= s.shrinkTo) {
            p.size.y = shrink(p.endSize.y, p.maxSize.y, s.shrinkFor.y);
        }

        if (d.render.type != ParticleType::BillboardedQuad) {
            if (d.spinAxis == SpinAxis::DirectionOfTravel) {
                p.axis = axisAngles(p.velocity);
            } else if (d.spinAxis == SpinAxis::Random && p.spinRate > 0.001F && d.spinTime.value != 0.0F) {
                p.axis = transformRow(p.axis, rotationY(dt / d.spinTime.value * kPi));
            }
        }
    }

    std::size_t expired = 0;
    for (std::size_t i = 0; i < m_count; ++i) {
        Particle& p = slot(i);
        p.age += dt / p.life;
        if (p.age >= 1.0F) {
            p.alive = false;
        }
        p.remaining -= dt;
        if (p.remaining <= 0.0F) {
            ++expired;
        }
    }
    expired = std::min(expired, m_count);
    m_head = (m_head + expired) % m_capacity;
    m_count -= expired;
}

} // namespace fh::particles
