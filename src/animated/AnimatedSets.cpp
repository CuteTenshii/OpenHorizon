#include "AnimatedSets.h"

#include <algorithm>
#include <cstring>

namespace openhorizon::animated {

namespace {

constexpr std::size_t kKindOffset = 0x30;
constexpr std::size_t kKindEnd = 0x34;
constexpr std::size_t kNameLengthOffset = 0x3C;
constexpr std::size_t kEntryCountOffset = 0x40;
constexpr std::size_t kHeaderEntryCountOffset = 0x44;
constexpr std::size_t kFileSizeOffset = 0x48;
constexpr std::size_t kMeshCountOffset = 0x54;
constexpr std::size_t kEventFlagOffset = 0x58;
constexpr std::size_t kDataOffset = 0x60;
constexpr std::size_t kEventBlockBytes = 56;
constexpr std::size_t kEventTagOffset = 4;
constexpr std::size_t kEventTagBytes = 32;
constexpr std::size_t kEventRouteCountOffset = 36;
constexpr std::size_t kEventRoutesOffset = 37;
constexpr std::uint32_t kEventBlockId = 0xFFFFFFFF;
constexpr std::uint32_t kMaxNameLength = 256;
constexpr std::uint32_t kMaxEntries = 4096;

constexpr std::size_t kObjectHeaderBytes = 96;
constexpr std::size_t kGrannySizeOffset = 36;
constexpr std::size_t kGpuSizeOffset = 40;
constexpr std::size_t kObjectEntryCountOffset = 60;
constexpr std::size_t kStringCountOffset = 68;
constexpr std::size_t kEntryBytes = 60;
constexpr std::size_t kEntryBoneCount = 8;
constexpr std::size_t kEntrySlots = 12;
constexpr std::size_t kEntrySlotBytes = 16;
constexpr std::size_t kSlotBatchCount = 8;
constexpr int kLevels = 3;
constexpr std::size_t kBatchBytes = 68;
constexpr std::size_t kLightBytes = 20;
constexpr std::size_t kLightCount = 8;
constexpr std::size_t kLightPartBytes = 56;
constexpr std::size_t kDrawBytes = 44;
constexpr std::uint32_t kNoBone = 0xFFFFFFFF;
constexpr std::uint32_t kVertexTypes = 6;
constexpr std::uint32_t kMaxCount = 1U << 20;

constexpr std::size_t kPlacementHeaderBytes = 28;
constexpr std::size_t kPlacementBytes = 128;
constexpr std::size_t kRecordBytes = 64;
constexpr std::size_t kOverrideBytes = 8;

/// Bounds-checked big-endian reads at absolute offsets.
class Reader {
public:
    Reader(ByteView data, const std::string& source)
        : m_data(data)
        , m_source(source)
    {
    }

    void need(std::size_t offset, std::size_t bytes) const
    {
        if (offset > m_data.size() || bytes > m_data.size() - offset) {
            throw DataError("{}: truncated at offset 0x{:x}", m_source, offset);
        }
    }
    std::uint32_t u32(std::size_t offset) const
    {
        need(offset, 4);
        return loadBe32(m_data.data() + offset);
    }
    float f32(std::size_t offset) const { return std::bit_cast<float>(u32(offset)); }
    glm::vec3 vec3(std::size_t offset) const { return {f32(offset), f32(offset + 4), f32(offset + 8)}; }
    std::uint32_t count(std::size_t offset, const char* what) const
    {
        const std::uint32_t value = u32(offset);
        if (value > kMaxCount) {
            fail("implausible {} {}", what, value);
        }
        return value;
    }
    std::string text(std::size_t offset, std::size_t length) const
    {
        need(offset, length);
        const auto start = m_data.begin() + static_cast<std::ptrdiff_t>(offset);
        return {start, std::find(start, start + static_cast<std::ptrdiff_t>(length), std::uint8_t{0})};
    }
    template <typename... Args> [[noreturn]] void fail(std::format_string<Args...> format, Args&&... args) const
    {
        throw DataError("{}: {}", m_source, std::format(format, std::forward<Args>(args)...));
    }
    ByteView data() const { return m_data; }

private:
    ByteView m_data;
    const std::string& m_source;
};

std::size_t alignUp(std::size_t offset, std::size_t alignment)
{
    return (offset + alignment - 1) / alignment * alignment;
}

/// What the header parser (0x82ECBB08) reads before the body.
struct SetHeader {
    std::uint32_t kind = 0;
    std::string name;
    bool eventProp = false;
    std::string eventTag;
    std::vector<std::uint8_t> eventRoutes;
    /// (id, blob length) of the header entries.
    std::vector<std::uint32_t> headerEntries;
    /// Ids of the e entries.
    std::vector<std::uint32_t> entries;
    /// Where the body starts.
    std::size_t bodyOffset = 0;
};

SetHeader readHeader(const Reader& r)
{
    const ByteView data = r.data();
    if (data.size() < kDataOffset || std::memcmp(data.data(), "OEGP", 4) != 0) {
        r.fail("not a procedural set");
    }
    if (r.u32(kFileSizeOffset) != data.size()) {
        r.fail("the size field does not match the file");
    }
    if (r.u32(kMeshCountOffset) != 0) {
        r.fail("the set has mesh entries");
    }
    SetHeader header;
    header.kind = r.u32(kKindOffset);
    std::size_t offset = kDataOffset;
    if (r.u32(kEventFlagOffset) != 0) {
        if (r.u32(kDataOffset) != kEventBlockId) {
            r.fail("the event block is not one");
        }
        r.need(kDataOffset, kEventBlockBytes);
        header.eventTag = r.text(kDataOffset + kEventTagOffset, kEventTagBytes);
        const std::size_t routes = data[kDataOffset + kEventRouteCountOffset];
        if (kEventRoutesOffset + routes > kEventBlockBytes) {
            r.fail("the event block lists too many routes");
        }
        const auto first = data.begin() + static_cast<std::ptrdiff_t>(kDataOffset + kEventRoutesOffset);
        header.eventRoutes.assign(first, first + static_cast<std::ptrdiff_t>(routes));
        header.eventProp = true;
        offset += kEventBlockBytes;
    }
    const std::uint32_t nameLength = r.u32(kNameLengthOffset);
    if (nameLength > kMaxNameLength) {
        r.fail("implausible name length {}", nameLength);
    }
    header.name = r.text(offset, nameLength);
    offset += nameLength;

    const std::uint32_t headerEntries = r.u32(kHeaderEntryCountOffset);
    const std::uint32_t entries = r.u32(kEntryCountOffset);
    if (headerEntries > kMaxEntries || entries > kMaxEntries) {
        r.fail("implausible entry counts");
    }
    if (headerEntries > 0) {
        offset = alignUp(offset, 4);
        const std::size_t list = offset;
        offset += static_cast<std::size_t>(headerEntries) * 8;
        for (std::uint32_t i = 0; i < headerEntries; ++i) {
            header.headerEntries.push_back(r.u32(list + i * 8));
            offset += r.u32(list + i * 8 + 4);
        }
    }
    if (entries > 0) {
        offset = alignUp(offset, 4);
        const std::size_t list = offset;
        offset += static_cast<std::size_t>(entries) * 8;
        for (std::uint32_t i = 0; i < entries; ++i) {
            if (r.u32(list + i * 8 + 4) != 0) {
                r.fail("entry {} carries a record, which these kinds never do", i);
            }
            header.entries.push_back(r.u32(list + i * 8));
        }
    }
    header.bodyOffset = offset;
    r.need(offset, 0);
    return header;
}

/// A list of u32 lengths followed by that many texts, each padded with NULs.
std::vector<std::string> readStrings(const Reader& r, std::size_t& offset, std::uint32_t count)
{
    std::vector<std::string> strings;
    if (count == 0) {
        return strings;
    }
    offset = alignUp(offset, 4);
    std::vector<std::uint32_t> lengths(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        lengths[i] = r.count(offset + i * 4, "string length");
    }
    offset += static_cast<std::size_t>(count) * 4;
    for (const std::uint32_t length : lengths) {
        strings.push_back(r.text(offset, length));
        offset += length;
    }
    return strings;
}

AnimatedObjectDraw readDraw(const Reader& r, std::size_t offset)
{
    AnimatedObjectDraw draw;
    for (std::size_t k = 0; k < draw.textures.size(); ++k) {
        draw.textures[k] = static_cast<std::int32_t>(r.u32(offset + k * 4));
    }
    draw.firstTriangle = r.u32(offset + 16);
    draw.triangleCount = r.u32(offset + 20);
    for (std::size_t k = 0; k < draw.flags.size(); ++k) {
        draw.flags[k] = r.u32(offset + 24 + k * 4);
    }
    draw.specularPower = r.f32(offset + 36);
    draw.specularIntensity = r.f32(offset + 40);
    return draw;
}

} // namespace

std::uint32_t AnimatedObjectSet::vertexStride(std::uint32_t vertexType)
{
    static constexpr std::array<std::uint32_t, kVertexTypes> kStrides{24, 20, 20, 16, 16, 12};
    return vertexType < kStrides.size() ? kStrides[vertexType] : 0;
}

std::uint32_t setKind(ByteView prefix)
{
    if (prefix.size() < kKindEnd || std::memcmp(prefix.data(), "OEGP", 4) != 0) {
        return 0;
    }
    return loadBe32(prefix.data() + kKindOffset);
}

AnimatedObjectSet readObjectSet(Bytes data, const std::string& source)
{
    AnimatedObjectSet set;
    {
        const Reader r(data, source);
        const SetHeader header = readHeader(r);
        if (header.kind != kObjectSetKind) {
            r.fail("kind {} is not an animated object", header.kind);
        }
        if (!header.headerEntries.empty()) {
            r.fail("an animated object has header entries");
        }
        set.name = header.name;
        set.textureIds = header.entries;

        const std::size_t h = alignUp(header.bodyOffset, 16);
        r.need(h, kObjectHeaderBytes);
        set.boundsMin = r.vec3(h);
        set.boundsMax = r.vec3(h + 16);
        set.grannySize = r.u32(h + kGrannySizeOffset);
        const std::uint32_t gpuSize = r.u32(h + kGpuSizeOffset);
        const std::uint32_t entryCount = r.count(h + kObjectEntryCountOffset, "entry count");
        std::size_t offset = h + kObjectHeaderBytes;
        set.textureNames = readStrings(r, offset, r.count(h + kStringCountOffset, "string count"));

        offset = alignUp(offset, 4);
        const std::size_t entries = offset;
        offset += static_cast<std::size_t>(entryCount) * kEntryBytes;
        set.entries.resize(entryCount);
        // Where each level's batch records are, in parse order, for the
        // draws that follow all of them.
        struct BatchRecord {
            AnimatedObjectBatch* batch = nullptr;
            std::size_t offset = 0;
        };
        std::vector<BatchRecord> records;
        for (std::uint32_t e = 0; e < entryCount; ++e) {
            const std::size_t entry = entries + static_cast<std::size_t>(e) * kEntryBytes;
            AnimatedObjectEntry& out = set.entries[e];
            out.boneCount = r.u32(entry + kEntryBoneCount);
            for (int level = 0; level < kLevels; ++level) {
                const std::size_t slot = entry + kEntrySlots + static_cast<std::size_t>(level) * kEntrySlotBytes;
                const std::uint32_t count = r.count(slot + kSlotBatchCount, "batch count");
                if (count == 0) {
                    continue;
                }
                std::vector<AnimatedObjectBatch>& batches = out.lods[static_cast<std::size_t>(level)];
                batches.resize(count);
                offset = alignUp(offset, 4);
                const std::size_t first = offset;
                offset += static_cast<std::size_t>(count) * kBatchBytes;
                for (std::uint32_t b = 0; b < count; ++b) {
                    const std::size_t record = first + static_cast<std::size_t>(b) * kBatchBytes;
                    AnimatedObjectBatch& batch = batches[b];
                    const std::uint32_t bone = r.u32(record);
                    batch.bone = bone == kNoBone ? -1 : static_cast<int>(bone);
                    batch.vertexCount = r.count(record + 4, "vertex count");
                    batch.indexCount = r.count(record + 8, "index count");
                    batch.modelData = r.f32(record + 60);
                    batch.vertexType = r.u32(record + 64);
                    if (AnimatedObjectSet::vertexStride(batch.vertexType) == 0) {
                        r.fail("batch {} has vertex type {}", b, batch.vertexType);
                    }
                    batch.opaque.resize(r.count(record + 12, "draw count"));
                    batch.blended.resize(r.count(record + 16, "draw count"));
                    records.push_back({&batch, record});
                }
                for (std::uint32_t b = 0; b < count; ++b) {
                    const std::size_t record = first + static_cast<std::size_t>(b) * kBatchBytes;
                    batches[b].attachments = readStrings(r, offset, r.count(record + 24, "string count"));
                }
                for (std::uint32_t b = 0; b < count; ++b) {
                    const std::size_t record = first + static_cast<std::size_t>(b) * kBatchBytes;
                    const std::uint32_t lights = r.count(record + 28, "light count");
                    batches[b].lights = lights;
                    if (lights == 0) {
                        continue;
                    }
                    offset = alignUp(offset, 4);
                    const std::size_t lightList = offset;
                    offset += static_cast<std::size_t>(lights) * kLightBytes;
                    for (std::uint32_t l = 0; l < lights; ++l) {
                        const std::uint32_t parts = r.count(
                            lightList + static_cast<std::size_t>(l) * kLightBytes + kLightCount, "light parts");
                        offset = alignUp(offset, 4) + static_cast<std::size_t>(parts) * kLightPartBytes;
                    }
                }
            }
        }
        for (const BatchRecord& record : records) {
            for (std::vector<AnimatedObjectDraw>* draws : {&record.batch->opaque, &record.batch->blended}) {
                if (draws->empty()) {
                    continue;
                }
                offset = alignUp(offset, 4);
                for (AnimatedObjectDraw& draw : *draws) {
                    draw = readDraw(r, offset);
                    offset += kDrawBytes;
                    if (static_cast<std::uint64_t>(draw.firstTriangle) + draw.triangleCount
                        > record.batch->indexCount / 3) {
                        r.fail("a draw runs past its batch's indices");
                    }
                }
            }
        }
        offset = alignUp(offset, 4);
        set.grannyOffset = offset;
        r.need(offset, set.grannySize);
        offset = alignUp(offset + set.grannySize, 4);
        if (static_cast<std::uint64_t>(offset) + gpuSize != data.size()) {
            r.fail("the GPU block does not end the file");
        }
        for (const BatchRecord& record : records) {
            if (record.batch->indexCount > 0 && r.u32(record.offset + 52) != 0) {
                offset = alignUp(offset, 4);
                record.batch->indexOffset = offset;
                offset += static_cast<std::size_t>(record.batch->indexCount) * 2;
            }
        }
        for (const BatchRecord& record : records) {
            if (record.batch->vertexCount > 0 && r.u32(record.offset + 44) != 0) {
                offset = alignUp(offset, 4);
                record.batch->vertexOffset = offset;
                offset += static_cast<std::size_t>(record.batch->vertexCount)
                    * AnimatedObjectSet::vertexStride(record.batch->vertexType);
            }
        }
        if (offset > data.size()) {
            r.fail("the vertices run past the end of the file");
        }
    }
    set.data = std::move(data);
    return set;
}

AnimatedPlacementSet readPlacementSet(ByteView data, const std::string& source)
{
    const Reader r(data, source);
    const SetHeader header = readHeader(r);
    if (header.kind != kPlacementSetKind) {
        r.fail("kind {} is not a placement set", header.kind);
    }
    AnimatedPlacementSet set;
    set.name = header.name;
    set.eventProp = header.eventProp;
    set.eventTag = header.eventTag;
    set.eventRoutes = header.eventRoutes;

    const std::size_t h = alignUp(header.bodyOffset, 4);
    r.need(h, kPlacementHeaderBytes);
    const std::uint32_t count = r.count(h, "placement count");
    const std::uint32_t records = r.count(h + 4, "record count");
    std::size_t offset = alignUp(h + kPlacementHeaderBytes, 16);
    const std::size_t first = offset;
    offset += static_cast<std::size_t>(count) * kPlacementBytes;
    r.need(first, static_cast<std::size_t>(count) * kPlacementBytes);
    set.placements.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t p = first + static_cast<std::size_t>(i) * kPlacementBytes;
        AnimatedPlacement placement;
        for (std::size_t axis = 0; axis < 3; ++axis) {
            placement.axes[axis] = r.vec3(p + axis * 16);
        }
        placement.position = r.vec3(p + 48);
        if (r.f32(p + 60) != 1.0F) {
            r.fail("placement {} is not valid", i);
        }
        const std::uint32_t entry = r.u32(p + 64);
        if (entry >= header.headerEntries.size()) {
            r.fail("placement {} names header entry {} of {}", i, entry, header.headerEntries.size());
        }
        placement.objectSet = header.headerEntries[entry];
        placement.lodDistances = {r.f32(p + 68), r.f32(p + 72)};
        placement.cullDistance = r.f32(p + 76);
        placement.ownInstance = r.u32(p + 80) != 0;
        placement.triggerDistance = r.f32(p + 100);
        placement.nightThreshold = r.f32(p + 104);
        placement.dayThreshold = r.f32(p + 108);
        const std::uint32_t overrides = r.count(p + 112, "override count");
        if (overrides > 0) {
            offset = alignUp(offset, 4) + static_cast<std::size_t>(overrides) * kOverrideBytes;
        }
        set.placements.push_back(placement);
    }
    offset = alignUp(offset, 4) + static_cast<std::size_t>(records) * kRecordBytes;
    if (offset != data.size()) {
        r.fail("the placements end at 0x{:x}, the file at 0x{:x}", offset, data.size());
    }
    return set;
}

} // namespace openhorizon::animated
