#pragma once

#include <glm/glm.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {

class Vfs;

/// One entry of a track's `Ribbon_00/GameObjs.xml`: a named gameplay
/// location such as a race start (`FR04`), a festival site, a barn find or a
/// speed camera.
struct GameObject {
    std::string id;
    glm::vec3 position{0.0F};
    /// Columns are the object's X, Y and Z axes in world space.
    glm::mat3 axes{1.0F};

    /// Heading of the Z axis, radians clockwise from north (+Z).
    float heading() const;
};

class GameObjects {
public:
    /// Reads `media/tracks/<track>/Ribbon_00/GameObjs.xml`. Throws DataError
    /// if it is missing or malformed.
    static GameObjects load(const Vfs& vfs, std::string_view track);
    /// A set of the given objects; for tests and tools.
    static GameObjects fromObjects(std::vector<GameObject> objects);

    const std::vector<GameObject>& all() const { return m_objects; }
    /// The object whose id matches exactly (ignoring case), if any.
    const GameObject* find(std::string_view id) const;

private:
    std::vector<GameObject> m_objects;
};

} // namespace fh
