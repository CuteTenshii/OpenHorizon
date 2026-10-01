#include "ScatterSet.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>

namespace openhorizon::world {

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
/// `byte >> 1` of the group, and no copy at all when odd (see ScatterSet).
constexpr std::size_t kInstanceBlockSelector = 95;
constexpr std::size_t kBoundsMinOffset = 0x10;
constexpr std::size_t kBoundsMaxOffset = 0x20;
constexpr std::size_t kBillboardReachOffset = 0x38;
// The far tier's fields (see ScatterSet).
constexpr std::size_t kGroupTexture = 0;
constexpr std::size_t kGroupSecondTexture = 4;
constexpr std::size_t kGroupUFirst = 8;
constexpr std::size_t kGroupUSecond = 12;
constexpr std::size_t kGroupVFoot = 16;
constexpr std::size_t kGroupVTop = 20;
constexpr std::size_t kGroupWidth = 24;
constexpr std::size_t kGroupHeight = 28;
constexpr std::size_t kGroupColourScale = 32;
constexpr std::size_t kGroupType = 36;
constexpr std::size_t kGroupInstanceColour = 40;
constexpr std::size_t kGroupAlwaysDrawn = 76;
constexpr std::size_t kInstanceColour = 64;
constexpr std::size_t kInstanceGroundNormal = 80;
constexpr std::size_t kInstanceBakedLight = 92;
constexpr std::size_t kBlockFarEnd = 20;
constexpr std::int32_t kNoTexture = -1;
/// Half the card's width is taken (0x82000D38).
constexpr float kHalf = 0.5F;
/// The near end of a copy whose byte 95 is odd (0x82000CA8).
constexpr float kUncelledNearEnd = -1000.0F;
/// Byte colours become unit ones (1/255 at 0x82000D2C).
constexpr float kByteScale = 1.0F / 255.0F;

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
    std::int32_t i32(std::size_t offset) const { return static_cast<std::int32_t>(u32(offset)); }

private:
    ByteView m_data;
};

/// Stands in for the game's random edge swap, so every load looks the same.
bool mirrored(const std::string& setName, std::uint32_t copy)
{
    std::uint32_t hash = 2166136261U;
    const auto mix = [&hash](std::uint8_t byte) {
        hash ^= byte;
        hash *= 16777619U;
    };
    for (const char c : setName) {
        mix(static_cast<std::uint8_t>(c));
    }
    for (int shift = 0; shift < 32; shift += 8) {
        mix(static_cast<std::uint8_t>(copy >> shift));
    }
    return (hash & 1U) != 0;
}

glm::vec3 normalised(const glm::vec3& v)
{
    const float length = glm::length(v);
    return length > 0.0F ? v / length : v;
}

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
    offset = alignUp(offset, 4);
    for (std::uint32_t e = 0; e < extraCount; ++e) {
        const std::size_t entry = offset + static_cast<std::size_t>(e) * kExtraBytes;
        if (r.u32(entry + 4) != 0) {
            throw DataError("texture entry {} of the set carries data whose layout is not known", e);
        }
        set.textureRecords.push_back(r.u32(entry));
    }
    offset += static_cast<std::size_t>(extraCount) * kExtraBytes;
    set.boundsMin = r.vec3(kBoundsMinOffset);
    set.boundsMax = r.vec3(kBoundsMaxOffset);
    set.billboardReach = r.f32(kBillboardReachOffset);
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
        std::size_t record = 0;
        std::int32_t texture = kNoTexture;
        std::int32_t secondTexture = kNoTexture;
        std::uint32_t type = 0;
        std::uint32_t alwaysDrawn = 0;
        /// The first group merged with this one, and the merge's fade start.
        std::uint32_t lead = 0;
        float fadeStart = 0.0F;
    };
    std::vector<Group> groups(groupCount);
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        const std::size_t entry = offset + static_cast<std::size_t>(g) * kGroupBytes;
        Group& group = groups[g];
        group.mesh = r.u32(entry + kGroupMesh);
        group.count = r.u32(entry + kGroupCount);
        group.blocks = r.u32(entry + kGroupBlocks);
        group.drawDistance = r.f32(entry + kGroupDrawDistance);
        group.record = entry;
        group.texture = r.i32(entry + kGroupTexture);
        group.secondTexture = r.i32(entry + kGroupSecondTexture);
        group.type = r.u32(entry + kGroupType);
        group.alwaysDrawn = r.u32(entry + kGroupAlwaysDrawn);
        if (group.mesh >= meshCount || group.blocks > kMaxBlocks) {
            throw DataError("group {} of the set is not valid", g);
        }
    }
    // The groups the loader merges into one far-tier batch, and the
    // smallest draw distance among them (0x82ED6624-0x82ED66F8).
    for (std::uint32_t g = 0; g < groupCount; ++g) {
        Group& group = groups[g];
        group.lead = g;
        for (std::uint32_t h = 0; h < g; ++h) {
            const Group& other = groups[h];
            if (other.texture == group.texture && other.secondTexture == group.secondTexture && other.type == group.type
                && other.alwaysDrawn == group.alwaysDrawn) {
                group.lead = other.lead;
                break;
            }
        }
    }
    for (Group& group : groups) {
        Group& lead = groups[group.lead];
        if (group.alwaysDrawn == 0 && group.drawDistance > 0.0F
            && (lead.fadeStart <= 0.0F || group.drawDistance < lead.fadeStart)) {
            lead.fadeStart = group.drawDistance;
        }
    }
    offset = alignUp(offset + static_cast<std::size_t>(groupCount) * kGroupBytes, 16);

    std::uint32_t copyNumber = 0;
    for (const Group& group : groups) {
        const std::size_t instances = offset;
        const std::size_t blocks = offset + static_cast<std::size_t>(group.count) * kInstanceBytes;
        r.need(instances, static_cast<std::size_t>(group.count) * kInstanceBytes + group.blocks * kBlockBytes);
        offset = blocks + static_cast<std::size_t>(group.blocks) * kBlockBytes;
        if (group.texture >= 0 && static_cast<std::size_t>(group.texture) < set.textureRecords.size()) {
            const Group& lead = groups[group.lead];
            const std::size_t g = group.record;
            const float colourScale = r.f32(lead.record + kGroupColourScale);
            const bool instanceColour = r.u32(g + kGroupInstanceColour) != 0;
            for (std::uint32_t i = 0; i < group.count; ++i, ++copyNumber) {
                const std::size_t record = instances + static_cast<std::size_t>(i) * kInstanceBytes;
                r.need(record, kInstanceBytes);
                const std::uint8_t selector = data[record + kInstanceBlockSelector];
                const std::uint32_t block = selector >> 1U;
                if (block >= group.blocks) {
                    throw DataError("instance {} of the set picks block {} of {}", i, block, group.blocks);
                }
                const std::size_t distances = blocks + static_cast<std::size_t>(block) * kBlockBytes;
                const glm::vec3 xAxis = r.vec3(record);
                const glm::vec3 yAxis = r.vec3(record + kAxisStride);
                const glm::vec3 zAxis = r.vec3(record + 2 * kAxisStride);
                ScatterSet::Billboard card;
                card.position = r.vec3(record + kInstancePosition);
                card.up = normalised(yAxis);
                card.across = normalised(xAxis);
                card.groundNormal = r.vec3(record + kInstanceGroundNormal);
                card.halfWidth = r.f32(g + kGroupWidth) * glm::length(zAxis) * kHalf;
                card.height = r.f32(g + kGroupHeight) * glm::length(yAxis);
                card.nearEnd = (selector & 1U) != 0 ? kUncelledNearEnd : r.f32(distances + 8);
                card.fadeStart = lead.fadeStart;
                card.farEnd = r.f32(distances + kBlockFarEnd);
                const float uFirst = r.f32(g + kGroupUFirst);
                const float uSecond = r.f32(g + kGroupUSecond);
                // Types 4 and 5 keep the edges, 1 and 3 swap them, 0 and 2 at
                // random (0x82ED6CE8-0x82ED6D18).
                bool swapped = group.type == 1 || group.type == 3;
                if (group.type == 0 || group.type == 2) {
                    swapped = mirrored(set.name, copyNumber);
                }
                card.uMinus = swapped ? uFirst : uSecond;
                card.uPlus = swapped ? uSecond : uFirst;
                card.vFoot = r.f32(g + kGroupVFoot);
                card.vTop = r.f32(g + kGroupVTop);
                card.uStep = uSecond - uFirst;
                if (instanceColour) {
                    const glm::vec4 own{r.f32(record + kInstanceColour), r.f32(record + kInstanceColour + 4),
                        r.f32(record + kInstanceColour + 8), r.f32(record + kInstanceColour + 12)};
                    card.colour = glm::vec4(glm::vec3(own) * colourScale, 1.0F - own.w);
                } else {
                    card.colour = glm::vec4(glm::vec3(colourScale * kHalf), 1.0F);
                }
                card.bakedLight = glm::vec3(data[record + kInstanceBakedLight], data[record + kInstanceBakedLight + 1],
                                      data[record + kInstanceBakedLight + 2])
                    * kByteScale;
                card.texture = static_cast<std::uint32_t>(group.texture);
                card.type = group.type;
                set.billboards.push_back(card);
            }
        } else {
            copyNumber += group.count;
        }
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
            const std::uint8_t selector = data[record + kInstanceBlockSelector];
            // The game makes no cell for such a copy, so never draws it.
            if ((selector & 1U) != 0) {
                continue;
            }
            const std::uint32_t block = selector >> 1U;
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

} // namespace openhorizon::world
