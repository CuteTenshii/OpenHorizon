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

/// The world's event props an activation zone shows while it is enabled,
/// which the free-roam world leaves out.
///
/// Verified: an is_activation zone shows `<object>`, `<object>_L` and
/// `<object>_00`, `_01`, ... up to the first missing number (0x8298af28,
/// 0x8298b414, 0x8298dbe8); `_NODE` stays hidden (0x8258b720).
/// Inferred: names are looked up among GameObjs.xml objects; a prop's level
/// of detail is picked by ground-plane distance, as the world tiles do.
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
