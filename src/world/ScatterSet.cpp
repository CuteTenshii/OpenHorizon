#include "ScatterSet.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace fh::world {

namespace {

constexpr std::size_t kKindOffset = 0x30;
constexpr std::size_t kNameLengthOffset = 0x3C;
constexpr std::size_t kExtraCountOffset = 0x40;
constexpr std::size_t kHeaderEntryCountOffset = 0x44;
constexpr std::size_t kFileSizeOffset = 0x48;
constexpr std::size_t kMeshCountOffset = 0x54;
constexpr std::size_t kEventBlockFlagOffset = 0x58;
constexpr std::size_t kDataOffset = 0x60;
constexpr std::size_t kEventBlockBytes = 56;
constexpr std::size_t kEventTagOffset = 4;
constexpr std::size_t kEventTagBytes = 32;
constexpr std::size_t kEventRouteCountOffset = 36;
constexpr std::size_t kEventRoutesOffset = 37;
constexpr std::uint32_t kEventBlockId = 0xFFFFFFFF;
constexpr std::uint32_t kMaxNameLength = 256;
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
/// The byte of an instance that picks its block of distances: block
/// `byte >> 1` of the group (see ScatterSet).
constexpr std::size_t kInstanceBlockSelector = 95;

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

/// The distances at which the levels of a `levels`-level mesh end (see
/// ScatterSet): each level ends where the next begins, and the last where
/// the block or the group stops drawing, whichever comes first.
std::array<float, 3> levelRanges(int levels, const std::array<float, 3>& ends, float drawDistance)
{
    std::array<float, 3> ranges{-1.0F, -1.0F, -1.0F};
    for (int level = 0; level + 1 < levels; ++level) {
        ranges[static_cast<std::size_t>(level)] = ends[static_cast<std::size_t>(level)];
    }
    ranges[static_cast<std::size_t>(std::max(levels, 1) - 1)] = std::min(ends[2], drawDistance);
    return ranges;
}

} // namespace

std::optional<std::uint32_t> scatterSetKind(ByteView prefix)
{
    if (prefix.size() < kScatterKindEnd || std::memcmp(prefix.data(), "OEGP", 4) != 0) {
        return std::nullopt;
    }
    return loadBe32(prefix.data() + kKindOffset);
}

ScatterSet readScatterSet(ByteView data)
{
    const Reader r(data);
    if (data.size() < kDataOffset || std::memcmp(data.data(), "OEGP", 4) != 0) {
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
    // None of Colorado's 2031 model sets has header entries (202 sets of
    // kind 5 do), whose own layout is not known.
    if (r.u32(kHeaderEntryCountOffset) != 0) {
        throw DataError("the set has header entries");
    }
    ScatterSet set;
    std::size_t nameOffset = kDataOffset;
    if (r.u32(kEventBlockFlagOffset) != 0) {
        if (r.u32(kDataOffset) != kEventBlockId) {
            throw DataError("the set's event block is not one");
        }
        r.need(kDataOffset, kEventBlockBytes);
        const auto tag = data.begin() + kDataOffset + kEventTagOffset;
        set.eventTag.assign(tag, std::find(tag, tag + kEventTagBytes, std::uint8_t{0}));
        const std::size_t routeCount = data[kDataOffset + kEventRouteCountOffset];
        if (kEventRoutesOffset + routeCount > kEventBlockBytes) {
            throw DataError("the set's event block lists too many routes");
        }
        const auto routes = data.begin() + kDataOffset + kEventRoutesOffset;
        set.eventRoutes.assign(routes, routes + static_cast<std::ptrdiff_t>(routeCount));
        set.eventProp = true;
        nameOffset += kEventBlockBytes;
    }
    const std::uint32_t nameLength = r.u32(kNameLengthOffset);
    if (nameLength > kMaxNameLength) {
        throw DataError("implausible name length {}", nameLength);
    }
    r.need(nameOffset, nameLength);
    const auto nameStart = data.begin() + static_cast<std::ptrdiff_t>(nameOffset);
    set.name.assign(nameStart, std::find(nameStart, nameStart + nameLength, std::uint8_t{0}));
    const std::size_t meshTable = alignUp(nameOffset + nameLength, 16);

    // Mesh entries: which carry draw records, and how many levels each has.
    std::vector<int> levels(meshCount, 0);
    std::vector<std::uint32_t> pathLengths(meshCount, 0);
    for (std::uint32_t m = 0; m < meshCount; ++m) {
        const std::size_t entry = meshTable + static_cast<std::size_t>(m) * kMeshBytes;
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
    std::size_t offset = meshTable + static_cast<std::size_t>(meshCount) * kMeshBytes;
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
        set.instances.reserve(set.instances.size() + group.count);
        for (std::uint32_t i = 0; i < group.count; ++i) {
            const std::size_t record = instances + static_cast<std::size_t>(i) * kInstanceBytes;
            if (r.f32(record + kInstancePosition + 12) != 1.0F) {
                throw DataError("instance {} of the set is not valid", i);
            }
            r.need(record + kInstanceBlockSelector, 1);
            const std::uint32_t block = data[record + kInstanceBlockSelector] >> 1U;
            if (block >= group.blocks) {
                throw DataError("instance {} of the set picks block {} of {}", i, block, group.blocks);
            }
            const std::size_t distances = blocks + static_cast<std::size_t>(block) * kBlockBytes;
            const std::array<float, 3> ranges = levelRanges(
                meshLevels, {r.f32(distances), r.f32(distances + 4), r.f32(distances + 8)}, group.drawDistance);
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
            instance.placement.eventProp = set.eventProp;
            instance.placement.eventTag = set.eventTag;
            instance.placement.eventRoutes = set.eventRoutes;
            set.instances.push_back(std::move(instance));
        }
    }
    return set;
}

} // namespace fh::world
