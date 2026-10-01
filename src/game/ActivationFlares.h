#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

namespace particles {
class ParticleManager;
} // namespace particles

/// The flares activation zones burn: the SimpleEmitter `<object>_flare` of
/// the track's ParticleEmitters.xml.
///
/// Verified: the flare is named `object` + "_flare" (0x8298b028-0x8298b050).
/// Its volume is a 500 m sphere around the zone (0x8298e9c8, 0x82f1b54c,
/// 0x8208c1d0); inside it the flare burns while the zone is enabled and not
/// hidden (on 0x8298b4f8, off 0x8298b5a0; 0x8298dcf0, 0x8298de68,
/// 0x8298dbe8, 0x8298b648); a new zone is not hidden (0x8298ae64).
/// Inferred: the subject is the player's car (0x8298ed64) and the centre the
/// zone object's position (0x8298b2b0), distance in 3D. Missing: what hides a
/// zone (0x8298b648, via 0x82504c50), so zones are never hidden.
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

} // namespace openhorizon
