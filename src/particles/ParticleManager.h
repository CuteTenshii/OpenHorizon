#pragma once

#include "Effect.h"
#include "GameRandom.h"
#include "ParticleSystem.h"

#include <glm/vec3.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::particles {

class ParticleEmitterSet;

/// A composite effect as the manager runs it: its file and a system for
/// each member effect, nullptr where the member's file is missing.
struct CompositeSystems {
    CompositeDefinition definition;
    std::vector<ParticleSystem*> members;
};

/// A SimpleEmitter, switched by gameplay through its GameplayID (CSimpleEmitter, 112 bytes; made by 0x82dc9c28,
/// composites 0x82dc9ce0).
struct SimpleEmitterState {
    std::string gameplayId;
    /// +92, from a counter the constructor steps; carried by the emitters it starts.
    int id = 0;
    glm::vec3 position{0.0F};
    glm::vec3 angles{0.0F};
    /// The single effect's system (+96) or the composite (+100).
    ParticleSystem* system = nullptr;
    const CompositeSystems* composite = nullptr;
    /// +108: switched on by gameplay.
    bool wanted = false;
    /// +109: started, until stopped.
    bool running = false;
};

/// CParticleSystemManagerNew: one system per DefaultTrack.xml entry, found by system name, updated once a frame.
class ParticleManager {
public:
    struct System {
        std::string name;
        std::unique_ptr<ParticleSystem> system;
    };

    /// Composites and entries whose file is missing go to skipped(). Throws DataError when the list is missing.
    static ParticleManager load(const Vfs& vfs);
    /// Builds systems for `entries` from already parsed definitions (matched
    /// by file name, case-insensitive).
    ParticleManager(std::vector<EffectDefinition> definitions, const std::vector<SystemEntry>& entries);

    ParticleManager(ParticleManager&&) noexcept = default;
    ParticleManager& operator=(ParticleManager&&) noexcept = default;
    ParticleManager(const ParticleManager&) = delete;
    ParticleManager& operator=(const ParticleManager&) = delete;
    ~ParticleManager() = default;

    /// The system of this DefaultTrack.xml system_name, or nullptr.
    ParticleSystem* find(std::string_view systemName);
    const ParticleSystem* find(std::string_view systemName) const;
    const std::vector<System>& systems() const { return m_systems; }
    const std::vector<std::string>& skipped() const { return m_skipped; }

    /// Emits with the time step of the last update(), as the game's emitters do.
    void emit(ParticleSystem& system, const EmitRequest& request);
    /// Advances busy systems by `dt` seconds and keeps `dt` for the next frame's emissions (manager +8152).
    void update(float dt);
    void clear();

    /// Loads media/effects/<file>.xml on first use (0x82dcbd20); nullptr when missing, unreadable or a composite.
    ParticleSystem* acquire(const Vfs& vfs, std::string_view file);
    /// The composite `file` with its member systems, loaded the first time
    /// (0x82dcc048); nullptr when its file is missing or unreadable.
    const CompositeSystems* acquireComposite(const Vfs& vfs, std::string_view file);

    /// Makes the SimpleEmitters of `set`, stopped (0x82dcc748). Unloadable effects go to skipped(); of two sharing
    /// a GameplayID the first is kept, as the map insert does (0x82c9abe8).
    void placeSimpleEmitters(const Vfs& vfs, const ParticleEmitterSet& set);
    /// The SimpleEmitter with this GameplayID, or nullptr.
    const SimpleEmitterState* simpleEmitter(std::string_view gameplayId) const;
    const std::vector<SimpleEmitterState>& simpleEmitters() const { return m_simpleEmitters; }
    /// ActivateSimpleEmitter (0x82dc66f8): starts even when running, which adds nothing while its emitters go on.
    /// False when there is no such emitter.
    bool activateSimpleEmitter(std::string_view gameplayId);
    /// DeactivateSimpleEmitter (0x82dc3578). False when there is no such emitter.
    bool deactivateSimpleEmitter(std::string_view gameplayId);
    /// The systems a SimpleEmitter's emitters run in.
    std::vector<const ParticleSystem*> systemsOf(const SimpleEmitterState& emitter) const;

    /// The manager's global rate scale (+8668, 1 unless the game lowers it).
    float rateScale() const { return m_rateScale; }
    void setRateScale(float scale) { m_rateScale = scale; }
    float frameDt() const { return m_frameDt; }

private:
    ParticleManager() = default;
    void start(SimpleEmitterState& emitter);
    void stop(SimpleEmitterState& emitter);

    std::deque<EffectDefinition> m_definitions;
    std::vector<System> m_systems;
    std::vector<std::string> m_skipped;
    std::deque<CompositeSystems> m_composites;
    std::vector<SimpleEmitterState> m_simpleEmitters;
    std::map<std::string, std::size_t, std::less<>> m_simpleByGameplayId;
    int m_nextEmitterId = 0;
    std::uint32_t m_nextSeed = 1;
    GameRandom m_crt{1};
    float m_frameDt = 0.0F;
    float m_rateScale = 1.0F;
};

} // namespace openhorizon::particles
