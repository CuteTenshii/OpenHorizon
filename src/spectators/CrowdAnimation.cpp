#include "CrowdAnimation.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace fh::spectators {

namespace {

constexpr std::size_t kCaffHeaderSizeOffset = 0x48;
constexpr std::size_t kCaffDataSizeOffset = 0x60;
constexpr std::size_t kCaffMinimumHeader = 0x64;
constexpr std::string_view kKind = "animation";
constexpr std::string_view kVersion = "00.00.00.0036";
constexpr std::size_t kVersionOffset = 0x0A;
constexpr std::size_t kDurationOffset = 0x1C;
constexpr std::size_t kBoneCountOffset = 0x20;
constexpr std::size_t kFrameCountOffset = 0x22;
constexpr std::size_t kCodecOffset = 0x24;
constexpr std::size_t kBoneTableOffset = 0x2C;
constexpr std::size_t kNameTableOffset = 0x34;
constexpr std::size_t kCodecNameBytes = 22;
constexpr std::size_t kCodecDataPointer = 24;
constexpr std::size_t kBoneBytes = 52;
constexpr std::size_t kBoneParentOffset = 40;
constexpr std::size_t kNameEntryBytes = 12;
constexpr int kChannelTypes = 9;
constexpr int kGroupSize = 3;
constexpr int kMaxBones = 256;
constexpr int kMaxFrames = 100000;
constexpr std::uint8_t kTranslationFollows = 0x02;
constexpr std::uint8_t kScaleFollows = 0x01;
constexpr std::uint8_t kFirstChannelBit = 0x80;
/// A present channel's "constant" flag sits this many bits below its
/// "present" flag.
constexpr int kConstantShift = 3;

/// Bounds-checked big-endian reads at offsets of the data section.
class Section {
public:
    Section(ByteView data, const std::string& source)
        : m_data(data)
        , m_source(source)
    {
    }

    void need(std::size_t offset, std::size_t bytes) const
    {
        if (offset > m_data.size() || bytes > m_data.size() - offset) {
            throw DataError(
                "{}: data offset 0x{:x}+{} lies past the section (0x{:x})", m_source, offset, bytes, m_data.size());
        }
    }
    std::uint8_t u8(std::size_t offset) const
    {
        need(offset, 1);
        return m_data[offset];
    }
    std::uint16_t u16(std::size_t offset) const
    {
        need(offset, 2);
        return loadBe16(m_data.data() + offset);
    }
    std::uint32_t u32(std::size_t offset) const
    {
        need(offset, 4);
        return loadBe32(m_data.data() + offset);
    }
    std::int32_t s32(std::size_t offset) const { return static_cast<std::int32_t>(u32(offset)); }
    std::int16_t s16(std::size_t offset) const { return static_cast<std::int16_t>(u16(offset)); }
    float f32(std::size_t offset) const { return std::bit_cast<float>(u32(offset)); }
    glm::vec3 vec3(std::size_t offset) const { return {f32(offset), f32(offset + 4), f32(offset + 8)}; }
    /// The u32 at `offset` read as the offset of something `bytes` long.
    std::size_t pointer(std::size_t offset, std::size_t bytes) const
    {
        const std::size_t target = u32(offset);
        need(target, bytes);
        return target;
    }
    std::string text(std::size_t offset, std::size_t maxBytes) const
    {
        need(offset, 0);
        const std::size_t available = std::min(maxBytes, m_data.size() - offset);
        const auto* first = reinterpret_cast<const char*>(m_data.data() + offset);
        return std::string(first, strnlen(first, available));
    }
    /// A 32-bit word of the bit stream starting at `offset`; bytes past the
    /// section read as zero, as the stream's last word may be partial.
    std::uint32_t streamWord(std::size_t offset) const
    {
        std::uint32_t word = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const std::size_t at = offset + i;
            word = (word << 8) | (at < m_data.size() ? m_data[at] : 0U);
        }
        return word;
    }
    std::size_t size() const { return m_data.size(); }
    [[noreturn]] void fail(const std::string& message) const { throw DataError("{}: {}", m_source, message); }

private:
    ByteView m_data;
    const std::string& m_source;
};

/// Reads the frame stream the way the game's decoder does: from 32-bit
/// big-endian words, least significant bit first, starting at any byte.
class BitReader {
public:
    BitReader(const Section& section, std::size_t byteOffset)
        : m_section(section)
        , m_word(byteOffset & ~std::size_t {3})
        , m_bit(static_cast<unsigned>((byteOffset & 3U) * 8U))
    {
    }

    std::uint32_t read(unsigned bits)
    {
        const std::uint64_t pair
            = (static_cast<std::uint64_t>(m_section.streamWord(m_word + 4)) << 32) | m_section.streamWord(m_word);
        const std::uint64_t mask = bits >= 32 ? 0xFFFFFFFFULL : ((1ULL << bits) - 1ULL);
        const auto value = static_cast<std::uint32_t>((pair >> m_bit) & mask);
        const unsigned end = m_bit + bits;
        m_word += (end / 32U) * 4U;
        m_bit = end % 32U;
        return value;
    }

private:
    const Section& m_section;
    std::size_t m_word;
    unsigned m_bit;
};

struct Codec {
    std::array<float, kChannelTypes> scales {};
    std::array<float, kChannelTypes> defaults {};
    std::size_t masks = 0;
    std::size_t initials = 0;
    std::size_t widths = 0;
    std::size_t stream = 0;
    std::size_t frameBytes = 0;
    bool wide = false;
};

/// One group's mask byte of each bone, with the group it belongs to.
struct GroupMask {
    int group = 0;
    std::uint8_t mask = 0;
};

std::vector<std::vector<GroupMask>> readMasks(const Section& s, std::size_t masks, std::size_t boneCount)
{
    std::vector<std::vector<GroupMask>> out(boneCount);
    std::size_t at = masks;
    for (std::vector<GroupMask>& bone : out) {
        const std::uint8_t first = s.u8(at++);
        bone.push_back({0, first});
        if ((first & kTranslationFollows) != 0) {
            bone.push_back({1, s.u8(at++)});
        }
        if ((first & kScaleFollows) != 0) {
            bone.push_back({2, s.u8(at++)});
        }
    }
    return out;
}

/// The per-channel initial values and widths, in stream order.
struct ChannelTable {
    std::vector<std::int32_t> initials;
    /// Bits stored per frame; 0 for constant channels.
    std::vector<unsigned> bits;
};

ChannelTable readChannels(const Section& s, const Codec& codec, const std::vector<std::vector<GroupMask>>& masks)
{
    ChannelTable table;
    std::size_t initial = codec.initials;
    std::size_t width = codec.widths;
    bool highNibble = false;
    for (const std::vector<GroupMask>& bone : masks) {
        for (const GroupMask& group : bone) {
            for (int c = 0; c < kGroupSize; ++c) {
                const auto present = static_cast<std::uint8_t>(kFirstChannelBit >> c);
                if ((group.mask & present) == 0) {
                    continue;
                }
                if (codec.wide) {
                    table.initials.push_back(s.s32(initial));
                    initial += 4;
                } else {
                    table.initials.push_back(s.s16(initial));
                    initial += 2;
                }
                if ((group.mask & (present >> kConstantShift)) != 0) {
                    table.bits.push_back(0);
                    continue;
                }
                unsigned stored = 0;
                if (codec.wide) {
                    stored = s.u8(width++);
                } else {
                    const std::uint8_t byte = s.u8(width);
                    stored = highNibble ? (byte >> 4) : (byte & 0x0FU);
                    width += highNibble ? 1 : 0;
                    highNibble = !highNibble;
                }
                table.bits.push_back(stored + 1);
            }
        }
    }
    return table;
}

glm::quat quaternionFromXyz(float x, float y, float z)
{
    const float rest = 1.0F - (x * x + y * y + z * z);
    return {rest > 0.0F ? std::sqrt(rest) : 0.0F, x, y, z};
}

Skeleton readSkeleton(const Section& s, std::size_t boneCount)
{
    const std::size_t table = s.pointer(kBoneTableOffset, boneCount * kBoneBytes);
    Skeleton skeleton;
    skeleton.bones.resize(boneCount);
    for (std::size_t b = 0; b < boneCount; ++b) {
        const std::size_t entry = table + b * kBoneBytes;
        Bone& bone = skeleton.bones[b];
        bone.localOffset = s.vec3(entry);
        bone.modelPosition = s.vec3(entry + 12);
        bone.parent = s.s16(entry + kBoneParentOffset);
        if (bone.parent >= static_cast<int>(b) || bone.parent < -1) {
            s.fail(std::format("bone {} has parent {}, which does not precede it", b, bone.parent));
        }
    }
    const std::size_t names = s.u32(kNameTableOffset);
    if (names != 0) {
        const std::uint32_t count = s.u32(names);
        const std::size_t entries = s.pointer(names + 4, static_cast<std::size_t>(count) * kNameEntryBytes);
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::size_t entry = entries + i * kNameEntryBytes;
            const std::uint32_t index = s.u32(entry + 8);
            if (index < boneCount) {
                skeleton.bones[index].name = s.text(s.u32(entry), 256);
            }
        }
    }
    return skeleton;
}

} // namespace

bool Skeleton::sameHierarchy(const Skeleton& other) const
{
    if (bones.size() != other.bones.size()) {
        return false;
    }
    for (std::size_t i = 0; i < bones.size(); ++i) {
        if (bones[i].parent != other.bones[i].parent) {
            return false;
        }
    }
    return true;
}

float AnimationClip::framesPerSecond() const
{
    return duration > 0.0F && frameCount > 1 ? static_cast<float>(frameCount - 1) / duration : 30.0F;
}

AnimationClip parseAnimation(ByteView data, const std::string& source)
{
    if (data.size() < kCaffMinimumHeader || std::memcmp(data.data(), "CAFF", 4) != 0) {
        throw DataError("{}: not a CAFF container", source);
    }
    const std::size_t headerSize = loadBe32(data.data() + kCaffHeaderSizeOffset);
    const std::size_t dataSize = loadBe32(data.data() + kCaffDataSizeOffset);
    if (headerSize > data.size() || dataSize > data.size() - headerSize) {
        throw DataError("{}: the data section does not fit the file", source);
    }
    const Section s(data.subspan(headerSize, dataSize), source);
    if (s.text(0, kKind.size() + 1) != kKind) {
        s.fail("the container does not hold an animation");
    }
    if (s.text(kVersionOffset, kVersion.size() + 1) != kVersion) {
        s.fail(std::format("unsupported animation version \"{}\"", s.text(kVersionOffset, kVersion.size() + 1)));
    }

    AnimationClip clip;
    clip.duration = s.f32(kDurationOffset);
    const int boneCount = s.u16(kBoneCountOffset);
    clip.frameCount = s.u16(kFrameCountOffset);
    if (boneCount == 0 || boneCount > kMaxBones || clip.frameCount == 0 || clip.frameCount > kMaxFrames
        || !(clip.duration >= 0.0F)) {
        s.fail(std::format("implausible clip: {} bones, {} frames, {} s", boneCount, clip.frameCount, clip.duration));
    }
    const auto bones = static_cast<std::size_t>(boneCount);
    clip.skeleton = readSkeleton(s, bones);

    const std::size_t codecBlock = s.pointer(kCodecOffset, kCodecDataPointer + 4);
    const std::string codecName = s.text(codecBlock, kCodecNameBytes);
    if (codecName != "QUAT_BITSTREAM" && codecName != "QUAT_BITSTREAM32") {
        s.fail(std::format("unsupported codec \"{}\"", codecName));
    }
    const std::size_t q = s.pointer(codecBlock + kCodecDataPointer, 40);
    Codec codec;
    const std::size_t scales = s.pointer(q + 4, kChannelTypes * 4);
    const std::size_t defaults = s.pointer(q + 8, kChannelTypes * 4);
    for (int t = 0; t < kChannelTypes; ++t) {
        codec.scales[static_cast<std::size_t>(t)] = s.f32(scales + static_cast<std::size_t>(t) * 4);
        codec.defaults[static_cast<std::size_t>(t)] = s.f32(defaults + static_cast<std::size_t>(t) * 4);
    }
    codec.masks = s.u32(q + 12);
    codec.initials = s.u32(q + 16);
    codec.widths = s.u32(q + 20);
    const std::size_t initialBytes = s.u16(q + 24);
    const std::size_t widthBytes = s.u16(q + 26);
    codec.frameBytes = s.u16(q + 28);
    if (s.u16(q + 30) != kChannelTypes) {
        s.fail(std::format("codec has {} channel types, not {}", s.u16(q + 30), kChannelTypes));
    }
    if (s.u16(q + 34) != boneCount || s.u16(q + 36) != clip.frameCount) {
        s.fail("codec bone or frame count differs from the clip's");
    }
    codec.wide = s.u8(q + 38) != 0;
    codec.stream = codec.initials + initialBytes + widthBytes;

    const std::vector<std::vector<GroupMask>> masks
        = codec.masks != 0 ? readMasks(s, codec.masks, bones) : std::vector<std::vector<GroupMask>>(bones);
    const ChannelTable channels = codec.initials != 0 ? readChannels(s, codec, masks) : ChannelTable {};
    for (const unsigned bits : channels.bits) {
        ++(bits > 0 ? clip.animatedChannels : clip.constantChannels);
    }
    if (!channels.bits.empty()) {
        std::size_t bitsPerFrame = 0;
        for (const unsigned bits : channels.bits) {
            bitsPerFrame += bits;
        }
        if ((bitsPerFrame + 7) / 8 != codec.frameBytes) {
            s.fail(std::format("channels need {} bits a frame, frames are {} bytes", bitsPerFrame, codec.frameBytes));
        }
        s.need(codec.stream, codec.frameBytes * static_cast<std::size_t>(clip.frameCount - 1) + (bitsPerFrame + 7) / 8);
    }

    clip.frames.resize(bones * static_cast<std::size_t>(clip.frameCount));
    for (int frame = 0; frame < clip.frameCount; ++frame) {
        BitReader bitsIn(s, codec.stream + codec.frameBytes * static_cast<std::size_t>(frame));
        std::size_t channel = 0;
        for (std::size_t b = 0; b < bones; ++b) {
            std::array<float, kChannelTypes> values = codec.defaults;
            for (const GroupMask& group : masks[b]) {
                for (int c = 0; c < kGroupSize; ++c) {
                    const auto present = static_cast<std::uint8_t>(kFirstChannelBit >> c);
                    if ((group.mask & present) == 0) {
                        continue;
                    }
                    const auto type = static_cast<std::size_t>(group.group) * kGroupSize + static_cast<std::size_t>(c);
                    const unsigned bits = channels.bits[channel];
                    const auto stored = static_cast<std::int64_t>(bits > 0 ? bitsIn.read(bits) : 0U);
                    values[type] = static_cast<float>(stored + channels.initials[channel]) * codec.scales[type];
                    ++channel;
                }
            }
            BonePose& pose = clip.frames[static_cast<std::size_t>(frame) * bones + b];
            pose.rotation = quaternionFromXyz(values[0], values[1], values[2]);
            pose.translation = {values[3], values[4], values[5]};
            pose.scale = {values[6], values[7], values[8]};
        }
    }
    return clip;
}

void samplePose(const AnimationClip& clip, float time, std::span<BonePose> out)
{
    const std::size_t bones = clip.boneCount();
    if (out.size() < bones) {
        throw DataError("samplePose: {} poses given for {} bones", out.size(), bones);
    }
    if (clip.frameCount <= 1 || !(clip.duration > 0.0F)) {
        for (std::size_t b = 0; b < bones; ++b) {
            out[b] = clip.pose(0, b);
        }
        return;
    }
    float local = std::fmod(time, clip.duration);
    if (local < 0.0F) {
        local += clip.duration;
    }
    const float position = local * clip.framesPerSecond();
    const int first = std::clamp(static_cast<int>(position), 0, clip.frameCount - 1);
    const int second = std::min(first + 1, clip.frameCount - 1);
    const float t = std::clamp(position - static_cast<float>(first), 0.0F, 1.0F);
    for (std::size_t b = 0; b < bones; ++b) {
        const BonePose& a = clip.pose(first, b);
        const BonePose& c = clip.pose(second, b);
        out[b].rotation = glm::slerp(a.rotation, c.rotation, t);
        out[b].translation = glm::mix(a.translation, c.translation, t);
        out[b].scale = glm::mix(a.scale, c.scale, t);
    }
}

void skinMatrices(
    const Skeleton& animated, const Skeleton& bind, std::span<const BonePose> pose, std::span<glm::mat4> out)
{
    const std::size_t bones = animated.bones.size();
    if (!animated.sameHierarchy(bind)) {
        throw DataError("skinMatrices: the skeletons have different bones");
    }
    if (pose.size() < bones || out.size() < bones) {
        throw DataError("skinMatrices: {} poses and {} outputs for {} bones", pose.size(), out.size(), bones);
    }
    std::vector<glm::mat4> world(bones);
    for (std::size_t b = 0; b < bones; ++b) {
        const Bone& bone = animated.bones[b];
        glm::mat4 local = glm::translate(glm::mat4(1.0F), bone.localOffset + pose[b].translation);
        local *= glm::mat4_cast(pose[b].rotation);
        local = glm::scale(local, pose[b].scale);
        world[b] = bone.parent >= 0 ? world[static_cast<std::size_t>(bone.parent)] * local : local;
        out[b] = glm::translate(world[b], -bind.bones[b].modelPosition);
    }
}

} // namespace fh::spectators
