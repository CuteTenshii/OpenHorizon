#pragma once

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openhorizon {
class GameObjects;
namespace render {
class WorldRenderer;
}
namespace world {
class WorldIndex;
}

/// The set dressing an activation zone shows while it is enabled: the
/// world's event props named after the zone's objects.
///
/// Verified in the executable: a zone's parser (0x8298af28) looks up, by
/// name, its object, `<object>_L`, `<object>_NODE` and `<object>_00`,
/// `<object>_01`, ... until a number is missing (0x8298b414); it hides the
/// NODE object for good (0x8258b720 with 1). Enabling a zone whose
/// is_activation flag is set (0x8298dbe8) shows the object, the `_L` object
/// and the numbered ones, and disabling it hides them again. The world's
/// event props carry those names (world::Placement::eventTag, and
/// Placement::gameplayId for some numbered ones): the career event FR04 has
/// its FR04 prop, FR04_01 to FR04_14 and gameplay objects FR04_00 and
/// FR04_16. The free-roam world leaves event props out, so these are drawn
/// here, as movable objects that do not move.
///
/// Inferred: the names are looked up among the GameObjs.xml objects (every
/// numbered name of a zone is there, some at the world origin with no prop).
/// Each prop is drawn at the level of detail whose distance band
/// (world::WorldChunk::bandStart/bandEnd) holds the camera's distance to the
/// prop's bounds on the ground plane, as the world's tiles choose theirs.
class ActivityProps {
public:
    explicit ActivityProps(const world::WorldIndex& index);
    ~ActivityProps();
    ActivityProps(const ActivityProps&) = delete;
    ActivityProps& operator=(const ActivityProps&) = delete;

    /// The names a zone with object `object` shows while enabled.
    static std::vector<std::string> zoneNames(std::string_view object, const GameObjects& objects);

    /// Shows the props named `names` as one group from now on. Returns the
    /// group's index.
    std::size_t addGroup(const std::vector<std::string>& names);
    /// Shows or hides a group, as enabling or disabling its zone does.
    void setShown(std::size_t group, bool shown);
    /// The world chunks a group draws, all levels of detail.
    const std::vector<std::uint32_t>& groupChunks(std::size_t group) const { return m_groups[group].chunks; }

    /// Picks each shown prop's level for a camera at `camera` and tells the
    /// renderer about groups whose drawn chunks changed.
    void update(const glm::vec3& camera, render::WorldRenderer& renderer);
    /// Removes everything from the renderer (before it goes).
    void clear(render::WorldRenderer& renderer);
    /// Chunks drawn after the last update.
    std::size_t chunksDrawn() const;

private:
    struct Group {
        std::vector<std::uint32_t> chunks;
        std::vector<std::uint32_t> drawn;
        bool shown = true;
        int handle = -1;
    };

    const world::WorldIndex& m_index;
    /// Placed chunks by their lower-case eventTag and gameplayId.
    std::unordered_map<std::string, std::vector<std::uint32_t>> m_byName;
    std::vector<Group> m_groups;
};

} // namespace openhorizon
