#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {

namespace particles {
class ParticleManager;
} // namespace particles

/// The flares activation zones burn: the SimpleEmitter `<object>_flare` of
/// the track's ParticleEmitters.xml (every career event zone of colorado
/// has one, a `_Composite_Flare_N` effect placed a few metres from the
/// zone's object).
///
/// Verified in the executable: the zone's parser names the flare `object`
/// + "_flare" (0x8298b028-0x8298b050, kept at +20 of the zone). When the
/// zone builds its trigger volumes (0x8298e9c8) it adds a second one for
/// the flare: a sphere (type 1, whose radius is its first extent,
/// 0x82f1b54c) of 500 m (the vector 0x8208c1d0) around the zone, given the
/// same subject as the activation volume. Entering it sets +108 and
/// switches the flare on (0x8298b4f8, the particle presentation's
/// ActivateSimpleEmitter at vtable +52) if the zone is enabled (+192 bit 0)
/// and not hidden (+109); leaving it clears +108 and switches the flare off
/// (0x8298b5a0, DeactivateSimpleEmitter at +56) (0x8298dcf0, 0x8298de68).
/// Enabling the zone (0x8298dbe8) switches the flare on when the subject is
/// inside and the zone not hidden, and disabling it switches it off; hiding
/// (0x8298b648 with 1) switches it off and showing again (with 0) on while
/// the subject is inside. A new zone is not hidden (+109 = 1, 0x8298ae64).
///
/// Inferred: the subject both volumes are given (+15668 of the object
/// 0x8298ed64 finds) is the player's car, as the activation volume offers
/// the event to the player; the zone's centre (+64) is its object's
/// position, which 0x8298b2b0 copies from the object's matrix into it with
/// VMX128 code the disassembler does not decode. The distance is taken in
/// three dimensions, as a sphere's.
/// Missing: what hides a zone (0x8298b648 is reached only through the
/// activity's virtual at +96, 0x82504c50, whose callers were not found), so
/// zones here are never hidden.
class ActivationFlares {
public:
    /// The radius of the flare's volume (0x8208c1d0).
    static constexpr float kRadius = 500.0F;

    /// The GameplayID of the flare of the zone whose object is `object`.
    static std::string flareId(std::string_view object);

    /// Adds the flare of the zone of `object`, centred on `centre`,
    /// disabled. Returns its index.
    std::size_t add(std::string_view object, const glm::vec3& centre);
    /// Enables or disables a zone (0x8298dbe8).
    void setEnabled(std::size_t zone, bool enabled);
    /// Hides a zone or shows it again (0x8298b648).
    void setHidden(std::size_t zone, bool hidden);
    /// Moves the subject to `subject` (none: nothing is inside) and switches
    /// the flares whose state that changes.
    void update(const std::optional<glm::vec3>& subject, particles::ParticleManager& particles);

    std::size_t size() const { return m_zones.size(); }
    const std::string& id(std::size_t zone) const { return m_zones[zone].flareId; }
    /// Whether the zone has switched its flare on and not off since.
    bool burning(std::size_t zone) const { return m_zones[zone].burning; }

private:
    struct Zone {
        std::string flareId;
        glm::vec3 centre{0.0F};
        bool enabled = false;
        bool hidden = false;
        bool inside = false;
        bool burning = false;
    };

    std::vector<Zone> m_zones;
};

} // namespace fh
