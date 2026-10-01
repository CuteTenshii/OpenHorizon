#pragma once

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
} // namespace openhorizon

namespace openhorizon::particles {

/// Tyre effect channels in the order of the name table (0x824AB4A8) and the car presentation's systems (+0x1594).
/// SkidData/SmokeType weights them by name (absent is 0); the *2/*3 variants and Leaf6 have no effect on the disc.
enum class TyreChannel : std::size_t {
    Smoke,
    Dust,
    Grass,
    Dirt,
    Leaf1,
    Leaf2,
    Leaf3,
    Leaf4,
    Leaf5,
    Leaf6,
    GravelBits,
    Litter,
    DustClose,
    DustBumper,
    SmokeRim0,
    SmokeRim1,
    SmokeRim2,
    SmokeRim3,
    Dirt2,
    Dust2,
    DustClose2,
    DustBumper2,
    Dust3,
    DustClose3,
    DustBumper3,
};

inline constexpr std::size_t kTyreChannelCount = 25;

/// The system name (as in DefaultTrack.xml) of a channel.
std::string_view tyreChannelName(TyreChannel channel);

/// A surface type's `<SkidData>` in surfaceTypes.xml, read as the surface loader does (0x82F16820).
struct SurfaceEffects {
    std::string surfaceName;
    bool hasSkidData = false;
    /// Floor of the skid intensity while the car moves on this surface.
    float minIntensity = 0.0F;
    std::array<float, kTyreChannelCount> weights{};

    float weight(TyreChannel channel) const { return weights[static_cast<std::size_t>(channel)]; }
};

/// Indexed like the `.fiz` surface ids: the position in `<SurfaceTypesList>`.
class SurfaceEffectTable {
public:
    /// Throws DataError when it is not the surface table.
    static SurfaceEffectTable parse(std::string_view xml);
    /// Reads media/physics/surfaceTypes.xml. Throws DataError if missing.
    static SurfaceEffectTable load(const Vfs& vfs);

    /// nullptr for ids past the list.
    const SurfaceEffects* find(std::size_t id) const { return id < m_surfaces.size() ? &m_surfaces[id] : nullptr; }
    const std::vector<SurfaceEffects>& surfaces() const { return m_surfaces; }
    std::optional<std::size_t> idOf(std::string_view name) const;

private:
    std::vector<SurfaceEffects> m_surfaces;
};

} // namespace openhorizon::particles
