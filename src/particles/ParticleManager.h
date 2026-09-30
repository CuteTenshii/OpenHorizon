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

/// A SimpleEmitter of the track's ParticleEmitters.xml, which gameplay code
/// switches on and off by its GameplayID (CSimpleEmitter, 112 bytes, made by
/// 0x82dc9c28 for a single effect and 0x82dc9ce0 for a composite).
struct SimpleEmitterState {
    std::string gameplayId;
    /// The emitter's own id (+92, from a counter the constructor steps),
    /// which the emitters it starts carry.
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

/// The game's particle manager (CParticleSystemManagerNew) for the effects a
/// track loads: one running system per DefaultTrack.xml entry, found by its
/// system name, updated together once a frame.
class ParticleManager {
public:
    struct System {
        std::string name;
        std::unique_ptr<ParticleSystem> system;
    };

    /// Loads DefaultTrack.xml and each single effect it lists. Composites
    /// (which only start other effects) and entries whose file is missing
    /// are skipped and named in skipped(). Throws DataError when the list
    /// itself is missing.
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

    /// Emits from `system` with the time step of the last update(), as the
    /// game's emitters do.
    void emit(ParticleSystem& system, const EmitRequest& request);
    /// Advances every system that has particles or kept emitters by `dt`
    /// seconds, runs its kept emitters, and keeps `dt` for the emissions of
    /// the coming frame (the manager's +8152).
    void update(float dt);
    void clear();

    /// The system named `file`, loading media/effects/<file>.xml into a new
    /// one the first time, as 0x82dcbd20 does for the effects placed
    /// emitters and composites name; nullptr when the file is missing,
    /// unreadable or a composite.
    ParticleSystem* acquire(const Vfs& vfs, std::string_view file);
    /// The composite `file` with its member systems, loaded the first time
    /// (0x82dcc048); nullptr when its file is missing or unreadable.
    const CompositeSystems* acquireComposite(const Vfs& vfs, std::string_view file);

    /// Makes the SimpleEmitters of `set`, stopped, loading their effects
    /// (the SimpleEmitter part of 0x82dcc748). One whose effect cannot be
    /// loaded is left out and named in skipped(); of two with one
    /// GameplayID the first is kept, as the map insert (0x82c9abe8) keeps
    /// it.
    void placeSimpleEmitters(const Vfs& vfs, const ParticleEmitterSet& set);
    /// The SimpleEmitter with this GameplayID, or nullptr.
    const SimpleEmitterState* simpleEmitter(std::string_view gameplayId) const;
    const std::vector<SimpleEmitterState>& simpleEmitters() const { return m_simpleEmitters; }
    /// Switches a SimpleEmitter on (0x82dc66f8, the particle presentation's
    /// ActivateSimpleEmitter): it starts whether or not it runs already,
    /// which starts nothing new while its emitters are still going. False
    /// when there is no such emitter.
    bool activateSimpleEmitter(std::string_view gameplayId);
    /// Switches a SimpleEmitter off (0x82dc3578, DeactivateSimpleEmitter),
    /// stopping it if it runs. False when there is no such emitter.
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
