#pragma once

#include "base/Bytes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace openhorizon::spectators {

/// The bind pose has no rotation: a bone's bind transform is a translation to `modelPosition`, the parent's plus
/// `localOffset`. Verified on every skeleton of the disc.
struct Bone {
    std::string name;
    /// Index of the parent bone, -1 for the root. Parents precede children.
    int parent = -1;
    glm::vec3 localOffset {0.0F};
    glm::vec3 modelPosition {0.0F};
};

/// 19 bones for standing people, 15 for seated ones, in the meshes' coordinates (SkinnedModel).
struct Skeleton {
    std::vector<Bone> bones;

    /// Same bone count and parents. Clips of one skeleton can still differ in bind positions (Barrier_Idle_04
    /// moves every bone by half a metre as part of its motion).
    bool sameHierarchy(const Skeleton& other) const;
};

/// A bone's pose relative to its bind pose: the bone's local transform is
/// translate(localOffset + translation) * rotate(rotation) * scale(scale).
struct BonePose {
    glm::quat rotation {1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 translation {0.0F};
    glm::vec3 scale {1.0F};
};

/// A decoded spectator animation (`media/spectators/anims/*.anim.bin`): a big-endian CAFF container (see
/// world::readCaffSurface; u32 header size at 0x48, ".data" size at 0x60), pointers as offsets from the section
/// start. Verified against the game's decoder (0x82EEC180-0x82EECCB0). Offsets from the section start:
///
///     0x00  "animation", NUL; 0x0A "00.00.00.0036", NUL (required version)
///     0x1C  f32 duration, seconds: (frames - 1) / 30
///     0x20  u16 bone count, u16 frame count
///     0x24  -> codec block ("QUAT_BITSTREAM")
///     0x2C  -> bone table, 52 bytes a bone: f32[3] local offset, f32[3]
///           model position, 16 unused bytes, s16 parent, s16 first child,
///           s16 next sibling, s16 index, s16 -1, s16 0
///     0x30  -> u16 per bone (0, 1, 2, ...)
///     0x34  -> name table: u32 count, -> entries of (-> name, u32 0,
///           u32 bone index), then a hash table
///     0x48  -> a second codec block ("BITSTREAM"), data pointer always null
///
///     codec block   char[22] name, u16 (codec number, set at load),
///                   -> codec data
///     codec data    f32 1.0; -> scale[9] f32; -> default[9] f32; -> masks;
///                   -> initial values; -> widths; u16 initial-value bytes;
///                   u16 width bytes; u16 bytes per frame; u16 channel types
///                   (9); u16 floats per bone output (12); u16 bones;
///                   u16 frames; u8 wide
///
/// Channels, three per group: 0-2 local rotation x, y, z (w = sqrt(1 - x² - y² - z²), never negative), 3-5
/// translation added to the bind offset, 6-8 scale. Defaults 0, 0, 0, 0, 0, 0, 1, 1, 1; scales 1/16383 for
/// rotation, 1/128 for the others.
///
/// Masks: per bone a rotation byte whose bit 1 / bit 0 say a translation / scale byte follows. In each, bits
/// 7, 6, 5 mark x, y, z present and bits 4, 3, 2 constant; absent channels take their default.
///
/// Present channels have an initial value (s16, s32 if `wide`) in bone, group, channel order; animated ones a
/// width (nibble, low first, or a byte if `wide`) and width + 1 bits a frame. Value is (initial + stored) * scale.
/// Frames follow the widths as one bit stream: bit b is bit b mod 32 (LSB first) of the big-endian u32 b / 32.
struct AnimationClip {
    std::string name;
    Skeleton skeleton;
    int frameCount = 0;
    float duration = 0.0F;
    /// Poses of every bone in frame 0, then frame 1, and so on.
    std::vector<BonePose> frames;
    int animatedChannels = 0;
    int constantChannels = 0;

    std::size_t boneCount() const { return skeleton.bones.size(); }
    const BonePose& pose(int frame, std::size_t bone) const
    {
        return frames[static_cast<std::size_t>(frame) * boneCount() + bone];
    }
    /// 30 on the disc; derived from the duration and frame count.
    float framesPerSecond() const;
};

/// Throws DataError naming `source` if the data is not a CAFF animation with the layout above.
AnimationClip parseAnimation(ByteView data, const std::string& source);

/// The pose at `time` seconds, looping: slerp for rotations, linear for the rest. `out` needs one entry per bone.
void samplePose(const AnimationClip& clip, float time, std::span<BonePose> out);

/// The posed transform (from `animated`'s offsets) times the inverse bind transform of `bind`, the mesh's
/// skeleton. The skeletons must share a hierarchy; `out` needs one entry per bone.
void skinMatrices(
    const Skeleton& animated, const Skeleton& bind, std::span<const BonePose> pose, std::span<glm::mat4> out);

} // namespace openhorizon::spectators
