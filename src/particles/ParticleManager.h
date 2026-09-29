#pragma once

#include "Effect.h"
#include "GameRandom.h"
#include "ParticleSystem.h"

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::particles {

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
    /// Advances every system by `dt` seconds and keeps `dt` for the
    /// emissions of the coming frame (the manager's +8152).
    void update(float dt);
    void clear();

    /// The manager's global rate scale (+8668, 1 unless the game lowers it).
    float rateScale() const { return m_rateScale; }
    void setRateScale(float scale) { m_rateScale = scale; }
    float frameDt() const { return m_frameDt; }

private:
    ParticleManager() = default;

    std::deque<EffectDefinition> m_definitions;
    std::vector<System> m_systems;
    std::vector<std::string> m_skipped;
    GameRandom m_crt{1};
    float m_frameDt = 0.0F;
    float m_rateScale = 1.0F;
};

} // namespace fh::particles
