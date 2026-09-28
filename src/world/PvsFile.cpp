#include "PvsFile.h"

#include <cstring>

namespace fh::world {

namespace {

constexpr std::size_t kZoneCountOffset = 0x20;
constexpr std::uint32_t kMaxZones = 4096;
/// Bytes between the zone table and the texture table vary; the table is
/// found by its contents within this window.
constexpr std::size_t kTextureTableSearch = 64;
constexpr std::size_t kTextureRecordBytes = 28;
constexpr std::size_t kDrawRecordBytes = 18;
constexpr std::size_t kObjectTrailerBytes = 60;
constexpr std::uint32_t kMaxShaderPathLength = 1024;
constexpr std::uint32_t kMaxObjects = 1'000'000;
constexpr std::uint32_t kOneFloat = 0x3F800000;

bool isTextureRecord(ByteView data, std::size_t offset, std::uint32_t number)
{
    return offset + kTextureRecordBytes <= data.size() && loadBe32(data.data() + offset + 4) == number
        && loadBe32(data.data() + offset + 8) == kOneFloat && loadBe32(data.data() + offset + 12) == kOneFloat;
}

} // namespace

PvsTables readPvs(ByteView pvs)
{
    if (pvs.size() < kZoneCountOffset + 4 || std::memcmp(pvs.data(), "FPVS", 4) != 0) {
        throw DataError("not a PVS file");
    }
    const std::uint32_t zones = loadBe32(pvs.data() + kZoneCountOffset);
    if (zones > kMaxZones) {
        throw DataError("implausible PVS zone count {}", zones);
    }
    const std::size_t zoneEnd = kZoneCountOffset + 4 + static_cast<std::size_t>(zones) * 4;

    std::size_t tableStart = 0;
    std::uint32_t recordCount = 0;
    for (std::size_t at = zoneEnd; at < zoneEnd + kTextureTableSearch && at + 4 <= pvs.size(); ++at) {
        const std::uint32_t n = loadBe32(pvs.data() + at);
        if (n == 0 || at + 4 + static_cast<std::size_t>(n) * kTextureRecordBytes > pvs.size()) {
            continue;
        }
        if (isTextureRecord(pvs, at + 4, 0)
            && isTextureRecord(pvs, at + 4 + static_cast<std::size_t>(n - 1) * kTextureRecordBytes, n - 1)) {
            tableStart = at;
            recordCount = n;
            break;
        }
    }
    if (recordCount == 0) {
        throw DataError("the PVS file has no texture table where one was expected");
    }
    PvsTables tables;
    tables.textureIds.resize(recordCount);
    for (std::uint32_t i = 0; i < recordCount; ++i) {
        const std::size_t record = tableStart + 4 + static_cast<std::size_t>(i) * kTextureRecordBytes;
        tables.textureIds[i] = loadBe32(pvs.data() + record);
    }

    BeReader r(pvs, "PVS file");
    r.seek(tableStart + 4 + static_cast<std::size_t>(recordCount) * kTextureRecordBytes);
    const std::uint32_t shaderCount = r.u32();
    for (std::uint32_t i = 0; i < shaderCount; ++i) {
        const std::uint32_t length = r.u32();
        if (length > kMaxShaderPathLength) {
            r.fail("implausible shader path length");
        }
        r.skip(length);
    }
    const std::uint32_t drawCount = r.u32();
    const ByteView draws = r.bytes(static_cast<std::size_t>(drawCount) * kDrawRecordBytes);
    tables.drawObjects.reserve(drawCount);
    for (std::uint32_t d = 0; d < drawCount; ++d) {
        tables.drawObjects.push_back(loadBe16(draws.data() + static_cast<std::size_t>(d) * kDrawRecordBytes));
    }
    const std::uint32_t objectCount = r.u32();
    if (objectCount > kMaxObjects) {
        r.fail(std::format("implausible render object count {}", objectCount));
    }
    tables.objectTextures.resize(objectCount);
    for (std::uint32_t o = 0; o < objectCount; ++o) {
        const std::uint32_t textureCount = r.u32();
        r.need(static_cast<std::size_t>(textureCount) * 4);
        std::vector<std::uint32_t>& textures = tables.objectTextures[o];
        textures.reserve(textureCount);
        for (std::uint32_t t = 0; t < textureCount; ++t) {
            const std::uint32_t record = r.u32();
            if (record >= recordCount) {
                r.fail(std::format("render object {} refers to texture record {} of {}", o, record, recordCount));
            }
            textures.push_back(tables.textureIds[record]);
        }
        const std::uint32_t shaderRefs = r.u32();
        r.skip(static_cast<std::size_t>(shaderRefs) * 4);
        r.skip(kObjectTrailerBytes);
    }
    return tables;
}

} // namespace fh::world
