#pragma once

#include "Effect.h"
#include "GameRandom.h"

#include <glm/mat3x3.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace fh::particles {

/// One live particle, with the fields of the game's 64-byte particle record
/// (the offsets in the comments). The game stores everything after the
/// position as 16-bit floats; these are full floats.
struct Particle {
    glm::vec3 position{0.0F}; // +0
    glm::vec3 velocity{0.0F}; // +16
    /// +24: for Quad particles, the roll/pitch/yaw angles their orientation
    /// is built from (see ParticleSystem::orientation()).
    glm::vec3 axis{0.0F};
    glm::vec2 size{0.0F}; // +32
    /// +36: age as a fraction of `life`.
    float age = 0.0F;
    /// +38: a random byte, 0-255, that picks the gradient row and the
    /// animation row.
    float random = 0.0F;
    float life = 0.0F; // +40
    glm::vec2 maxSize{0.0F}; // +42
    glm::vec2 startSize{0.0F}; // +46
    glm::vec2 endSize{0.0F}; // +50
    float spinAngle = 0.0F; // +54
    float spinRate = 0.0F; // +56
    /// +58: seconds until the slot is freed: the longest life the particle
    /// could have drawn, counted down whether or not it died sooner.
    float remaining = 0.0F;
    /// +60 bit 0. Bit 1 (visible) is always set with it by the code paths
    /// implemented here.
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

/// What one emission call of the game (0x82DC7090) passes: scales for the
/// particle count, life, size and speed, where to emit from and the
/// velocity particles may inherit.
struct EmitRequest {
    float countScale = 1.0F;
    float lifeScale = 1.0F;
    float sizeScale = 1.0F;
    float speedScale = 1.0F;
    glm::vec3 position{0.0F};
    /// Emission direction: its horizontal heading and its y become the
    /// emitter's yaw and roll (see ParticleSystem::emit()).
    glm::vec3 direction{1.0F, 0.0F, 0.0F};
    /// Multiplies the effect's Box/Plane size.
    glm::vec3 shapeScale{1.0F, 1.0F, 1.0F};
    glm::vec3 inheritVelocity{0.0F};
    /// Particles are spread along position + segment * [0, 1].
    glm::vec3 segment{0.0F};
};

/// An emitter the system keeps and runs by itself on every update: the
/// 192-byte emitter objects the placed emitters of a track start
/// (0x82dc3a60 builds them with 0x82dc31f0 and keeps them in the system's
/// list at +76), where an EmitRequest is one frame's emission of an
/// emitter that game code drives.
struct SystemEmitter {
    /// The placed emitter's id (+148), which stopping finds it by.
    int id = -1;
    /// +64.
    glm::vec3 position{0.0F};
    /// Pitch, yaw and roll of the emission (see rollPitchYaw()).
    glm::vec3 angles{0.0F};
    /// Particles per second (+128): the effect's quantity times the
    /// system's budget scale when the emitter is made.
    float rate = 0.0F;
    /// Seconds until the emitter starts (+132) and until it ends (+136),
    /// both counted down; neither matters to an infinite one.
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

/// Rotation matrices as the game's math library builds them (XNA math,
/// row vectors: a point p maps to p * M). rollPitchYaw applies roll about Z,
/// then pitch about X, then yaw about Y (XMMatrixRotationRollPitchYaw; the
/// executable's 0x82490C30 builds it from half-angle sines and cosines the
/// way XNA's quaternion form does).
glm::mat3 rotationX(float angle);
glm::mat3 rotationY(float angle);
glm::mat3 rollPitchYaw(float pitch, float yaw, float roll);
/// Rotation by `angle` about the unit vector `axis` (XMMatrixRotationNormal,
/// 0x82488D78 in the tyre code).
glm::mat3 rotationNormal(const glm::vec3& axis, float angle);
/// Row-vector product: p * M.
glm::vec3 transformRow(const glm::vec3& p, const glm::mat3& m);

/// The running instance of one effect (CParticleSystemNew): a ring of at
/// most `budget` particles, the emission rate accumulator and the adaptive
/// budget scale, updated as the game's particle manager does every frame.
class ParticleSystem {
public:
    ParticleSystem(const EffectDefinition& definition, std::uint32_t seed);

    const EffectDefinition& definition() const { return *m_definition; }

    /// Emits the particles due for this frame (0x82DC7090). `frameDt` is the
    /// time step of the manager's last update, which the rate is multiplied
    /// by. `crt` stands in for the C runtime's rand(), which the emitter
    /// shapes draw from while particle attributes use the system's own
    /// generator.
    void emit(const EmitRequest& request, float frameDt, GameRandom& crt);
    /// Advances particles by `dt` seconds (0x82416F58).
    void update(float dt, float rateScale);
    /// Runs the kept emitters for `dt` seconds, after update() (the emitter
    /// loop of 0x82416F58, each emitter through 0x82414440), drawing their
    /// shapes from `crt` as emit() does.
    void updateEmitters(float dt, GameRandom& crt);
    /// Clears the particles; the kept emitters stay.
    void clear();

    /// A kept emitter at `position` with `angles` for placed emitter `id`,
    /// with the effect's rate and emit times (0x82dc31f0 as 0x82dc3a60
    /// calls it).
    SystemEmitter makeEmitter(int id, const glm::vec3& position, const glm::vec3& angles) const;
    void addEmitter(const SystemEmitter& emitter);
    /// Whether an emitter of placed emitter `id` runs (0x82dc2298).
    bool hasEmitter(int id) const;
    /// Stops the emitters of placed emitter `id` (0x82dc2330): an infinite
    /// one goes at its next update; a timed one keeps running when
    /// `finishTime` is empty (a single effect's), and otherwise starts again
    /// and runs for `finishTime` seconds (a composite member's).
    void stopEmitters(int id, std::optional<float> finishTime);
    const std::vector<SystemEmitter>& emitters() const { return m_emitters; }

    std::size_t capacity() const { return m_capacity; }
    std::size_t size() const { return m_count; }
    /// The i-th particle of the ring, oldest first.
    const Particle& at(std::size_t i) const { return m_particles[(m_head + i) % m_capacity]; }
    /// The orientation the renderer builds for a Quad particle whose spin
    /// axis is not PerpToDirOfTravel: rotation about X by the spin angle,
    /// then roll/pitch/yaw by `axis` (0x82DC9274; order inferred from the
    /// matrix product there).
    static glm::mat3 orientation(const Particle& p);

    /// The emission scale the game lowers when a frame spawns more than the
    /// budget sustains (+376).
    float budgetScale() const { return m_budgetScale; }

private:
    Particle& slot(std::size_t i) { return m_particles[(m_head + i) % m_capacity]; }
    BubbleState& bubbleSlot(std::size_t i) { return m_bubble[(m_head + i) % m_capacity]; }
    void spawn(std::size_t first, std::size_t count, const EmitRequest& request, GameRandom& crt);
    /// Places `count` new particles from the effect's shape at `position`
    /// turned by `angles` and adds them; false for shapes not decoded.
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

} // namespace fh::particles
