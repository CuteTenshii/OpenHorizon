#include "ActivityProps.h"

#include "game/GameObjects.h"
#include "render/WorldRenderer.h"
#include "world/WorldIndex.h"

#include "base/Bytes.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace fh {

namespace {

/// Distance on the ground plane from `point` to a chunk's bounds.
float groundDistance(const world::WorldChunk& chunk, const glm::vec3& point)
{
    const float dx = std::max({chunk.boundsMin.x - point.x, 0.0F, point.x - chunk.boundsMax.x});
    const float dz = std::max({chunk.boundsMin.z - point.z, 0.0F, point.z - chunk.boundsMax.z});
    return std::hypot(dx, dz);
}

} // namespace

ActivityProps::ActivityProps(const world::WorldIndex& index)
    : m_index(index)
{
    const std::vector<world::WorldChunk>& chunks = index.chunks();
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const world::WorldChunk& chunk = chunks[i];
        if (!chunk.placed) {
            continue;
        }
        const auto index32 = static_cast<std::uint32_t>(i);
        if (!chunk.placement.eventTag.empty()) {
            m_byName[toLower(chunk.placement.eventTag)].push_back(index32);
        }
        if (!chunk.placement.gameplayId.empty()) {
            m_byName[toLower(chunk.placement.gameplayId)].push_back(index32);
        }
    }
}

ActivityProps::~ActivityProps() = default;

std::vector<std::string> ActivityProps::zoneNames(std::string_view object, const GameObjects& objects)
{
    std::vector<std::string> names = {std::string(object), std::format("{}_L", object)};
    for (int i = 0;; ++i) {
        std::string numbered = std::format("{}_{:02}", object, i);
        if (objects.find(numbered) == nullptr) {
            break;
        }
        names.push_back(std::move(numbered));
    }
    return names;
}

std::size_t ActivityProps::addGroup(const std::vector<std::string>& names)
{
    Group group;
    for (const std::string& name : names) {
        const auto it = m_byName.find(toLower(name));
        if (it != m_byName.end()) {
            group.chunks.insert(group.chunks.end(), it->second.begin(), it->second.end());
        }
    }
    std::sort(group.chunks.begin(), group.chunks.end());
    group.chunks.erase(std::unique(group.chunks.begin(), group.chunks.end()), group.chunks.end());
    m_groups.push_back(std::move(group));
    return m_groups.size() - 1;
}

void ActivityProps::setShown(std::size_t group, bool shown)
{
    m_groups[group].shown = shown;
}

void ActivityProps::update(const glm::vec3& camera, render::WorldRenderer& renderer)
{
    const std::vector<world::WorldChunk>& chunks = m_index.chunks();
    for (Group& group : m_groups) {
        std::vector<std::uint32_t> drawn;
        if (group.shown) {
            for (const std::uint32_t c : group.chunks) {
                const float distance = groundDistance(chunks[c], camera);
                if (distance >= chunks[c].bandStart && distance < chunks[c].bandEnd) {
                    drawn.push_back(c);
                }
            }
        }
        if (drawn == group.drawn) {
            continue;
        }
        if (group.handle >= 0) {
            renderer.removeDynamic(group.handle);
            group.handle = -1;
        }
        if (!drawn.empty()) {
            group.handle = renderer.addDynamic(drawn);
        }
        group.drawn = std::move(drawn);
    }
}

void ActivityProps::clear(render::WorldRenderer& renderer)
{
    for (Group& group : m_groups) {
        if (group.handle >= 0) {
            renderer.removeDynamic(group.handle);
        }
        group.handle = -1;
        group.drawn.clear();
    }
}

std::size_t ActivityProps::chunksDrawn() const
{
    std::size_t count = 0;
    for (const Group& group : m_groups) {
        count += group.drawn.size();
    }
    return count;
}

} // namespace fh
