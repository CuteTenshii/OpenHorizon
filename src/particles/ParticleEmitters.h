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

/// An emitter placed in a track's world by its ParticleEmitters.xml.
///
/// Verified in the loader (0x82dcc748, the part after DefaultTrack.xml): the
/// root `<ParticleEmitters>` holds `<TriggerableEmitter>`, `<SimpleEmitter>`
/// and `<TriggerZone>` elements, names compared ignoring case (0x82b3c7c0).
/// Both emitter kinds read `Name` and `Effect` (and a SimpleEmitter first its
/// `GameplayID`), then the placement (0x82dc4eb8): `Type` ("superspray",
/// "plane" or "box", any case; SuperSpray when none matches, the caller's
/// 2), `Position` with its z negated on load, `Orientation` as X, Y and Z
/// axis rows, and by type `Spray` (offAxisSpread, planeSpread), `Width` and
/// `Length`, or `Size`. An Effect starting `_Composite_` names a composite
/// effect (0x82cc0ae0), any other a single effect file; either is loaded by
/// name the first time it is named (0x82dcbd20 for effects, 0x82dcc048 for
/// composites), whether DefaultTrack.xml lists it or not.
struct PlacedEmitter {
    std::string name;
    /// The name gameplay code switches a SimpleEmitter by (the manager's map
    /// at +8692); empty for triggerable emitters.
    std::string gameplayId;
    std::string effect;
    EmitterShape type = EmitterShape::SuperSpray;
    /// World position: the file's with z negated (0x82dc5018), which puts
    /// the activation flares next to their zone objects in GameObjs.xml.
    glm::vec3 position{0.0F};
    /// The Orientation axes as the file stores them, one row per axis; the
    /// loader negates only the position's z.
    glm::mat3 axes{1.0F};
    /// Pitch, yaw and roll the emitter is kept with (see placementAngles()).
    glm::vec3 angles{0.0F};
    /// offAxisSpread and planeSpread. Read and stored with the placement
    /// (+48 of CSimpleEmitter) but not passed on to the emitters the effect
    /// runs (0x82dc3a60 drops that argument), so they change nothing.
    glm::vec2 spray{0.0F};
    float width = 0.0F;
    float length = 0.0F;
    glm::vec3 size{0.0F};

    bool composite() const { return isCompositeName(effect); }
};

/// A `<TriggerZone>`: a volume that switches the triggerable emitters it
/// lists (by Name) on and off. Read by 0x82dc4640 into a trigger volume of
/// the same kind the activities use (0x82dc3450 builds it with 0x82f1b420)
/// and switched through the particle presentation's TriggerEmitter and
/// UntriggerEmitter (vtable 0x82256574 +44 and +48, 0x82dc6658 and
/// 0x82dc2600).
struct EmitterTriggerZone {
    std::string name;
    /// "Sphere" for all but one "Box" and five with no type in colorado.
    std::string type;
    glm::vec3 position{0.0F};
    glm::mat3 axes{1.0F};
    float radius = 0.0F;
    glm::vec3 size{0.0F};
    float retriggerDelay = 0.0F;
    std::vector<std::string> emitters;
};

/// The pitch, yaw and roll 0x82dc4eb8 keeps a placed emitter's orientation
/// as: the angles whose rollPitchYaw() matrix has `axes` for rows.
///
/// Verified: for a SuperSpray, a Z axis with |x| and |z| below 1e-5
/// (0x82249240) gets z = 1e-5 (0x82063cf0) first, so a Z axis pointing
/// straight up still has a heading; then 0x82d79240 takes yaw = atan2(Z.x,
/// Z.z) and pitch = atan2(-Z.y, sqrt(Z.x^2 + Z.z^2)), which are the yaw and
/// pitch of rollPitchYaw's third row.
/// Inferred: the third angle, computed with VMX128 code the disassembler
/// does not decode, is the roll that completes the decomposition; and the
/// axes are used as read, the re-orthonormalisation before it (0x82d794c0,
/// also VMX128) leaving the file's orthonormal axes as they are.
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
