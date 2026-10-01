#pragma once

#include "Effect.h"

#include <glm/mat3x3.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::particles {

/// An emitter placed by a track's ParticleEmitters.xml (loader 0x82dcc748, placement 0x82dc4eb8; element names
/// case-insensitive, 0x82b3c7c0). Unknown `Type` means SuperSpray. Verified.
/// Effects load by name on first use (0x82dcbd20, composites 0x82dcc048), listed in DefaultTrack.xml or not.
struct PlacedEmitter {
    std::string name;
    /// The name gameplay code switches a SimpleEmitter by (manager +8692); empty for triggerable emitters.
    std::string gameplayId;
    std::string effect;
    EmitterShape type = EmitterShape::SuperSpray;
    /// The file's position with z negated (0x82dc5018).
    glm::vec3 position{0.0F};
    /// The Orientation axes as stored, one row per axis; only the position's z is negated.
    glm::mat3 axes{1.0F};
    /// Pitch, yaw and roll the emitter is kept with (see placementAngles()).
    glm::vec3 angles{0.0F};
    /// offAxisSpread and planeSpread: stored (CSimpleEmitter +48) but unused, 0x82dc3a60 drops them.
    glm::vec2 spray{0.0F};
    float width = 0.0F;
    float length = 0.0F;
    glm::vec3 size{0.0F};

    /// An Effect starting `_Composite_` names a composite effect (0x82cc0ae0).
    bool composite() const { return isCompositeName(effect); }
};

/// A volume that switches the triggerable emitters it names on and off (read by 0x82dc4640, built by 0x82dc3450
/// as the activities' trigger volume, 0x82f1b420). Switched by TriggerEmitter / UntriggerEmitter (vtable
/// 0x82256574 +44 / +48: 0x82dc6658, 0x82dc2600).
struct EmitterTriggerZone {
    std::string name;
    /// "Sphere", "Box" or empty.
    std::string type;
    glm::vec3 position{0.0F};
    glm::mat3 axes{1.0F};
    float radius = 0.0F;
    glm::vec3 size{0.0F};
    float retriggerDelay = 0.0F;
    std::vector<std::string> emitters;
};

/// The pitch, yaw and roll (0x82dc4eb8, 0x82d79240) whose rollPitchYaw() matrix has `axes` for rows. Verified for
/// yaw and pitch, including a SuperSpray's near-vertical Z getting z = 1e-5 (0x82249240, 0x82063cf0).
/// Inferred: roll (VMX128 code), and that re-orthonormalising (0x82d794c0) leaves the file's axes unchanged.
glm::vec3 placementAngles(const glm::mat3& axes, EmitterShape type);

/// A track ribbon's ParticleEmitters.xml.
class ParticleEmitterSet {
public:
    /// Game path of ribbon `ribbon` of track `track` (the executable's
    /// "tracks\%s\Ribbon_%.2d\ParticleEmitters.xml", 0x82241afc).
    static std::string pathOf(std::string_view track, int ribbon = 0);
    /// Reads the file; an empty set when the track has none. Throws
    /// DataError when it is not a ParticleEmitters file.
    static ParticleEmitterSet load(const Vfs& vfs, std::string_view track, int ribbon = 0);
    /// Parses the file's text; `file` names it in errors.
    static ParticleEmitterSet parse(std::string_view xml, std::string_view file);

    const std::vector<PlacedEmitter>& simpleEmitters() const { return m_simple; }
    const std::vector<PlacedEmitter>& triggerableEmitters() const { return m_triggerable; }
    const std::vector<EmitterTriggerZone>& zones() const { return m_zones; }
    /// The SimpleEmitter with this GameplayID, or nullptr.
    const PlacedEmitter* findSimple(std::string_view gameplayId) const;
    /// How many placed emitters (both kinds) run each effect.
    std::map<std::string, std::size_t> countByEffect() const;

private:
    std::vector<PlacedEmitter> m_simple;
    std::vector<PlacedEmitter> m_triggerable;
    std::vector<EmitterTriggerZone> m_zones;
};

} // namespace openhorizon::particles
