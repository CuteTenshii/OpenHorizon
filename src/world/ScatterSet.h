#pragma once

#include "TrackPlacements.h"

#include <glm/vec4.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon::world {

/// A procedural placement set of a track (`__R00G#####.pgeo` in bin.zip,
/// magic "OEGP", big-endian): many copies of a few models, such as the trees,
/// bushes, rocks and fences of an area. Offsets are from the file start:
///
///     0x30 u32 kind (7 for model sets), 0x3C u32 name length n,
///     0x40 u32 e, 0x44 u32 x, 0x48 u32 file size, 0x54 u32 m,
///     0x58 u32 k, non-zero when the set carries an event block
///     0x60 when k != 0: the event block, 56 bytes (see below)
///     then the set name, n bytes with its NUL
///     4-byte aligned, x entries of 8 bytes (none in any model set)
///     16-byte aligned, m mesh entries of 64 bytes: bounds (2 x 4 f32),
///          u32 p, three level-of-detail slots (u32 1 when used, u32), u32
///     one u32 draw record per used slot of each mesh with p = 0, in order
///     for each mesh with p > 0: p bytes of source path; then 4-byte aligned
///     e entries of 8 bytes, then 20 bytes whose fourth u32 is g
///     g groups of 92 bytes: +44 u32 mesh, +48 u32 n, +56 u32 b,
///          +60 f32 draw distance
///     16-byte aligned, for each group: n instances of 96 bytes (X, Y and Z
///          axes as 4 f32, scaled; position as 4 f32 with w = 1; tint;
///          ground normal, whose last byte, +95, picks the instance's
///          block), then b blocks of 8 f32: the ends E0, E1 and E2 of the
///          levels of detail, and +28 the fade width F
///
/// The event block is the one the game gates props with (see
/// Placement::eventProp): u32 0xFFFFFFFF, a 32-byte NUL-padded event tag,
/// u8 route count and the route bytes.
///
/// Verified in the executable: the set loader (0x82EE0998) picks the class
/// by the kind at 0x30, kind 7 making a CProceduralModels (0x82EDEAE0); the
/// header parser (0x82ECBB08) starts the data at 0x60, steps over 56 bytes
/// when the u32 at 0x58 is not zero, then takes n name bytes (0x82EBAC70),
/// the 4-byte aligned 8-byte entries counted at 0x44 and the 16-byte aligned
/// 64-byte mesh entries counted at 0x54. Every set of Colorado whose u32 at
/// 0x58 is not zero has 0xFFFFFFFF at 0x60; 469 of its 2031 kind 7 sets
/// carry an event block, none of them with a tag. Kinds other than 7 hold
/// grass, crowds, light glows and night lighting in other layouts. A mesh's
/// draw records are template placements at the world origin whose models
/// the set copies; meshes named by a source path instead have no draw
/// record and are left out. The rest of the layout reads all 2031 kind 7
/// sets of Colorado; it is inferred from the data, not from game code.
///
/// Verified in the executable, the distances (CProceduralModels'
/// visibility, 0x82EE1D00): an instance takes block `byte95 >> 1` of its
/// group (the block's address is the blocks' start plus
/// `(byte95 << 4) & ~31`); the loader (0x82ED6318) makes one cell per
/// instance whose byte 95 is even, holding the world box of the instance's
/// transformed mesh bounds, and draws no instance whose byte 95 is odd; the
/// camera's distance d is the 3D distance to the closest point of that box; the group is culled
/// beyond its draw distance (+60), the instance beyond E2, and it draws the
/// level of slot 0 below E0, slot 1 below E1 and slot 2 beyond, each slot
/// taking the highest used level at or below it. All of them are scaled by
/// a factor that is 1 except for a zoom camera (0x833A572C, set at
/// 0x8246E2D4). In a sample of 20,074 instances of Colorado, byte 95 is
/// not 0 in 30% and E2 is below the group's draw distance in 6%. Not applied: the fade
/// over F at each level's ends and the distance fade the foliage effects
/// take from FadeValues (see ScatterPass).
///
/// Far tier (verified, loader 0x82ED6318): each copy is also a card from its
/// group's draw distance out to its block's fifth distance (+20). Group: +0
/// texture entry (-1 none), +4 second entry, +8/+12 u of the edges, +16/+20 v
/// of foot and top, +24/+28 width and height, +32 colour scale, +36 type, +40
/// colour from the copy (+64..+76) instead of 0.5. Set entries at 0x40: a PVS
/// texture record and 0. Groups sharing +0, +4, +36 and +76 merge, with the
/// smallest draw distance as fade start (0x82ED66C8). Types 0 and 2 swap the
/// edges at random (0x82DBE490), 1 and 3 always; a hash of the copy stands in
/// for the random. Header 0x38: the largest +20, past which the set is skipped
/// (0x82EE1F04); +0x10/+0x20 its box.
struct ScatterSet {
    struct Instance {
        /// Index into `meshDraws`.
        std::uint32_t mesh = 0;
        /// Where the instance goes, with the distances at which the game
        /// switches between the mesh's levels and stops drawing it.
        Placement placement;
    };

    std::string name;
    /// The set's entry in the track's archive (see lzx::ZipArchive::entries()).
    std::uint32_t entry = 0;
    /// Whether the set carries an event block, with the block's tag and
    /// routes. They are copied into every instance's placement
    /// (Placement::eventProp, eventTag and eventRoutes), so the set's copies
    /// are shown and hidden like the zone files' event props.
    bool eventProp = false;
    std::string eventTag;
    std::vector<std::uint8_t> eventRoutes;
    /// Draw record of each level of detail of each mesh, finest first; empty
    /// for meshes named by a source path.
    std::vector<std::vector<std::uint32_t>> meshDraws;
    std::vector<Instance> instances;

    /// One copy's card in the far tier, in world coordinates.
    struct Billboard {
        glm::vec3 position{0.0F};
        /// The copy's Y axis, normalised: the card's up.
        glm::vec3 up{0.0F};
        /// The copy's X axis, normalised: the normal of cards that don't turn.
        glm::vec3 across{0.0F};
        /// The copy's ground normal, which lights the cards that turn.
        glm::vec3 groundNormal{0.0F};
        float halfWidth = 0.0F;
        float height = 0.0F;
        /// The block's third distance, the group's fade start, the fifth distance.
        float nearEnd = 0.0F;
        float fadeStart = 0.0F;
        float farEnd = 0.0F;
        /// u of the edge on the side axis's side and of the opposite edge, v of
        /// foot and top, and the rectangle's width in u (the step to the next picture).
        float uPlus = 0.0F;
        float uMinus = 0.0F;
        float vFoot = 0.0F;
        float vTop = 0.0F;
        float uStep = 0.0F;
        /// Colour; w above 0.5 takes it as it is.
        glm::vec4 colour{0.0F};
        glm::vec3 bakedLight{0.0F};
        /// Index into textureRecords.
        std::uint32_t texture = 0;
        /// The group's type (+36), which picks the shaders.
        std::uint32_t type = 0;
    };
    /// PVS texture records of the set's texture entries.
    std::vector<std::uint32_t> textureRecords;
    std::vector<Billboard> billboards;
    /// The set's box and the distance beyond which none of its cards shows.
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    float billboardReach = 0.0F;
};

/// Bytes at the start of a procedural set that scatterSetKind() reads.
inline constexpr std::size_t kScatterKindEnd = 0x34;
/// The kind of a procedural set that holds models (see ScatterSet).
inline constexpr std::uint32_t kModelSetKind = 7;

/// The kind of the procedural set starting with `prefix` (at least
/// kScatterKindEnd bytes), or nothing if it is not one.
std::optional<std::uint32_t> scatterSetKind(ByteView prefix);

/// Parses a procedural placement set. Throws DataError if the data does not
/// have the layout ScatterSet documents.
ScatterSet readScatterSet(ByteView data);

} // namespace openhorizon::world
