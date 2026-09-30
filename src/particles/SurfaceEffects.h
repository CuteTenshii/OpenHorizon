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

/// The tyre effect channels, in the order of the game's name table
/// (0x824AB4A8). A surface's SkidData/SmokeType element gives a weight for
/// each (attributes of these names; absent ones are 0), and the car's
/// presentation keeps one system pointer per channel in the same order
/// (presentation +0x1594 onwards), found by these names in DefaultTrack.xml.
/// Dirt2, Dust2, the *2/*3 variants and Leaf6 have no effect on the disc and
/// no surface gives them a weight.
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

/// What a surface type does to the tyre effects: `<SkidData>` of its entry
/// in `media/physics/surfaceTypes.xml`, read as the game's surface loader
/// reads it (0x82F16820): MinIntensity and the 25 SmokeType weights.
struct SurfaceEffects {
    std::string surfaceName;
    bool hasSkidData = false;
    /// Floor of the skid intensity while the car moves on this surface.
    float minIntensity = 0.0F;
    std::array<float, kTyreChannelCount> weights{};

    float weight(TyreChannel channel) const { return weights[static_cast<std::size_t>(channel)]; }
};

/// The SkidData of every surface type, indexed like the surface ids in the
/// `.fiz` collision meshes (the position in `<SurfaceTypesList>`).
class SurfaceEffectTable {
public:
    /// Parses surfaceTypes.xml. Throws DataError when it is not the surface
    /// table.
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
