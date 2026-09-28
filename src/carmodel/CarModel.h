#pragma once

#include "CarBin.h"
#include "CarMaterials.h"
#include "world/TrackTextures.h"

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Database;
class Vfs;
} // namespace fh

namespace fh::carmodel {

enum class Corner : std::uint8_t { FrontLeft, FrontRight, RearLeft, RearRight };

inline constexpr std::array<Corner, 4> kCorners{
    Corner::FrontLeft, Corner::FrontRight, Corner::RearLeft, Corner::RearRight};

/// Two-letter corner code as the game spells it in file names ("lf", "rr").
std::string_view cornerCode(Corner corner);

/// A car's geometry, ready for upload: interleaved position (x, y, z),
/// normal (x, y, z), first and second texture coordinates (u, v each) and
/// the vertex colour's four bytes (R, G, B, A) in one float's space, as in
/// world::TileMesh, and triangle-list indices in batches of one material
/// each. Everything is in car space: X to the right, Y up, Z forward, the
/// origin on the ground below the middle of the wheelbase. Triangles wind
/// clockwise seen from their front, as in world::TileMesh; mirrored wheels
/// are rewound to match. Texture coordinates follow Direct3D (v down, the
/// first texture row at v = 0), which uploads unchanged to GL when rows are
/// given top first.
///
/// The colour is an occlusion estimate from the vertex lighting bytes (see
/// carmodel::Vertex): grey levels from the first byte scaled so that its
/// open-surface peak (106) is white, alpha 255; vertices without lighting
/// data (all four bytes zero, as in wheels) are white.
struct CarMesh {
    static constexpr int kFloatsPerVertex = 11;
    static constexpr int kNormal = 3;
    static constexpr int kTexcoord = 6;
    static constexpr int kSecondTexcoord = 8;
    static constexpr int kColour = 10;
    static constexpr std::uint32_t kNoTexture = 0xFFFFFFFF;

    enum class NodeKind : std::uint8_t {
        /// The exterior; static.
        Body,
        /// The cockpit file: the interior and the parts it shares with the
        /// outside (bonnet, headlights, mirrors, windscreen, A pillars),
        /// which the exterior file lacks; static, drawn with the body.
        Cockpit,
        /// Rim and tyre; spins about the axle and, at the front, steers.
        Wheel,
        /// Brake disc; spins and steers with its wheel.
        Rotor,
        /// Brake caliper; steers with its wheel but does not spin.
        Caliper,
    };

    struct Batch {
        std::string material;
        Shading shading = Shading::Plain;
        /// Index into CarModel::textures, or kNoTexture.
        std::uint32_t texture = kNoTexture;
        /// Multiplies the texture, or stands alone without one (see
        /// carmodel::baseColour).
        glm::vec4 colour{1.0F};
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        /// Index into `nodes`.
        std::uint32_t node = 0;
        /// Index into CarModel::parts.
        std::uint32_t part = 0;
        bool translucent = false;
        /// Blurred rims and tyre lips, shown instead of the sharp ones when
        /// the wheel spins fast.
        bool motionBlur = false;
    };

    /// A group of batches that moves as one.
    struct Node {
        NodeKind kind = NodeKind::Body;
        /// For wheels, rotors and calipers.
        std::optional<Corner> corner;
        /// The wheel centre for corner nodes (spin about the X axis through
        /// it, steer about the Y axis through it); the origin otherwise.
        glm::vec3 pivot{0.0F};
        std::uint32_t firstBatch = 0;
        std::uint32_t batchCount = 0;
    };

    std::vector<float> vertices;
    std::vector<std::uint32_t> indices;
    std::vector<Batch> batches;
    std::vector<Node> nodes;
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};

    std::size_t vertexCount() const { return vertices.size() / kFloatsPerVertex; }
    std::size_t triangleCount() const { return indices.size() / 3; }
};

/// Where a wheel sits and how its model was fitted.
struct WheelMount {
    Corner corner = Corner::FrontLeft;
    /// Wheel centre in car space.
    glm::vec3 centre{0.0F};
    /// Tyre radius and width after fitting, metres.
    float radius = 0.0F;
    float width = 0.0F;
    /// Scale applied to the wheel model across (X) and along the tread (Y,
    /// Z) to reach that size.
    float axialScale = 1.0F;
    float radialScale = 1.0F;
    /// The model was mirrored across X (right-hand wheels without a
    /// `_rightside` model of their own).
    bool mirrored = false;
};

/// What to load.
struct CarModelOptions {
    /// Leave out upgrade variants (see partVariant()).
    bool stockOnly = true;
    bool cockpit = true;
    bool brakes = true;
    bool wheels = true;
    /// Decode textures to mip chains; off, batches still name their files.
    bool textures = true;
};

/// A loaded car: its files parsed, geometry merged into one CarMesh and
/// textures decoded.
///
/// Files, relative to the car's folder `media/cars/<car>/`: the exterior
/// `<car>_lod0.carbin` (the most detailed; `<car>.carbin` holds the in-race
/// LODs 1 to 5 and is used when `_lod0` is missing or, as for the traffic
/// vehicles made drivable, holds only 17-vertex stand-ins), the cockpit
/// `<car>_cockpit.carbin`, and per corner `<car>_caliper<xx>_lod0.carbin`
/// and `<car>_rotor<xx>_lod0.carbin` (xx: lf, rf, lr, rr), modelled around
/// their wheel centre. The wheel is `media/wheels/<wheel>/<wheel>.carbin`
/// for the car's stock wheel (Data_Car.StockWheelID → List_Wheels.MediaName,
/// or the car's own name without a database), with
/// `media/wheels/<wheel>_rightside/` for right-hand wheels where it exists.
/// Wheel models are left-hand wheels (their spokes face -X); others are
/// mirrored for the right. Their LOD 0 has rims only, so the tyre comes from
/// the most detailed LOD that has one (LOD 1); the game builds its close-up
/// tyre another way, which is not decoded.
///
/// Wheel centres come from `physics/maxdata.xml` (Misc: Wheelbase,
/// FrontTrackOuter, RearTrackOuter, BottomCenterWheelbasePos): Z at half
/// the wheelbase either side, X at half the outer track less half the tyre
/// width, Y at the tyre radius. Tyre sizes come from Data_Car
/// (Front/RearTireWidthMM, TireAspect, WheelDiameterIN) when a database is
/// given, and the wheel is scaled to them; otherwise the model's own size is
/// kept. The exterior's own `wheel` part (the stock rim without tyre, at the
/// origin) is used only when the wheel archive is missing.
struct CarModel {
    struct Texture {
        /// Game path of the file.
        std::string path;
        world::TextureMipChain chain;
    };

    /// One part of one file, for inspection.
    struct Part {
        /// Game path of the carbin.
        std::string file;
        std::string name;
        ModelSource source = ModelSource::Exterior;
        PartVariant variant;
        /// Included in the mesh.
        bool used = false;
        std::uint32_t vertexCount = 0;
        std::uint32_t triangleCount = 0;
        /// Car space box of the part's vertices (at its corner for wheel
        /// and brake parts).
        glm::vec3 boundsMin{0.0F};
        glm::vec3 boundsMax{0.0F};
    };

    std::string mediaName;
    std::string wheelMediaName;
    /// Game paths of the carbins read.
    std::vector<std::string> files;
    std::vector<Part> parts;
    std::vector<WheelMount> wheels;
    CarMesh mesh;
    std::vector<Texture> textures;
    /// Files or textures that were missing or unreadable; the model is still
    /// usable without them.
    std::vector<std::string> warnings;

    /// Loads car `mediaName` (a folder of media/cars, any case). `db` may be
    /// null. Throws DataError if the exterior cannot be read or parsed.
    static CarModel load(
        const Vfs& vfs, const Database* db, std::string_view mediaName, const CarModelOptions& options = {});
};

/// Reads a carbin through the file system and parses it.
CarBin loadCarBin(const Vfs& vfs, std::string_view path, CarBin::Kind kind);

/// Decodes an Xbox 360 texture file (.xds) to a mip chain: DXT1, DXT3/5 and
/// DXN keep their blocks when the texture's swizzle is the identity (DXT3
/// becomes DXT5, as world::buildMipChain does); anything else is decoded to
/// RGBA8 with its swizzle. Throws DataError on failure.
world::TextureMipChain decodeXdsTexture(ByteView data);

} // namespace fh::carmodel
