#pragma once

#include "base/Bytes.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/mat3x3.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/// The RAD Granny 3D files (`.gr2`) the animated objects carry their
/// skeletons and animations in: the executable links Granny (its sources
/// named d:/dev/rad/granny/rt/*, entry points thunked at
/// 0x82A70DC0-0x82A71088), and every kind-4 procedural set holds one file
/// (see AnimatedObjectSet).
///
/// File layout, big-endian with 32-bit pointers, verified on the 33 kind-4
/// sets of Colorado and the 290 files of media/animatedobjects:
///
///     0x00  16-byte magic b595110e 4bb5a56a 502828eb 04b37825
///     0x10  u32 header size (456), u32 0 (no compression), 8 bytes
///     0x20  u32 version (7), u32 total size, u32 CRC, u32 offset of the
///           section table from 0x20, u32 section count, u32 section and
///           u32 offset of the root object's type, the same of the root
///           object, u32 type tag, ...
///     section table, 44 bytes per section: u32 compression (0 in every
///           file), u32 data offset, u32 stored size, u32 size, u32
///           alignment, u32 first 16-bit and first 8-bit offsets, u32
///           offset and count of the pointer fixups, u32 offset and count
///           of the byte-order marshalling entries
///     pointer fixup, 12 bytes: u32 offset in its section of a pointer
///           field, u32 section and u32 offset of what it points to
///
/// Offsets are from the start of the file. A pointer field holds nothing
/// usable itself: a fixup says where it points, and a field without one is
/// null. The data is self-describing: every object's type is a list of
/// 32-byte member definitions (u32 member type, name pointer, pointer to the
/// member's own type, u32 array width, 16 more bytes), ended by member type
/// 0, and the readers below find members by name. The root object is
/// Granny's file info, whose Models and Animations are what the game uses;
/// the files hold no Granny meshes (the geometry is the set's own).
namespace openhorizon::animated::granny {

/// A Granny transform: position, orientation and a 3x3 scale and shear,
/// each present when its flag is set.
///
/// Verified in BuildCompositeTransform4x4 (0x82A7FF80): the matrix is
/// p' = R (SS p) + position, R the orientation's rotation (0x82A8D420) and
/// SS the scale and shear with its stored rows acting on column vectors
/// (0x82A8D2B0), written out for row vectors with the translation in the
/// last row.
struct Transform {
    static constexpr std::uint32_t kHasPosition = 1;
    static constexpr std::uint32_t kHasOrientation = 2;
    static constexpr std::uint32_t kHasScaleShear = 4;

    std::uint32_t flags = 0;
    glm::vec3 position{0.0F};
    glm::quat orientation{1.0F, 0.0F, 0.0F, 0.0F};
    /// Acting on column vectors, as the file's rows say.
    glm::mat3 scaleShear{1.0F};

    /// The matrix BuildCompositeTransform4x4 composes, as a glm matrix
    /// acting on column vectors.
    glm::mat4 matrix() const;
};

/// A bone of a skeleton.
struct Bone {
    std::string name;
    /// Index of the parent bone, -1 for a root.
    int parent = -1;
    Transform local;
    /// The inverse of the bone's world matrix in the rest pose, as a glm
    /// matrix acting on column vectors (the file stores it for row vectors,
    /// translation in its last row). It includes the model's initial
    /// placement, so InverseWorld x World is the identity at rest.
    glm::mat4 inverseWorld{1.0F};
};

struct Model {
    std::string name;
    std::string skeletonName;
    std::vector<Bone> bones;
    Transform initialPlacement;

    /// The index of the bone called `boneName`, or -1.
    int findBone(std::string_view boneName) const;
};

/// One of Granny's curve formats, numbered as the format table at
/// 0x83376EE0 (52-byte entries) numbers them and as a curve's Format byte
/// names them.
enum class CurveFormat : std::uint8_t {
    DaKeyframes32f = 0,
    DaK32fC32f = 1,
    DaIdentity = 2,
    DaConstant32f = 3,
    D3Constant32f = 4,
    D4Constant32f = 5,
    DaK16uC16u = 6,
    DaK8uC8u = 7,
    D4nK16uC15u = 8,
    D4nK8uC7u = 9,
    D3K16uC16u = 10,
    D3K8uC8u = 11,
    D9I1K16uC16u = 12,
    D9I3K16uC16u = 13,
    D9I1K8uC8u = 14,
    D9I3K8uC8u = 15,
    D3I1K32fC32f = 16,
    D3I1K16uC16u = 17,
    D3I1K8uC8u = 18,
};

/// A curve decoded to floats: `knots` (times in seconds) and one control
/// point of `dimension` floats per knot, sampled as a B-spline of
/// `degree` (see sample()).
struct Curve {
    enum class Kind : std::uint8_t {
        /// The rest pose's value: the track does not move this part.
        Identity,
        /// One value for all time (`controls` holds it).
        Constant,
        /// Knots and controls.
        Spline,
        /// DaKeyframes32f: controls without knots, whose sampling was not
        /// traced (see sample()).
        Keyframes,
    };

    Kind kind = Kind::Identity;
    CurveFormat format = CurveFormat::DaIdentity;
    int degree = 0;
    int dimension = 0;
    std::vector<float> knots;
    std::vector<float> controls;

    /// The curve's value at `time` seconds into an animation of
    /// `duration` seconds, into `out` (`dimension` floats). With `loop`
    /// the spline wraps around the ends as a looping Granny control
    /// samples it; without, it repeats its first and last knots and
    /// controls. Returns false for Identity and Keyframes curves, leaving
    /// `out` alone (see TransformTrack for what they give).
    ///
    /// Verified in the executable: FindKnot (0x82A8C2F0, 0x82A8C360) takes
    /// the first knot after the time, clamped to the last; the padding at
    /// the ends (0x82A85CF0) repeats the first and last knot and control, or
    /// with a looping control takes them from the other end, shifted by
    /// the duration; the samplers (tables 0x833778D0 and, for
    /// orientations, 0x83377970, indexed by 10 x degree + dimension)
    /// interpolate linearly at degree 1, with the quadratic weights of
    /// 0x82A84018 at degree 2 and with de Boor's recursion (0x82A85B28) at
    /// degree 3; orientation curves are normalised after (0x82A94AE8,
    /// 0x82A858B0).
    /// Missing: how DaKeyframes32f curves are sampled (their FindKnot
    /// returns 0 and they store no knots), used by 217 curves in 8 files of
    /// the disc (bungee, fly-under-string, stage strobes, a timed-event
    /// biplane); they are left at the rest pose.
    bool sample(float time, float duration, bool loop, float* out) const;
};

/// The curves that move one bone. A bone with a track takes all three
/// parts of its local transform from it, an identity curve giving the
/// identity (zero position, no rotation, unit scale) with the part's flag
/// clear, not the rest pose's value (SampleTrackUUULocal 0x82A95050:
/// 0x82A950BC, 0x82A9516C, 0x82A9522C). A bone without one keeps the
/// skeleton's rest transform (EndLocalPoseAccumulation 0x82A74F08).
struct TransformTrack {
    /// The bone the track moves, by name.
    std::string name;
    Curve position;
    Curve orientation;
    Curve scaleShear;
};

struct TrackGroup {
    std::string name;
    std::vector<TransformTrack> tracks;
};

struct Animation {
    std::string name;
    float duration = 0.0F;
    float timeStep = 0.0F;
    float oversampling = 0.0F;
    /// One group per model the animation moves.
    std::vector<TrackGroup> groups;
};

struct FileInfo {
    std::vector<Model> models;
    std::vector<Animation> animations;
};

/// Reads the file info of a Granny file. Throws DataError, naming `source`,
/// if the data is not a Granny file of the layout above, uses a compressed
/// section, or lacks a member the readers need.
FileInfo readFileInfo(ByteView data, const std::string& source);

/// The world matrices of `model`'s bones (as glm matrices acting on column
/// vectors) for local transforms `locals` (one per bone), each bone's local
/// matrix times its parent's world (for row vectors; verified on the
/// position-only path 0x82A9B748), the roots' parent being the model's
/// initial placement: the game builds that placement's matrix
/// (0x82ED895C) and passes it as the offset when it samples the model's
/// animations (0x82EC49E4).
void worldPose(const Model& model, const std::vector<Transform>& locals, std::vector<glm::mat4>& world);

} // namespace openhorizon::animated::granny
