#include "ActivationFlares.h"

#include "particles/ParticleManager.h"

#include <glm/geometric.hpp>

namespace fh {

std::string ActivationFlares::flareId(std::string_view object)
{
    return std::string(object) + "_flare";
}

std::size_t ActivationFlares::add(std::string_view object, const glm::vec3& centre)
{
    Zone zone;
    zone.flareId = flareId(object);
    zone.centre = centre;
    m_zones.push_back(std::move(zone));
    return m_zones.size() - 1;
}

void ActivationFlares::setEnabled(std::size_t zone, bool enabled)
{
    m_zones[zone].enabled = enabled;
}

void ActivationFlares::setHidden(std::size_t zone, bool hidden)
{
    m_zones[zone].hidden = hidden;
}

void ActivationFlares::update(const std::optional<glm::vec3>& subject, particles::ParticleManager& particles)
{
    // The executable switches a flare on the events listed in the class
    // comment, and each leaves it on exactly when the zone is enabled, not
    // hidden and has the subject inside, so that state is followed. The one
    // exception, showing a hidden zone again, switches the flare on without
    // looking at the enabled bit (0x8298b648); zones are never hidden here.
    for (Zone& zone : m_zones) {
        zone.inside = subject && glm::distance(*subject, zone.centre) < kRadius;
        const bool wanted = zone.enabled && !zone.hidden && zone.inside;
        if (wanted == zone.burning) {
            continue;
        }
        zone.burning = wanted;
        if (wanted) {
            particles.activateSimpleEmitter(zone.flareId);
        } else {
            particles.deactivateSimpleEmitter(zone.flareId);
        }
    }
}

} // namespace fh
