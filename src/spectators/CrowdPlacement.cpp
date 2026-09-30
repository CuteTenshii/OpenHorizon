#include "CrowdPlacement.h"

#include <glm/geometric.hpp>

#include <cmath>
#include <cstring>
#include <numbers>

namespace fh::spectators {

namespace {

constexpr std::size_t kTypeOffset = 0x30;
constexpr std::size_t kNameLengthOffset = 0x3C;
constexpr std::size_t kFileSizeOffset = 0x48;
constexpr std::size_t kEventFlagOffset = 0x58;
constexpr std::size_t kEventBlockOffset = 0x60;
constexpr std::size_t kEventBlockBytes = 0x38;
constexpr std::size_t kEventTagBytes = 32;
constexpr std::size_t kMaxRoutes = kEventBlockBytes - 4 - kEventTagBytes - 1;
constexpr std::uint32_t kEventMarker = 0xFFFFFFFF;
constexpr std::size_t kGroupHeaderBytes = 64;
constexpr std::size_t kGroupCountOffset = 32;
constexpr std::size_t kRecordBytes = 12;
constexpr std::size_t kPathCountOffset = 40;
constexpr std::size_t kPathEntryBytes = 12;
constexpr std::size_t kSegmentBytes = 96;
constexpr std::size_t kSegmentPointBytes = 16;
constexpr std::size_t kSegmentDistances = 8;
constexpr std::uint32_t kMaxPaths = 4096;
constexpr std::uint32_t kMaxSegments = 65536;
/// Walking order of a segment's stored points.
constexpr std::array<std::size_t, 3> kPointOrder {1, 0, 2};
constexpr std::uint32_t kMaxNameLength = 1024;
constexpr float kQuantumSteps = 65535.0F;
/// The game's person decoder (0x82EB99E0) turns the heading byte into
/// b / 255 of a full turn.
constexpr float kHeadingSteps = 255.0F;

glm::vec3 readVec3(BeReader& r)
{
    const float x = r.f32();
    const float y = r.f32();
    const float z = r.f32();
    return {x, y, z};
}

std::vector<WalkPath> readWalkPaths(BeReader& r, std::size_t group)
{
    r.seek(group + kPathCountOffset);
    const std::uint32_t count = r.u32();
    if (count > kMaxPaths) {
        r.fail(std::format("implausible path count {}", count));
    }
    std::vector<WalkPath> paths(count);
    r.seek(group + kGroupHeaderBytes);
    for (WalkPath& path : paths) {
        r.skip(8);
        const std::uint32_t crowdClass = r.u32();
        if (crowdClass > 0xFF) {
            r.fail(std::format("implausible crowd class {}", crowdClass));
        }
        path.crowdClass = static_cast<std::uint8_t>(crowdClass);
    }
    r.align(16);
    for (WalkPath& path : paths) {
        path.boundsMin = readVec3(r);
        r.skip(4);
        path.boundsMax = readVec3(r);
        r.skip(8);
        const std::uint32_t segments = r.u32();
        if (segments == 0 || segments > kMaxSegments) {
            r.fail(std::format("implausible segment count {}", segments));
        }
        path.length = r.f32();
        r.skip(4);
        r.need(static_cast<std::size_t>(segments) * kSegmentBytes);
        for (std::uint32_t s = 0; s < segments; ++s) {
            const std::size_t segment = r.pos();
            std::array<glm::vec3, 3> points {};
            for (glm::vec3& point : points) {
                point = readVec3(r);
                r.skip(4);
            }
            for (const std::size_t k : kPointOrder) {
                path.points.push_back(points[k]);
            }
            r.seek(segment + 3 * kSegmentPointBytes + kSegmentPointBytes);
            for (std::size_t d = 0; d < kSegmentDistances; ++d) {
                path.distances.push_back(r.f32());
            }
        }
    }
    return paths;
}

} // namespace

float WalkPath::loopLength() const
{
    float total = 0.0F;
    for (std::size_t i = 0; i < points.size(); ++i) {
        total += glm::length(points[(i + 1) % points.size()] - points[i]);
    }
    return total;
}

glm::vec3 WalkPath::pointAt(float distance, glm::vec3* direction) const
{
    if (points.empty()) {
        return glm::vec3(0.0F);
    }
    const float total = loopLength();
    float remaining = total > 0.0F ? std::fmod(distance, total) : 0.0F;
    if (remaining < 0.0F) {
        remaining += total;
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        const glm::vec3& from = points[i];
        const glm::vec3& to = points[(i + 1) % points.size()];
        const float step = glm::length(to - from);
        if (remaining <= step || i + 1 == points.size()) {
            if (direction) {
                glm::vec3 flat(to.x - from.x, 0.0F, to.z - from.z);
                *direction = glm::length(flat) > 0.0F ? glm::normalize(flat) : glm::vec3(0.0F, 0.0F, 1.0F);
            }
            return step > 0.0F ? from + (to - from) * std::min(remaining / step, 1.0F) : from;
        }
        remaining -= step;
    }
    return points.front();
}

float PlacedSpectator::headingRadians() const
{
    return static_cast<float>(heading) * (2.0F * std::numbers::pi_v<float> / kHeadingSteps);
}

std::string CrowdPlacementSet::eventId() const
{
    const std::size_t underscore = eventTag.rfind('_');
    return underscore == std::string::npos ? eventTag : eventTag.substr(0, underscore);
}

std::optional<std::uint32_t> placementSetType(ByteView prefix)
{
    if (prefix.size() < kPlacementSetPrefixBytes || std::memcmp(prefix.data(), "OEGP", 4) != 0) {
        return std::nullopt;
    }
    return loadBe32(prefix.data() + kTypeOffset);
}

CrowdPlacementSet parseCrowdPlacementSet(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    if (placementSetType(data) != kCrowdSetType) {
        r.fail("not a crowd placement set");
    }
    r.seek(kFileSizeOffset);
    if (r.u32() != data.size()) {
        r.fail("the size field does not match the file");
    }
    r.seek(kNameLengthOffset);
    const std::uint32_t nameLength = r.u32();
    if (nameLength == 0 || nameLength > kMaxNameLength) {
        r.fail(std::format("implausible name length {}", nameLength));
    }

    CrowdPlacementSet set;
    r.seek(0x10);
    set.boundsMin = readVec3(r);
    r.skip(4);
    set.boundsMax = readVec3(r);

    r.seek(kEventFlagOffset);
    set.hasEventBlock = r.u32() != 0;
    std::size_t nameOffset = kEventBlockOffset;
    if (set.hasEventBlock) {
        r.seek(kEventBlockOffset);
        r.expect(kEventMarker, "event block marker");
        set.eventTag = r.fixedString(kEventTagBytes);
        const std::uint8_t routes = r.u8();
        if (routes > kMaxRoutes) {
            r.fail(std::format("implausible route count {}", routes));
        }
        const ByteView ids = r.bytes(routes);
        set.routes.assign(ids.begin(), ids.end());
        nameOffset = kEventBlockOffset + kEventBlockBytes;
    }
    r.seek(nameOffset);
    set.name = r.fixedString(nameLength);

    r.seek(nameOffset + nameLength);
    r.align(16);
    const std::size_t group = r.pos();
    const glm::vec3 low = readVec3(r);
    r.skip(4);
    const glm::vec3 high = readVec3(r);
    r.seek(group + kGroupCountOffset);
    const std::uint32_t count = r.u32();
    if (count == 0 && data.size() > group + kGroupHeaderBytes) {
        set.walkPaths = readWalkPaths(r, group);
        if (!r.atEnd()) {
            r.fail(std::format("{} bytes follow the walking paths", r.remaining()));
        }
        return set;
    }
    r.seek(group + kGroupHeaderBytes);
    if (static_cast<std::size_t>(count) * kRecordBytes != r.remaining()) {
        r.fail(std::format("{} records do not fill the {} bytes left", count, r.remaining()));
    }
    const glm::vec3 step = (high - low) / kQuantumSteps;
    set.spectators.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        PlacedSpectator spectator;
        const float x = r.u16();
        const float y = r.u16();
        const float z = r.u16();
        spectator.position = low + glm::vec3(x, y, z) * step;
        spectator.heading = r.u8();
        spectator.crowdClass = r.u8();
        spectator.flags = r.u8();
        spectator.extra = {r.u8(), r.u8(), r.u8()};
        set.spectators.push_back(spectator);
    }
    return set;
}

LegacyCrowdFile parseLegacyCrowdFile(ByteView data, const std::string& source)
{
    BeReader r(data, source);
    LegacyCrowdFile file;
    file.version = r.u32();
    file.entryCount = r.u32();
    file.reserved = r.u32();
    return file;
}

} // namespace fh::spectators
