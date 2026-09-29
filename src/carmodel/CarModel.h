#pragma once

#include "CarBin.h"
#include "CarEffect.h"
#include "CarMaterials.h"
#include "world/TrackTextures.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

/// A car's geometry as the game's shaders read it: every part's vertices as
/// stored in its carbin (big-endian, 28 bytes each, see carmodel::Vertex),
/// triangle-list indices into the part's own vertices, and batches of one
/// mesh each. The car shaders decode the vertices themselves (the position
/// through `pack_partPosition`, texture coordinates through the mesh's
/// factors), so nothing is converted on the CPU.
///
/// Nodes place their batches: `placement` maps file space (as the carbin
/// stores positions: right-handed, front towards -Z) to car space (X to the
/// right, Y up, Z forward, the origin on the ground below the middle of the
/// wheelbase). Triangles are left in file order; a placement with a negative
/// determinant (the Z flip, and mirrored wheels) reverses their facing,
/// which the renderer accounts for.
struct CarMesh {
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

    /// The vertices of one part in `vertices`.
    struct VertexRange {
        std::uint32_t byteOffset = 0;
        std::uint32_t count = 0;
        /// The part's position (xyz, file space) and the length of its box's
        /// half diagonal (w), which scale the stored directions into
        /// positions (see carmodel::Vertex).
        glm::vec4 packedPosition{0.0F};
    };

    struct Batch {
        /// The mesh's material as the carbin names it.
        std::string material;
        /// The technique of CarModel::effect that draws it (see
        /// carmodel::techniqueName).
        std::string technique;
        /// Texture coordinate factors of the mesh (CarBin::Mesh::uv0, uv1).
        glm::vec4 uv0{0.0F, 1.0F, 0.0F, 1.0F};
        glm::vec4 uv1{0.0F, 1.0F, 0.0F, 1.0F};
        std::uint32_t firstIndex = 0;
        std::uint32_t indexCount = 0;
        /// Index into `nodes`.
        std::uint32_t node = 0;
        /// Index into `ranges`.
        std::uint32_t range = 0;
        /// Index into CarModel::parts.
        std::uint32_t part = 0;
        /// In the model's translucent draw list.
        bool translucent = false;
        /// Blurred rims and tyre lips, shown instead of the sharp ones when
        /// the wheel spins fast.
        bool motionBlur = false;
        /// Texture files for the effect's samplers, by sampler name; indices
        /// into CarModel::textures.
        std::vector<std::pair<std::string, std::uint32_t>> textures;
    };

    /// A group of batches that moves as one.
    struct Node {
        NodeKind kind = NodeKind::Body;
        /// For wheels, rotors and calipers.
        std::optional<Corner> corner;
        /// File space to car space.
        glm::mat4 placement{1.0F};
        /// The file header's lighting minimum and range, which decompress
        /// the vertices' lighting bytes (the shaders' shCompressionFactors).
        glm::vec2 shCompression{0.0F, 1.0F};
        /// The wheel centre for corner nodes (spin about the X axis through
        /// it, steer about the Y axis through it); the origin otherwise.
        glm::vec3 pivot{0.0F};
        std::uint32_t firstBatch = 0;
        std::uint32_t batchCount = 0;
    };

    Bytes vertices;
    std::vector<std::uint32_t> indices;
    std::vector<VertexRange> ranges;
    std::vector<Batch> batches;
    std::vector<Node> nodes;
    /// Car space box of every batch but the blurred wheels.
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};

    std::size_t vertexCount() const { return vertices.size() / carbin::kVertexStride; }
    std::size_t triangleCount() const { return indices.size() / 3; }
    /// Car space position of vertex `index` of `batch`'s range, as the
    /// shaders compute it.
    glm::vec3 position(const Batch& batch, std::uint32_t index) const;
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
    /// Load the in-race LODs of `<car>.carbin` (their most detailed, LOD 1)
    /// instead of `<car>_lod0.carbin`.
    bool inRaceLods = false;
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
/// the most detailed LOD that has one (LOD 1). The executable draws its
/// close-up tyre from the shared `media/wheels/tire<compound>/` model
/// instead, sized per corner through the tyre shaders' wheelMorph and
/// wheelScale; those values come from a per-wheel block whose fill was not
/// traced (vertex constants c59, c120 and c122 are copied from its offsets
/// 16, 64/68 and 32 at 0x824652A0), so neither that model nor the morph is
/// used and the LOD 1 tyre keeps its modelled profile.
///
/// Batches name their technique, textures and parameters as the game does:
/// see techniqueName(), CarTextureTable and ShaderSettings.
///
/// Wheel centres come from `physics/maxdata.xml`, the fit being ours and not
/// the game's (whose hub placement was not found) (Misc: Wheelbase,
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
    /// The effect the batches' techniques belong to, with those techniques
    /// translated.
    std::shared_ptr<const CarEffect> effect;
    /// Material parameters by paint finish ("Normal", "Metallic"): the
    /// shared settings, the finish's differences and the car's own file.
    std::map<std::string, ShaderSettings, std::less<>> settings;
    /// Wheel and tyre parameters (`shadersettings/tire/normal.xml`).
    ShaderSettings tyreSettings;
    /// The BlurRim element of `media/cars/shared/GlobalCarAttribs.xml`: the
    /// wheel speed the blurred rims are scaled to (MaxRPM) and the fraction
    /// of it at which the geometry blends to them (GeomBlendStart, End).
    struct BlurRim {
        float maxRpm = 0.0F;
        float blendStart = 0.0F;
        float blendEnd = 0.0F;
    };
    std::optional<BlurRim> blurRim;
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
