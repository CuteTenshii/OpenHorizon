#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
} // namespace fh

namespace fh::collision {

/// Game path of the surface table.
inline constexpr std::string_view kSurfaceTypesPath = "media/physics/surfaceTypes.xml";

/// Velocity-dependent friction: extra resistance that grows with the tyre's
/// sliding speed up to `velPeak` (m/s). The game gives soft surfaces large
/// coefficients (sand 5, slow concrete 10), so it works as the off-road drag
/// that slows a car on loose ground.
struct VelocityFriction {
    float velPeak = 0.0F;
    float lateralCoeff = 0.0F;
    float longitudinalCoeff = 0.0F;
};

/// The noise a surface adds to the ground under each tyre.
struct Bumpiness {
    /// "None", "Simplex", "Simplex4", "HeightGrid", "SinWave" or "Ridges".
    std::string noiseType = "None";
    float frequency = 0.0F;
    float clampMin = -1.0F;
    float clampMax = 1.0F;
    /// Height scale in metres; negative on some asphalt types.
    float scale = 0.0F;
    float offset = 0.0F;
    /// Controller rumble strength.
    float rumble = 0.0F;
};

/// A surface's suspension-like give (hay bales, tyre walls).
struct Springiness {
    bool springy = false;
    float springK = 0.0F;
    float compressionDamping = 0.0F;
    float reboundDamping = 0.0F;
};

/// One entry of `surfaceTypes.xml`. The id is the entry's position in
/// `<SurfaceTypesList>`, which is the byte `.fiz` triangles store; the
/// parameters come from the element of the same name under
/// `<SurfaceTypes>`.
///
/// Values the element leaves out keep the defaults below, which match the
/// file's asphalt grip and drag values. `hasParameters` is false for list
/// entries without an element (the list names "Kerb", which has none) and
/// for elements without a `<Friction>` block (CameraCollision and the
/// smashables).
struct SurfaceType {
    std::string name;
    /// Index into SurfaceTable::categories(); kNoCategory if the element is
    /// missing or names an unknown category.
    std::uint8_t category = 0xFF;
    bool hasParameters = false;

    /// Tyre grip multiplier for road cars: 1 on asphalt, 0.95 dirt, 0.85
    /// grass, 0.3 slow grass, 0 on invisible walls.
    float frictionScale = 1.0F;
    float heatTireScale = 1.0F;
    /// How much the off-road tyre model applies: 0 on paved surfaces, 1 on
    /// loose ones.
    float offRoadness = 0.0F;
    float offRoadWetFrictionScale = 1.0F;
    /// Slip angle (degrees) of peak grip on this surface when dry and wet.
    float offRoadDryPeakSlipAngle = 12.0F;
    float offRoadWetPeakSlipAngle = 12.0F;
    float rearGripMultiplier = 1.0F;
    float minimumArcadeGrip = 0.0F;
    float handbrakeGripMultiplier = 0.5F;
    VelocityFriction velocityFriction;
    /// Aerodynamic drag of bodies on or in the surface.
    float linearDrag = 0.03F;
    float angularDrag = 0.1F;
    Bumpiness bumpiness;
    Springiness springiness;
    float carDamageScalar = 1.0F;
    /// Driving on it counts as cutting the track in races.
    bool penalty = false;
    bool sparks = false;
    /// Colour of the game's collision debug view, RGBA.
    std::array<std::uint8_t, 4> debugColour{128, 128, 128, 255};
};

/// Contact response between two categories (`<CategoryToCategoryParams>`).
struct CategoryPair {
    std::string category0;
    std::string category1;
    float friction = 0.5F;
    /// Friction at low sliding speeds, where the file gives one.
    std::optional<float> lowSpeedFriction;
    /// Restitution; negative values are the file's own convention for car
    /// against world contacts.
    float elasticity = 0.1F;
};

/// The surface types and categories of `physics/surfaceTypes.xml`.
class SurfaceTable {
public:
    static constexpr std::uint8_t kNoCategory = 0xFF;

    /// Parses the file's text. Throws DataError if it is not XML or has no
    /// `<SurfaceTypesList>`.
    static SurfaceTable parse(std::string_view xml, const std::string& source = std::string(kSurfaceTypesPath));
    /// Reads kSurfaceTypesPath. Throws DataError if it is missing or bad.
    static SurfaceTable load(const Vfs& vfs);

    const std::vector<SurfaceType>& surfaces() const { return m_surfaces; }
    /// Category names in file order: Car, Cone, Tire, HardWorld, SoftWorld,
    /// RumbleStrip, TireWall, GuardRail, GenCollObj, Pylon, SoccerBall,
    /// NoCollision, Smashable on the retail disc.
    const std::vector<std::string>& categories() const { return m_categories; }
    const std::vector<CategoryPair>& categoryPairs() const { return m_pairs; }

    /// The surface with id `id`, or nullptr past the end of the list.
    const SurfaceType* find(std::size_t id) const { return id < m_surfaces.size() ? &m_surfaces[id] : nullptr; }
    /// The surface's name, or "surface<id>" for ids past the list.
    std::string nameOf(std::size_t id) const;
    /// The surface's category name, or "" if it has none.
    std::string_view categoryOf(std::size_t id) const;
    std::optional<std::size_t> idOf(std::string_view name) const;
    /// The parameters for contacts between two categories, in either order,
    /// falling back to the Default/Default pair; nullptr if neither exists.
    const CategoryPair* pair(std::string_view category0, std::string_view category1) const;

private:
    std::vector<SurfaceType> m_surfaces;
    std::vector<std::string> m_categories;
    std::vector<CategoryPair> m_pairs;
};

} // namespace fh::collision
