#pragma once

#include "base/Bytes.h"

#include <glm/vec3.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/// The procedural sets that carry the game's animated world objects: kind 4
/// holds an object (its geometry and a Granny file of skeletons and
/// animations), kind 5 places objects of kind-4 sets around the track.
///
/// Both start with the header every procedural set has (see
/// world::ScatterSet; parsed by 0x82ECBB08): "OEGP", the kind at 0x30, the
/// name length at 0x3C, the count e of 8-byte entries at 0x40, the count x
/// of 8-byte header entries at 0x44, the file size at 0x48, the mesh count
/// at 0x54 (0 in every set of these kinds) and at 0x58 a flag for the
/// 56-byte event block at 0x60. After the name: x header entries (4-byte
/// aligned, u32 id and u32 length of a blob that follows the list), then e
/// entries (4-byte aligned, u32 id and u32 flag; a set flag adds a 20-byte
/// record and four strings, which no set of these kinds has). The body
/// starts after them. The loader picks the class by the kind (0x82EE0998:
/// kind 4 makes the class with vtable 0x82002954, kind 5 the one with
/// vtable 0x82002A04), and the class's body parser (vtable slot 30) reads
/// the rest.
namespace openhorizon::animated {

/// The kinds of procedural set this module reads.
inline constexpr std::uint32_t kObjectSetKind = 4;
inline constexpr std::uint32_t kPlacementSetKind = 5;

/// A kind-4 set: an animated object, in the track's bin.zip
/// (`Anim_ANIM_*` sets) or as a file of media/animatedobjects.
///
/// Body layout, verified in the body parser (0x82EC0F58) and read to the
/// last byte of all 33 kind-4 sets of Colorado and all 290 files of
/// media/animatedobjects; each block is 4-byte aligned:
///
///     16-byte aligned, a 96-byte header: +0 bounds minimum and +16 maximum
///       (vec4), +36 size of the Granny file, +40 size of the GPU block,
///       +48 bytes of vertices and indices, +60 entry count, +68 string
///       count, +80 and +84 opaque and blended draw totals
///     strings: one u32 length each, then the texts (texture names in the
///       pack's files, none in the track's)
///     entries, 60 bytes: u32 opaque draws, u32 blended draws, u32 bone
///       count, then per level of detail u32 opaque draws, u32 blended
///       draws, u32 batch count and a pointer
///     per entry and level: its batches, 68 bytes: +0 bone (0xFFFFFFFF for
///       skinned geometry), +4 vertex count, +8 index count, +12 opaque and
///       +16 blended draw counts, +20 1 when rigid, +24 string count, +28
///       light count, +36 to +56 pointers (non-zero where the batch has
///       vertices, +44, and indices, +52), +60 f32 ModelData, +64 vertex
///       type; then per batch
///       its strings, then per batch its light records (20 bytes, +8 a
///       count of 56-byte records following each)
///     per batch of every entry and level: its opaque draws, then its
///       blended draws, 44 bytes each: four s32 texture indices (-1 none),
///       u32 first triangle, u32 triangle count, three u32 flags, f32
///       specular power, f32 specular intensity
///     the Granny file (granny::readFileInfo)
///     the GPU block to the end of the file: every batch's u16 index list
///       (triangles), then every batch's vertices, each 4-byte aligned, in
///       entry, level and batch order
///
/// Entry i draws model i of the Granny file (the counts of all 1,014
/// entries match its models and skeletons); a rigid batch's bone indexes
/// that model's skeleton (all 1,496 are in range), and so do a skinned
/// batch's vertices' bone indices. Every draw's triangles lie in its
/// batch's index list. In the track's sets, entry k of the e list is the
/// track texture a draw's texture index k names (e equals the number of
/// indices the draws use in all 33 sets; inferred from that). In the
/// pack's files the strings name `textures/<name>.xds` (2,503 of 2,513
/// references resolve).
///
/// Batch strings are attachments, e.g. `Audio: path=Triggerable_Events/...
/// eventfile=TRG_Fan_Generator oneshot=False` (615 batches); light records
/// look like spot lights (inferred from their values). Neither is used here.
/// Not known: header +44, +52 and +56, batch +32 and the draws' flags
/// beyond what AnimatedObjectDraw says.
struct AnimatedObjectDraw {
    /// Texture index per sampler (diffuse s0, normal map s1, emissive s3,
    /// specular s4), -1 where the draw has none. Which slot feeds which
    /// sampler is inferred from the texture names' suffixes (_d, _n, _emis,
    /// _s) in the pack's files.
    std::array<std::int32_t, 4> textures{-1, -1, -1, -1};
    std::uint32_t firstTriangle = 0;
    std::uint32_t triangleCount = 0;
    /// The words at +24, +28 and +32. The draw loop's render states read
    /// a material's +24 (non-zero: no culling) and +28 (non-zero: additive,
    /// no depth write), and its +32 (2: the normal map blends in at 0.7);
    /// that these are the same records is inferred.
    std::array<std::uint32_t, 3> flags{};
    float specularPower = 0.0F;
    float specularIntensity = 0.0F;
};

struct AnimatedObjectBatch {
    /// The skeleton bone a rigid batch moves with; -1 for skinned batches,
    /// whose vertices name their own bones.
    int bone = -1;
    /// 0-5 (see AnimatedObjectSet::vertexStride()).
    std::uint32_t vertexType = 0;
    std::uint32_t vertexCount = 0;
    std::uint32_t indexCount = 0;
    /// Where the batch's vertices and u16 indices start in the set's data.
    std::size_t vertexOffset = 0;
    std::size_t indexOffset = 0;
    std::vector<AnimatedObjectDraw> opaque;
    std::vector<AnimatedObjectDraw> blended;
    std::vector<std::string> attachments;
    std::uint32_t lights = 0;
    /// The word at +60, which the draw loop uploads as ModelData.x
    /// (0x82444D4C): the switch-on value from which emissive maps light.
    /// 0 in every batch of the disc.
    float modelData = 0.0F;
};

struct AnimatedObjectEntry {
    std::uint32_t boneCount = 0;
    /// Batches per level of detail, finest first.
    std::array<std::vector<AnimatedObjectBatch>, 3> lods;
};

struct AnimatedObjectSet {
    std::string name;
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    /// Track texture ids (the e list); empty in the pack's files.
    std::vector<std::uint32_t> textureIds;
    /// Texture names (the strings); empty in the track's sets.
    std::vector<std::string> textureNames;
    std::vector<AnimatedObjectEntry> entries;
    /// Where the Granny file lies in `data`, and its size.
    std::size_t grannyOffset = 0;
    std::size_t grannySize = 0;
    /// The whole file, which the batches' offsets point into.
    Bytes data;

    /// Bytes per vertex of a vertex type, as the draw sets the stream
    /// stride (0x82445210): 0 four bones and a tangent, 1 four bones, 2 one
    /// bone and a tangent, 3 one bone, 4 rigid with a tangent, 5 rigid.
    static std::uint32_t vertexStride(std::uint32_t vertexType);
    ByteView granny() const { return ByteView(data).subspan(grannyOffset, grannySize); }
};

/// A kind-5 set: placements of animated objects.
///
/// Body layout, verified in the body parser (0x82EC6E78), writer
/// (0x82EC77E8) and size function (0x82EC7F50), and read to the last byte
/// of all 202 kind-5 sets of Colorado:
///
///     4-byte aligned, a 28-byte header: u32 placement count, u32 count of
///       64-byte records (0 in every set), u32 run-time bytes, u32 more
///       run-time bytes, two pointers, f32 the largest cull distance
///     16-byte aligned, 128 bytes per placement: X, Y and Z axis rows and
///       the position (vec4 each, w = 1), then +64 u32 index into the
///       header entries, +68 and +72 f32 distances at which the object
///       switches to levels 1 and 2, +76 f32 cull distance, +80 u32 1 for
///       an instance of its own (0: shares the set's one instance), +84
///       u32, +88 and +92 f32 cull distances of particular views (-1:
///       never drawn there), +96 u32 0, +100 f32 trigger distance (-1:
///       always), +104 f32 night threshold, +108 f32 day threshold, +112
///       u32 count of 8-byte (u32 id, f32 value) overrides, +116 pointer
///     per placement, its overrides (none on Colorado), then the 64-byte
///       records
///
/// A placement names its object by a header entry, whose id is the number
/// of the kind-4 set (`__R00G<number>.pgeo`). The run-time link code was
/// not found; the mapping is inferred from the set names, and it holds for
/// all 455 placements of Colorado (windmills to the windmill sets,
/// sprinklers to the sprinklers, festival rides to the rides). The axes
/// are proper rotations without scale on every placement of Colorado and
/// map the object's coordinates to the world's: world = x X + y Y + z Z +
/// position, as the draw's InstanceMatrix does (0x824449B0).
struct AnimatedPlacement {
    /// Number of the kind-4 set placed.
    std::uint32_t objectSet = 0;
    glm::vec3 position{0.0F};
    /// Rows: the images of the object's X, Y and Z axes.
    std::array<glm::vec3, 3> axes{
        glm::vec3(1.0F, 0.0F, 0.0F), glm::vec3(0.0F, 1.0F, 0.0F), glm::vec3(0.0F, 0.0F, 1.0F)};
    /// Distances from which levels 1 and 2 are drawn.
    std::array<float, 2> lodDistances{0.0F, 0.0F};
    float cullDistance = 0.0F;
    bool ownInstance = true;
    /// The object plays only while the camera is this close; negative:
    /// always.
    float triggerDistance = -1.0F;
    /// The object plays only while the time of day's SwitchOnLights value
    /// is at least this, when this is positive.
    float nightThreshold = 0.0F;
    /// The object plays only while SwitchOnLights is at most this, when
    /// this is below 0.95.
    float dayThreshold = 1.0F;
};

struct AnimatedPlacementSet {
    std::string name;
    /// The set's event block (see world::Placement::eventProp), whose
    /// routes gate its placements.
    bool eventProp = false;
    std::string eventTag;
    std::vector<std::uint8_t> eventRoutes;
    std::vector<AnimatedPlacement> placements;
};

/// The kind of a procedural set from its first 0x34 bytes, or 0 if the data
/// is not a procedural set.
std::uint32_t setKind(ByteView prefix);

/// Parses a kind-4 set, keeping `data`. Throws DataError, naming `source`,
/// if it does not have the layout above or its Granny file and GPU block do
/// not end where the header says.
AnimatedObjectSet readObjectSet(Bytes data, const std::string& source);

/// Parses a kind-5 set. Throws DataError, naming `source`, if it does not
/// have the layout above.
AnimatedPlacementSet readPlacementSet(ByteView data, const std::string& source);

} // namespace openhorizon::animated
