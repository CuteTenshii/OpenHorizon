#pragma once

#include "base/Bytes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace fh::spectators {

/// A bone of a spectator skeleton. The bind pose has no rotation: every
/// bone's frame is parallel to the model's, so a bone's bind transform is a
/// translation to `modelPosition`, and `modelPosition` is the parent's plus
/// `localOffset` (verified on every skeleton of the disc).
struct Bone {
    std::string name;
    /// Index of the parent bone, -1 for the root. Parents precede children.
    int parent = -1;
    glm::vec3 localOffset {0.0F};
    glm::vec3 modelPosition {0.0F};
};

/// The skeletons of the disc: 19 bones for standing people ("Bip001",
/// "sk1_Bone_root", legs, spine, neck, arms, head) and 15 for seated ones.
/// Coordinates are those of the meshes (SkinnedModel).
struct Skeleton {
    std::vector<Bone> bones;

    /// True if both have as many bones with the same parents. Clips of one
    /// skeleton share this but not always the bind positions: most differ
    /// by millimetres, and one (Barrier_Idle_04) moves every bone by half a
    /// metre, which is part of its motion.
    bool sameHierarchy(const Skeleton& other) const;
};

/// A bone's pose relative to its bind pose: the bone's local transform is
/// translate(localOffset + translation) * rotate(rotation) * scale(scale).
struct BonePose {
    glm::quat rotation {1.0F, 0.0F, 0.0F, 0.0F};
    glm::vec3 translation {0.0F};
    glm::vec3 scale {1.0F};
};

/// A decoded spectator animation (`media/spectators/anims/*.anim.bin`).
///
/// The file is a CAFF container (see world::readCaffSurface): a 0x190-byte
/// header whose u32 at 0x48 is the header size and whose u32 at 0x60 is the
/// size of the ".data" section that follows. Pointers inside the section are
/// byte offsets from its start (the container's relocation table turns them
/// into addresses when the game loads the section in place). Big-endian.
/// Offsets below are from the section start; everything marked verified was
/// matched against the game's decoder (0x82EEC180-0x82EECCB0 in the
/// executable) and decodes all 56 files of the disc to smooth motion:
///
///     0x00  "animation", NUL; 0x0A "00.00.00.0036", NUL (the version the
///           game requires)
///     0x1C  f32 duration, seconds: (frames - 1) / 30
///     0x20  u16 bone count, u16 frame count
///     0x24  -> codec block ("QUAT_BITSTREAM")
///     0x2C  -> bone table, 52 bytes a bone: f32[3] local offset, f32[3]
///           model position, 16 unused bytes, s16 parent, s16 first child,
///           s16 next sibling, s16 index, s16 -1, s16 0
///     0x30  -> u16 per bone (0, 1, 2, ...)
///     0x34  -> name table: u32 count, -> entries of (-> name, u32 0,
///           u32 bone index), then a hash table
///     0x48  -> a second codec block ("BITSTREAM") whose data pointer is
///           null in every file of the disc
///
///     codec block   char[22] name, u16 (codec number, set at load),
///                   -> codec data
///     codec data    f32 1.0; -> scale[9] f32; -> default[9] f32; -> masks;
///                   -> initial values; -> widths; u16 initial-value bytes;
///                   u16 width bytes; u16 bytes per frame; u16 channel types
///                   (9); u16 floats per bone output (12); u16 bones;
///                   u16 frames; u8 wide
///
/// Channel types, three per group: 0-2 rotation x, y, z (a unit quaternion
/// with w = sqrt(1 - x² - y² - z²), never negative); 3-5 translation; 6-8
/// scale. Defaults are 0, 0, 0, 0, 0, 0, 1, 1, 1; scales 1/16383 for the
/// rotation and 1/128 for the others. Rotations are the bones' local
/// rotations (the bind pose has none) and translations are added to the
/// bind offsets; the idle clips and most cheers start at the bind pose.
///
/// Masks: per bone one byte for the rotation group, whose low two bits say
/// which other groups follow as bytes of their own: bit 1 translation, bit 0
/// scale. In a group's byte, bits 7, 6, 5 mark its x, y, z channels as
/// present and bits 4, 3, 2 mark a present channel as constant. Absent
/// channels take their default.
///
/// Every present channel has an initial value (s16, or s32 when `wide` is
/// set) in order of bone, group and channel; every animated one has a width
/// (4 bits, low nibble first, or a byte when `wide` is set) and stores
/// width + 1 bits a frame. A channel's value is (initial + stored) * scale;
/// a constant one's is initial * scale. The frames follow the widths, each
/// `bytes per frame` long, and are read as one bit stream: bit b is bit
/// b mod 32 (least significant first) of the big-endian u32 at word b / 32.
struct AnimationClip {
    std::string name;
    Skeleton skeleton;
    int frameCount = 0;
    float duration = 0.0F;
    /// Poses of every bone in frame 0, then frame 1, and so on.
    std::vector<BonePose> frames;
    /// Channels that change over the clip, and those that hold one value.
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

/// Decodes an animation file. Throws DataError naming `source` if the data
/// is not a CAFF animation with the layout above.
AnimationClip parseAnimation(ByteView data, const std::string& source);

/// The pose at `time` seconds, looping over the clip: rotations are
/// interpolated spherically and the rest linearly between the two frames
/// around `time`. `out` needs one entry per bone.
void samplePose(const AnimationClip& clip, float time, std::span<BonePose> out);

/// Skinning matrices of `pose` (one per bone) in the skeleton's
/// coordinates: the posed bone's transform, built from `animated`'s local
/// offsets, times the inverse of its bind transform in `bind` (the skeleton
/// the mesh was made for), so a vertex's skinned position is the weighted
/// sum of matrix * position. The skeletons must have the same hierarchy;
/// `out` needs one entry per bone.
void skinMatrices(
    const Skeleton& animated, const Skeleton& bind, std::span<const BonePose> pose, std::span<glm::mat4> out);

} // namespace fh::spectators
