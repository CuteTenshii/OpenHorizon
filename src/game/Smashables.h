#pragma once

#include "car/CarPhysics.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {

class Vfs;

namespace world {
class WorldIndex;
struct TrackWorld;
} // namespace world

/// The breakable props of a track, placed by `Ribbon_00/CollObjs.xml` (one entry per piece; pieces at one position
/// move as one) and matched by position to the world index's models. Discount signs come from zone placements
/// (world::Placement::gameplayId) instead. Mass and shape are guessed: PhysicsDefinitions.bin is not decoded.
class Smashables {
public:
    struct Body {
        /// Chunks of the world index (every level of detail) that draw it.
        std::vector<std::uint32_t> chunks;
        /// The chunks of its finest level, drawn once it moves.
        std::vector<std::uint32_t> finestChunks;
        std::string type;
        /// GameObjs.xml GameplayID ("flyer_042"), empty for plain props.
        std::string gameplayId;
        /// The centre of its bounds and, for a flyer, its placement's rotation (plain props start axis-aligned).
        glm::vec3 restCentre{0.0F};
        glm::quat restOrientation{1.0F, 0.0F, 0.0F, 0.0F};
        /// Half the size of its bounds along the body's axes.
        glm::vec3 halfExtents{0.1F};
        float mass = 10.0F;
        /// Spheres in the body frame.
        std::vector<glm::vec4> spheres;

        bool loose = false;
        bool sleeping = false;
        float sleepTimer = 0.0F;
        glm::vec3 position{0.0F};
        glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
        glm::vec3 velocity{0.0F};
        glm::vec3 angularVelocity{0.0F};
        glm::vec3 inverseInertia{1.0F};

        /// From the rest pose to now; the models are stored in world space at rest.
        glm::mat4 displacement() const;
    };

    /// Placements without a matching model are skipped. Throws DataError if CollObjs.xml is missing or malformed.
    static Smashables load(const Vfs& vfs, std::string_view track, const world::WorldIndex& index);
    /// As above, plus the discount signs.
    static Smashables load(const Vfs& vfs, std::string_view track, const world::TrackWorld& world);

    const std::vector<Body>& bodies() const { return m_bodies; }
    std::size_t bodyCount() const { return m_bodies.size(); }
    /// Per world index chunk: true for breakable chunks, which static collision leaves out.
    const std::vector<bool>& chunkMask() const { return m_chunkMask; }

    /// `carSpheres` are world centres and radii. Returns the bodies knocked loose in this call.
    std::vector<std::size_t> collideCar(CarPhysics& car, const std::vector<glm::vec4>& carSpheres);
    void step(float dt, const GroundQuery& ground);
    const std::vector<std::size_t>& looseBodies() const { return m_loose; }
    /// Returns the bodies that were loose.
    std::vector<std::size_t> reset();

    /// Ignores case.
    const Body* findGameplayObject(std::string_view gameplayId) const;

private:
    void addFlyers(const world::TrackWorld& world);
    void addBody(Body body);
    void knockLoose(std::size_t index);
    std::vector<std::size_t> nearby(const glm::vec3& point, float radius) const;
    static std::uint64_t cellKey(int x, int z);

    std::vector<Body> m_bodies;
    std::vector<bool> m_chunkMask;
    std::vector<std::size_t> m_loose;
    /// Bodies by 16 m cell of their rest centre.
    std::unordered_map<std::uint64_t, std::vector<std::size_t>> m_cells;
};

} // namespace openhorizon
