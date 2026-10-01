#pragma once

#include "Effect.h"
#include "GameRandom.h"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace openhorizon::particles {

/// The game's 64-byte particle record (offsets in the comments), which stores everything after the position as
/// halves; these are floats.
struct Particle {
    glm::vec3 position{0.0F}; // +0
    glm::vec3 velocity{0.0F}; // +16
    /// +24: a Quad particle's roll/pitch/yaw (see ParticleSystem::orientation()).
    glm::vec3 axis{0.0F};
    glm::vec2 size{0.0F}; // +32
    /// +36: age as a fraction of `life`.
    float age = 0.0F;
    /// +38: a random byte, 0-255, picking the gradient and animation rows.
    float random = 0.0F;
    float life = 0.0F; // +40
    glm::vec2 maxSize{0.0F}; // +42
    glm::vec2 startSize{0.0F}; // +46
    glm::vec2 endSize{0.0F}; // +50
    float spinAngle = 0.0F; // +54
    float spinRate = 0.0F; // +56
    /// +58: seconds until the slot is freed, from the longest drawable life even if it dies sooner.
    float remaining = 0.0F;
    /// +60 bit 0; bit 1 (visible) is always set with it here.
    bool alive = false;
};

/// Per-particle state of the BubbleMotion behaviour (0x82DC11C8).
struct BubbleState {
    float amplitude = 0.0F;
    float period = 0.0F;
    float time = 0.0F;
    glm::vec3 axis{0.0F};
    glm::vec3 offset{0.0F};
};

/// The arguments of one emission call (0x82DC7090).
struct EmitRequest {
    float countScale = 1.0F;
    float lifeScale = 1.0F;
    float sizeScale = 1.0F;
    float speedScale = 1.0F;
    glm::vec3 position{0.0F};
    /// Its horizontal heading and y become the emitter's yaw and roll (see ParticleSystem::emit()).
    glm::vec3 direction{1.0F, 0.0F, 0.0F};
    /// Multiplies the effect's Box/Plane size.
    glm::vec3 shapeScale{1.0F, 1.0F, 1.0F};
    glm::vec3 inheritVelocity{0.0F};
    /// Particles are spread along position + segment * [0, 1].
    glm::vec3 segment{0.0F};
};

/// A 192-byte emitter the system runs itself each update, started by a placed emitter (0x82dc3a60 builds it with
/// 0x82dc31f0 into the system's list at +76). An EmitRequest is instead one frame of a code-driven emitter.
struct SystemEmitter {
    /// +148: the placed emitter's id, used to stop it.
    int id = -1;
    /// +64.
    glm::vec3 position{0.0F};
    /// Pitch, yaw and roll of the emission (see rollPitchYaw()).
    glm::vec3 angles{0.0F};
    /// +128: particles per second, the effect's quantity times the budget scale at creation.
    float rate = 0.0F;
    /// Seconds until it starts (+132) and ends (+136), counted down; unused when infinite.
    float start = 0.0F;
    float end = 0.0F;
    /// +144, the effect's EmitTime infinite (the system's +532).
    bool infinite = false;
    /// +145: removed at its next update.
    bool stopping = false;
    /// +24: the fraction of a particle carried to the next update.
    float accumulator = 0.0F;
    /// +140: seconds since it was made.
    float age = 0.0F;
};

/// XNA math rotations, row vectors (p * M). rollPitchYaw is roll about Z, then pitch about X, then yaw about Y
/// (XMMatrixRotationRollPitchYaw, 0x82490C30).
glm::mat3 rotationX(float angle);
glm::mat3 rotationY(float angle);
glm::mat3 rollPitchYaw(float pitch, float yaw, float roll);
/// Rotation by `angle` about the unit vector `axis` (XMMatrixRotationNormal,
/// 0x82488D78 in the tyre code).
glm::mat3 rotationNormal(const glm::vec3& axis, float angle);
/// Row-vector product: p * M.
glm::vec3 transformRow(const glm::vec3& p, const glm::mat3& m);

/// CParticleSystemNew: a ring of at most `budget` particles with the rate accumulator and adaptive budget scale.
class ParticleSystem {
public:
    ParticleSystem(const EffectDefinition& definition, std::uint32_t seed);

    const EffectDefinition& definition() const { return *m_definition; }

    /// Emits this frame's particles (0x82DC7090) at the rate times `frameDt`, the manager's last step. `crt` stands
    /// in for the C runtime's rand(), which emitter shapes use; attributes use the system's own generator.
    void emit(const EmitRequest& request, float frameDt, GameRandom& crt);
    /// Advances particles by `dt` seconds (0x82416F58).
    void update(float dt, float rateScale);
    /// Runs the kept emitters after update() (0x82416F58, each through 0x82414440), shapes drawn from `crt`.
    void updateEmitters(float dt, GameRandom& crt);
    /// Clears the particles; the kept emitters stay.
    void clear();

    /// A kept emitter with the effect's rate and emit times (0x82dc31f0 as 0x82dc3a60 calls it).
    SystemEmitter makeEmitter(int id, const glm::vec3& position, const glm::vec3& angles) const;
    void addEmitter(const SystemEmitter& emitter);
    /// Whether an emitter of placed emitter `id` runs (0x82dc2298).
    bool hasEmitter(int id) const;
    /// 0x82dc2330: an infinite emitter goes at its next update; a timed one keeps running without `finishTime`
    /// (single effects), else restarts for `finishTime` seconds (composite members).
    void stopEmitters(int id, std::optional<float> finishTime);
    const std::vector<SystemEmitter>& emitters() const { return m_emitters; }

    std::size_t capacity() const { return m_capacity; }
    std::size_t size() const { return m_count; }
    /// The i-th particle of the ring, oldest first.
    const Particle& at(std::size_t i) const { return m_particles[(m_head + i) % m_capacity]; }
    /// A Quad particle's orientation unless PerpToDirOfTravel: X by the spin angle, then roll/pitch/yaw by `axis`
    /// (0x82DC9274; order inferred).
    static glm::mat3 orientation(const Particle& p);

    /// +376: lowered when a frame spawns more than the budget sustains.
    float budgetScale() const { return m_budgetScale; }

private:
    Particle& slot(std::size_t i) { return m_particles[(m_head + i) % m_capacity]; }
    BubbleState& bubbleSlot(std::size_t i) { return m_bubble[(m_head + i) % m_capacity]; }
    void spawn(std::size_t first, std::size_t count, const EmitRequest& request, GameRandom& crt);
    /// False for shapes not decoded.
    bool generate(std::size_t count, const glm::vec3& position, const glm::vec3& angles, const EmitRequest& request,
        GameRandom& crt);
    glm::vec3 randomUnit();
    void applyBehaviours(float dt);

    const EffectDefinition* m_definition;
    std::size_t m_capacity = 0;
    std::vector<Particle> m_particles;
    std::vector<BubbleState> m_bubble;
    std::size_t m_head = 0;
    std::size_t m_count = 0;
    GameRandom m_random;
    float m_rateAccumulator = 0.0F;
    float m_budgetScale = 1.0F;
    std::uint32_t m_spawnedThisFrame = 0;
    std::vector<SystemEmitter> m_emitters;
};

} // namespace openhorizon::particles
