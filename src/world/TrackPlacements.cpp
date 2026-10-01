#include "TrackPlacements.h"

#include "Archive.h"
#include "PvsFile.h"
#include "ScatterSet.h"

#include <lzx/zip.h>

#include <algorithm>
#include <limits>

namespace openhorizon::world {

namespace {

constexpr std::uint32_t kMaxZoneEntries = 1'000'000;
/// Bytes of a transform record before its variable part: 3 half floats,
/// 3 floats, 9 half floats and 16 zero bytes.
constexpr std::size_t kRecordFixedBytes = 52;
constexpr std::size_t kRecordPaddingBytes = 16;
constexpr std::size_t kBlockTrailerBytes = 32;
/// Block id of props that only an event or a script shows.
constexpr std::uint32_t kEventBlock = 0xFFFFFFFF;

/// Skips a section: a u32 count and `count * bytesPerEntry` bytes.
void skipSection(BeReader& r, std::size_t bytesPerEntry)
{
    const std::uint32_t count = r.u32();
    if (count > kMaxZoneEntries) {
        r.fail(std::format("implausible section size {}", count));
    }
    r.skip(static_cast<std::size_t>(count) * bytesPerEntry);
}

} // namespace

glm::vec3 Placement::apply(const glm::vec3& parsed) const
{
    // The parser negated the file's Z, so it is negated back here.
    const float x = parsed.x;
    const float y = parsed.y;
    const float z = -parsed.z;
    return {x * rows[0] + y * rows[3] + z * rows[6] + position.x,
        x * rows[1] + y * rows[4] + z * rows[7] + position.y, x * rows[2] + y * rows[5] + z * rows[8] + position.z};
}

bool Placement::belongsToEvent(std::string_view eventId) const
{
    if (eventId.empty() || !startsWithNoCase(eventTag, eventId)) {
        return false;
    }
    // "FR01" must not claim "FR010"'s props, only its own suffixed parts.
    return eventTag.size() == eventId.size() || eventTag[eventId.size()] == '_';
}

bool Placement::onRoute(int routeId) const
{
    return eventRoutes.empty() || std::find(eventRoutes.begin(), eventRoutes.end(), routeId) != eventRoutes.end();
}

std::vector<std::pair<std::uint32_t, Placement>> TrackPlacements::readZone(ByteView zone)
{
    BeReader r(zone, "zone file");
    const std::uint32_t drawCount = r.u32();
    if (drawCount > kMaxZoneEntries) {
        throw DataError("not a zone file");
    }
    const ByteView drawBytes = r.bytes(static_cast<std::size_t>(drawCount) * 4);
    std::vector<std::uint32_t> draws(drawCount);
    for (std::uint32_t k = 0; k < drawCount; ++k) {
        draws[k] = loadBe32(drawBytes.data() + static_cast<std::size_t>(k) * 4) & 0xFFFF;
    }
    skipSection(r, 2);
    skipSection(r, 16);
    skipSection(r, 4);
    skipSection(r, 1);
    skipSection(r, 2);
    skipSection(r, 4);
    skipSection(r, 1);
    skipSection(r, 16);
    skipSection(r, 1);
    const std::uint32_t recordCount = r.u32();
    if (recordCount != drawCount) {
        throw DataError("zone file has {} transform records for {} draws", recordCount, drawCount);
    }
    std::vector<std::pair<std::uint32_t, Placement>> placements;
    placements.reserve(drawCount);
    for (std::uint32_t k = 0; k < recordCount; ++k) {
        r.need(kRecordFixedBytes + 1);
        Placement placement;
        for (float& range : placement.ranges) {
            range = r.f16();
        }
        const float px = r.f32();
        const float py = r.f32();
        const float pz = r.f32();
        placement.position = glm::vec3(px, py, pz);
        for (float& value : placement.rows) {
            value = r.f16();
        }
        r.skip(kRecordPaddingBytes);
        const std::uint8_t blocks = r.u8();
        for (std::uint8_t b = 0; b < blocks; ++b) {
            const bool eventBlock = r.u32() == kEventBlock;
            const std::uint8_t length = r.u8();
            const ByteView bytes = r.bytes(length);
            if (eventBlock) {
                placement.eventRoutes.insert(placement.eventRoutes.end(), bytes.begin(), bytes.end());
                placement.eventProp = true;
            }
            const ByteView name = r.bytes(kBlockTrailerBytes);
            const auto end = std::find(name.begin(), name.end(), std::uint8_t{0});
            std::string& target = eventBlock ? placement.eventTag : placement.gameplayId;
            if (target.empty()) {
                target.assign(name.begin(), end);
            }
        }
        placements.emplace_back(draws[k], std::move(placement));
    }
    return placements;
}

std::optional<TrackPlacements> TrackPlacements::load(
    ByteView pvs, const lzx::ZipArchive& archive, const std::atomic<bool>* cancel)
{
    PvsTables tables = readPvs(pvs);
    TrackPlacements result;
    result.m_drawObjects = std::move(tables.drawObjects);
    result.m_placements.resize(result.m_drawObjects.size());
    result.m_zonesListing.resize(result.m_drawObjects.size());
    const auto cancelled = [cancel] { return cancel != nullptr && cancel->load(); };
    const auto& entries = archive.entries();

    const std::vector<std::size_t> zones = distinctEntries(archive, ".pvsz");
    std::vector<int> zoneNumbers;
    zoneNumbers.reserve(zones.size());
    for (const std::size_t entry : zones) {
        const std::optional<std::uint32_t> number = numberBefore(entries[entry].name, 'z', ".pvsz");
        zoneNumbers.push_back(
            number && *number <= std::numeric_limits<std::uint16_t>::max() ? static_cast<int>(*number) : -1);
    }
    using ZonePlacements = std::optional<std::vector<std::pair<std::uint32_t, Placement>>>;
    std::vector<ZonePlacements> parsed(zones.size());
    parallelFor(zones.size(), [&](std::size_t z) {
        if (cancelled()) {
            return;
        }
        try {
            parsed[z] = readZone(readEntry(archive, entries[zones[z]]));
        } catch (const DataError&) {
            parsed[z].reset();
        }
    });
    if (cancelled()) {
        return std::nullopt;
    }
    result.m_zones = static_cast<int>(zones.size());
    for (std::size_t z = 0; z < parsed.size(); ++z) {
        if (!parsed[z]) {
            ++result.m_failedZones;
            continue;
        }
        for (auto& [draw, placement] : *parsed[z]) {
            if (draw >= result.m_placements.size()) {
                continue;
            }
            if (zoneNumbers[z] >= 0) {
                result.m_zonesListing[draw].push_back(static_cast<std::uint16_t>(zoneNumbers[z]));
            }
            if (!result.m_placements[draw]) {
                result.m_placements[draw] = std::move(placement);
            }
        }
    }
    for (std::vector<std::uint16_t>& listing : result.m_zonesListing) {
        std::sort(listing.begin(), listing.end());
        listing.erase(std::unique(listing.begin(), listing.end()), listing.end());
    }

    // Only a set file's start is read, enough to tell its kind.
    const std::vector<std::size_t> sets = distinctEntries(archive, ".pgeo");
    struct ReadSet {
        bool isModelSet = false;
        std::optional<ScatterSet> set;
    };
    std::vector<ReadSet> readSets(sets.size());
    parallelFor(sets.size(), [&](std::size_t s) {
        if (cancelled()) {
            return;
        }
        ReadSet& read = readSets[s];
        const lzx::ZipEntry& entry = entries[sets[s]];
        try {
            read.isModelSet = scatterSetKind(readEntryPrefix(archive, entry, kScatterKindEnd)) == kModelSetKind;
            if (read.isModelSet) {
                read.set = readScatterSet(readEntry(archive, entry));
                read.set->entry = static_cast<std::uint32_t>(sets[s]);
            }
        } catch (const DataError&) {
            read.set.reset();
        }
    });
    if (cancelled()) {
        return std::nullopt;
    }
    result.m_scatterTemplates.assign(result.m_drawObjects.size(), false);
    for (ReadSet& read : readSets) {
        if (!read.isModelSet) {
            continue;
        }
        if (!read.set) {
            ++result.m_failedScatterSets;
            continue;
        }
        for (const std::vector<std::uint32_t>& draws : read.set->meshDraws) {
            for (const std::uint32_t draw : draws) {
                if (draw < result.m_scatterTemplates.size()) {
                    result.m_scatterTemplates[draw] = true;
                }
            }
        }
        result.m_scatterSets.push_back(std::move(*read.set));
    }
    return result;
}

TrackPlacements::TrackPlacements() = default;
TrackPlacements::~TrackPlacements() = default;
TrackPlacements::TrackPlacements(TrackPlacements&&) noexcept = default;
TrackPlacements& TrackPlacements::operator=(TrackPlacements&&) noexcept = default;

bool TrackPlacements::isScatterTemplate(std::size_t draw) const
{
    return draw < m_scatterTemplates.size() && m_scatterTemplates[draw];
}

std::size_t TrackPlacements::scatterInstanceCount() const
{
    std::size_t count = 0;
    for (const ScatterSet& set : m_scatterSets) {
        count += set.instances.size();
    }
    return count;
}

const std::vector<std::uint16_t>& TrackPlacements::zonesListing(std::size_t draw) const
{
    static const std::vector<std::uint16_t> kNone;
    return draw < m_zonesListing.size() ? m_zonesListing[draw] : kNone;
}

const Placement* TrackPlacements::placement(std::size_t draw) const
{
    if (draw >= m_placements.size()) {
        return nullptr;
    }
    const std::optional<Placement>& placement = m_placements[draw];
    return placement.has_value() ? &placement.value() : nullptr;
}

} // namespace openhorizon::world
