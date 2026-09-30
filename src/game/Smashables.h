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

/// The breakable props of a track: signs, fences, marker poles, cones and
/// the like, which a car knocks over instead of stopping against.
///
/// `Ribbon_00/CollObjs.xml` places every such object (`PhysicsType` names the
/// render part, e.g. "CO_SignG_001.2.rmb", one entry per piece). Each
/// placement is matched by position to the placed render models of the world
/// index, so the object's models can leave the static world when it is hit.
/// All pieces at one position move as one object.
///
/// The discount signs ("flyers", type Flyer in SmashableObjectTypes.xml) are
/// not in CollObjs.xml. Each is one zone placement of a render model whose
/// record names the sign's GameObjs.xml GameplayID (see
/// world::Placement::gameplayId); those placements become bodies too, framed
/// by the placement's rotation so a sign standing at an angle gets a shape
/// along its board rather than along the world axes.
///
/// The game describes each object type's mass and collision shape in the
/// track's PhysicsDefinitions.bin, which is not decoded yet; here an
/// object's mass follows from the size of its models (about 60 kg per cubic
/// metre of bounding box, between 3 and 250 kg) and its shape is a row of
/// spheres along its longest side.
class Smashables {
public:
    struct Body {
        /// Chunks of the world index (every level of detail) that draw it.
        std::vector<std::uint32_t> chunks;
        /// The chunks of its finest level, drawn once it moves.
        std::vector<std::uint32_t> finestChunks;
        std::string type;
        /// The GameObjs.xml GameplayID the object stands for ("flyer_042"),
        /// or empty for plain props.
        std::string gameplayId;
        /// Where the body's frame is as placed: the centre of its bounds
        /// and, for a flyer, its placement's rotation (plain props start
        /// axis-aligned).
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

        /// Transform from the body's rest pose to where it is now, for
        /// drawing its models, which are stored in world space at rest.
        glm::mat4 displacement() const;
    };

    /// Reads the track's placements and matches them against the world
    /// index. Placements without a matching model are skipped. Throws
    /// DataError if CollObjs.xml is missing or malformed.
    static Smashables load(const Vfs& vfs, std::string_view track, const world::WorldIndex& index);
    /// As above, plus the discount signs, whose model bounds are read from
    /// the world's archive.
    static Smashables load(const Vfs& vfs, std::string_view track, const world::TrackWorld& world);

    const std::vector<Body>& bodies() const { return m_bodies; }
    std::size_t bodyCount() const { return m_bodies.size(); }
    /// One flag per world index chunk: true for chunks that belong to a
    /// breakable object, which static collision leaves out.
    const std::vector<bool>& chunkMask() const { return m_chunkMask; }

    /// Collides a car's body spheres (world centres and radii) with the
    /// objects near it, knocking loose the ones it hits. Returns the indices
    /// of bodies that came loose in this call.
    std::vector<std::size_t> collideCar(CarPhysics& car, const std::vector<glm::vec4>& carSpheres);
    /// Advances loose bodies by `dt` against the ground.
    void step(float dt, const GroundQuery& ground);
    /// Loose bodies, for the renderer.
    const std::vector<std::size_t>& looseBodies() const { return m_loose; }
    /// Puts every object back where it stood; returns the bodies that were
    /// loose.
    std::vector<std::size_t> reset();

    /// The body standing for GameObjs.xml object `gameplayId` (ignoring
    /// case), or nullptr.
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
