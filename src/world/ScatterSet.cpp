#include "ScatterSet.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fh::world {

namespace {

constexpr std::size_t kExtraCountOffset = 0x40;
constexpr std::size_t kFileSizeOffset = 0x48;
constexpr std::size_t kMeshCountOffset = 0x54;
constexpr std::size_t kNameOffset = 0x60;
constexpr std::size_t kNameBytes = 0x1C;
constexpr std::size_t kMeshTableOffset = 0x80;
constexpr std::size_t kMeshBytes = 64;
constexpr std::size_t kMeshPathLength = 32;
constexpr std::size_t kMeshSlots = 36;
constexpr std::size_t kMeshSlotBytes = 8;
constexpr int kSlotCount = 3;
constexpr std::size_t kExtraBytes = 8;
constexpr std::size_t kGroupPreambleBytes = 20;
constexpr std::size_t kGroupCountInPreamble = 12;
constexpr std::size_t kGroupBytes = 92;
constexpr std::size_t kGroupMesh = 44;
constexpr std::size_t kGroupCount = 48;
constexpr std::size_t kGroupBlocks = 56;
constexpr std::size_t kGroupDrawDistance = 60;
constexpr std::size_t kInstanceBytes = 96;
constexpr std::size_t kAxisStride = 16;
constexpr std::size_t kInstancePosition = 48;
constexpr std::size_t kBlockBytes = 32;
constexpr std::uint32_t kMaxMeshes = 1024;
constexpr std::uint32_t kMaxGroups = 1024;
constexpr std::uint32_t kMaxExtra = 4096;
constexpr std::uint32_t kMaxPathLength = 1024;
constexpr std::uint32_t kMaxBlocks = 16;
/// Used when a group has no block of switch distances: its levels switch at
/// these fractions of its draw distance. A viewer choice, not game data.
constexpr float kFallbackFirstSwitch = 0.25F;
constexpr float kFallbackSecondSwitch = 0.5F;

/// Bounds-checked reads at absolute offsets.
class Reader {
public:
    explicit Reader(ByteView data)
        : m_data(data)
    {
    }

    void need(std::size_t offset, std::size_t bytes) const
    {
        if (offset > m_data.size() || bytes > m_data.size() - offset) {
            throw DataError("the set is truncated at offset 0x{:x}", offset);
        }
    }
    std::uint32_t u32(std::size_t offset) const
    {
        need(offset, 4);
        return loadBe32(m_data.data() + offset);
    }
    float f32(std::size_t offset) const { return std::bit_cast<float>(u32(offset)); }
    glm::vec3 vec3(std::size_t offset) const { return {f32(offset), f32(offset + 4), f32(offset + 8)}; }

private:
    ByteView m_data;
};

std::size_t alignUp(std::size_t offset, std::size_t alignment)
{
    return (offset + alignment - 1) / alignment * alignment;
}

/// The distances at which the levels of a `levels`-level mesh end: each
/// level ends where the next begins and the last where drawing stops.
std::array<float, 3> levelRanges(int levels, float firstSwitch, float secondSwitch, float drawDistance)
{
    std::array<float, 3> ranges{-1.0F, -1.0F, -1.0F};
    const std::array<float, 2> switches{firstSwitch, secondSwitch};
    for (int level = 0; level + 1 < levels; ++level) {
        ranges[static_cast<std::size_t>(level)] = switches[static_cast<std::size_t>(level)];
    }
    ranges[static_cast<std::size_t>(std::max(levels, 1) - 1)] = drawDistance;
    return ranges;
}

} // namespace

ScatterSet readScatterSet(ByteView data)
{
    const Reader r(data);
    if (data.size() < kMeshTableOffset || std::memcmp(data.data(), "OEGP", 4) != 0) {
        throw DataError("not a procedural placement set");
    }
    if (r.u32(kFileSizeOffset) != data.size()) {
        throw DataError("the set's size field does not match its size");
    }
    const std::uint32_t meshCount = r.u32(kMeshCountOffset);
    const std::uint32_t extraCount = r.u32(kExtraCountOffset);
    if (meshCount == 0 || meshCount > kMaxMeshes || extraCount > kMaxExtra) {
        throw DataError("implausible mesh or entry counts");
    }
    ScatterSet set;
    const auto nameStart = data.begin() + kNameOffset;
    const auto nameEnd = std::find(nameStart, nameStart + kNameBytes, std::uint8_t{0});
    set.name.assign(nameStart, nameEnd);

    // Mesh entries: which carry draw records, and how many levels each has.
    std::vector<int> levels(meshCount, 0);
    std::vector<std::uint32_t> pathLengths(meshCount, 0);
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        const std::size_t entry = kMeshTableOffset + static_cast<std::size_t>(m) * kMeshBytes;
        pathLengths[m] = r.u32(entry + kMeshPathLength);
        if (pathLengths[m] > kMaxPathLength) {
            throw DataError("mesh {} has an implausible path length", m);
        }
        if (pathLengths[m] == 0) {
            while (levels[m] < kSlotCount
                && r.u32(entry + kMeshSlots + kMeshSlotBytes * static_cast<std::size_t>(levels[m])) == 1) {
                ++levels[m];
            }
            if (levels[m] == 0) {
                throw DataError("mesh {} has neither levels of detail nor a path", m);
            }
        }
    }
    std::size_t offset = kMeshTableOffset + static_cast<std::size_t>(meshCount) * kMeshBytes;
    set.meshDraws.resize(meshCount);
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        for (int level = 0; level < levels[m]; ++level) {
            set.meshDraws[m].push_back(r.u32(offset));
            offset += 4;
        }
    }
    for (const std::uint32_t length : pathLengths) {
        offset += length;
    }
    offset = alignUp(offset, 4) + static_cast<std::size_t>(extraCount) * kExtraBytes;
    const std::uint32_t groupCount = r.u32(offset + kGroupCountInPreamble);
    offset += kGroupPreambleBytes;
    if (groupCount > kMaxGroups) {
        throw DataError("implausible group count {}", groupCount);
    }
    r.need(offset, static_cast<std::size_t>(groupCount) * kGroupBytes);

    struct Group {
        std::uint32_t mesh = 0;
        std::uint32_t count = 0;
        std::uint32_t blocks = 0;
        float drawDistance = 0.0F;
    };
    std::vector<Group> groups(groupCount);
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        const std::size_t entry = offset + static_cast<std::size_t>(g) * kGroupBytes;
        groups[g] = {r.u32(entry + kGroupMesh), r.u32(entry + kGroupCount), r.u32(entry + kGroupBlocks),
            r.f32(entry + kGroupDrawDistance)};
        if (groups[g].mesh >= meshCount || groups[g].blocks > kMaxBlocks) {
            throw DataError("group {} of the set is not valid", g);
        }
    }
    offset = alignUp(offset + static_cast<std::size_t>(groupCount) * kGroupBytes, 16);

    for (const Group& group : groups) {
        const std::size_t instances = offset;
        const std::size_t blocks = offset + static_cast<std::size_t>(group.count) * kInstanceBytes;
        r.need(instances, static_cast<std::size_t>(group.count) * kInstanceBytes + group.blocks * kBlockBytes);
        offset = blocks + static_cast<std::size_t>(group.blocks) * kBlockBytes;
        const int meshLevels = levels[group.mesh];
        if (meshLevels == 0 || group.count == 0 || !(group.drawDistance > 0.0F)) {
            continue;
        }
        // Blocks that switch no level hold zeros; the first that does gives
        // the switch distances.
        float firstSwitch = group.drawDistance * kFallbackFirstSwitch;
        float secondSwitch = group.drawDistance * kFallbackSecondSwitch;
        for (std::uint32_t b = 0; b < group.blocks; ++b) {
            const float first = r.f32(blocks + b * kBlockBytes);
            const float second = r.f32(blocks + b * kBlockBytes + 4);
            if (first > 0.0F && second >= first && second <= group.drawDistance) {
                firstSwitch = first;
                secondSwitch = second;
                break;
            }
        }
        const std::array<float, 3> ranges = levelRanges(meshLevels, firstSwitch, secondSwitch, group.drawDistance);
        set.instances.reserve(set.instances.size() + group.count);
        for (std::uint32_t i = 0; i < group.count; ++i) {
            const std::size_t record = instances + static_cast<std::size_t>(i) * kInstanceBytes;
            if (r.f32(record + kInstancePosition + 12) != 1.0F) {
                throw DataError("instance {} of the set is not valid", i);
            }
            ScatterSet::Instance instance;
            instance.mesh = group.mesh;
            instance.placement.position = r.vec3(record + kInstancePosition);
            // The axes form a proper rotation; placements map the model's
            // stored coordinates, whose Z axis points the other way.
            const glm::vec3 x = r.vec3(record);
            const glm::vec3 y = r.vec3(record + kAxisStride);
            const glm::vec3 z = -r.vec3(record + 2 * kAxisStride);
            instance.placement.rows = {x.x, x.y, x.z, y.x, y.y, y.z, z.x, z.y, z.z};
            instance.placement.ranges = ranges;
            set.instances.push_back(std::move(instance));
        }
    }
    return set;
}

} // namespace fh::world
