#pragma once

#include "base/Bytes.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
}

/// Readers for the three files of an Anark Gameface 4.0.7 scene as Forza
/// Horizon ships them in `media/ui/scenes/ui4/` (and per-car digital gauges in
/// `media/cars/<car>/digitalgauge/`). All three are big-endian.
///
/// `.bgf` ("AnarkBGF", the presentation) holds the logic: every element with
/// its name hash, parent and master attribute values, the components' slides
/// (states) with per-slide attribute values, and keyframed animation tracks.
///
/// `.bsg` ("FGBkranA", the scene graph) holds the exported node tree with each
/// node's baked transform, bounds and opacity.
///
/// `.fbf` (render data, read by the game's own CRenderEngineForza) holds each
/// node's name and what the renderer needs: text styles, material colours and
/// blend modes, texture paths with their UV transforms, and the meshes.
///
/// The three files share element ids: `BgfElement::id`, `BsgNode::id` and
/// `FbfRecord::id` name the same object.
///
/// The `.bgf` and `.fbf` layouts were read off their deserializers in the
/// game executable (CBGFDeserializer and CRenderEngineForza::LoadRenderData)
/// and checked against every scene on the disc; the `.bsg` layout was
/// established from the data alone. Fields the code does not use and whose
/// meaning was not established are skipped and documented as such.
namespace fh::ui {

/// Anark's name hash: `h = h * 0x1003F + c` over the name's bytes (as signed
/// chars) and its terminating NUL, keeping the low 27 bits. Attribute keys
/// and element identifiers carry it in their low 27 bits. Read off the
/// executable's hashing loop; the names of the attributes the HUD uses and
/// 94% of element names (the rest were renamed by the exporter) match.
std::uint32_t anarkHash(std::string_view name);

/// The key of a string table entry for an id such as "IDS_Place": start at
/// 0xFFFF and, per character (lower-cased), XOR it in and rotate the 16-bit
/// value left by 7. Verified against the executable and the English tables.
std::uint16_t stringIdHash(std::string_view id);

/// The value types found in attribute keys' top five bits.
enum class AttributeType : std::uint8_t {
    Integer = 1,
    Float = 3,
    Boolean = 4,
    /// An index into BgfFile::strings.
    String = 5,
    /// Types 17 to 21 appear only on behaviour (contract) elements: 17 an
    /// integer, 19 a float, 20 and 21 strings (21 holds the contract class
    /// and the element paths of its parameters, such as
    /// "parent.GAUGE_ANALOG.TEXT_GEAR_ANALOG").
    ContractInteger = 17,
    ContractFloat = 19,
    ContractString = 20,
    ContractPath = 21,
};

/// One attribute value: a 27-bit name hash, its type and 32 raw bits.
struct Attribute {
    std::uint32_t hash = 0;
    std::uint8_t type = 0;
    std::uint32_t raw = 0;

    float asFloat() const;
};

enum class ElementKind : std::uint8_t {
    /// Groups and models.
    Node = 1,
    Camera = 2,
    Light = 3,
    Text = 4,
    Material = 5,
    Image = 6,
    /// A component: a node with its own slides and timeline.
    Component = 7,
    /// A behaviour instance (contract); the HUD reimplements the few it needs.
    Behavior = 8,
};

struct BgfElement {
    /// anarkHash() of the element's name.
    std::uint32_t nameHash = 0;
    /// Index into BgfFile::elements, or -1 for the root.
    std::int32_t parent = -1;
    std::uint32_t id = 0;
    ElementKind kind = ElementKind::Node;
    /// Master values of the attributes the presentation sets or animates.
    std::vector<Attribute> attributes;
};

/// A key of an animation track. Between two keys the value follows a cubic
/// Bezier in value with control values `control1` and `control2` (the value a
/// third and two thirds of the way to the next key for a straight line);
/// time runs linearly. -1 in both controls marks the last key.
struct Keyframe {
    float time = 0.0F;
    float value = 0.0F;
    float control1 = 0.0F;
    float control2 = 0.0F;
};

struct Track {
    /// Index into BgfFile::elements.
    std::uint32_t element = 0;
    /// anarkHash() of the animated attribute.
    std::uint32_t attribute = 0;
    std::vector<Keyframe> keys;

    /// The track's value at `timeMs`, holding the end values outside it.
    float evaluate(float timeMs) const;
};

struct SlideEntry {
    /// Index into BgfFile::elements.
    std::uint32_t element = 0;
    /// False in the master slide, true in the component's own slides.
    bool active = false;
    std::vector<Attribute> attributes;
    /// Indices into BgfFile::tracks run while the slide plays.
    std::vector<std::uint16_t> tracks;
};

struct Slide {
    std::string name;
    /// Index into BgfFile::elements of the component the slide belongs to.
    std::uint32_t owner = 0;
    /// The play flags byte as stored. The runtime (slide entry at
    /// 0x82fec688, timeline tick at 0x82febdb0) reads it as follows, which
    /// the fields below spell out.
    std::uint8_t flags = 0;
    /// flags & 3: how many times the timeline plays before it holds at its
    /// end; 0 plays it forever.
    int playCount = 0;
    /// flags & 4: the timeline starts paused (the host seeks it).
    bool startsPaused = false;
    /// flags & 8: every second pass runs backwards.
    bool pingPong = false;
    /// flags & 16: when a timeline that plays a set number of times, without
    /// ping-pong, reaches its end, the runtime fires COMMAND_GOTONEXTSLIDE
    /// at the component (0x82feeab0).
    bool nextAtEnd = false;
    std::uint32_t durationMs = 0;
    std::vector<SlideEntry> entries;
};

/// The commands a logic action can carry, as the anarkHash() of their names.
namespace command {
extern const std::uint32_t GoToSlideName;
extern const std::uint32_t SetProperty;
extern const std::uint32_t GoToTime;
extern const std::uint32_t Play;
} // namespace command

/// One action of an event handler, five u32 as the loader (0x82fe7ee0)
/// reads them.
struct LogicAction {
    /// Index into BgfFile::elements of the element acted on: the component
    /// for slide and time commands, the element whose attribute is set for
    /// COMMAND_SETPROPERTY.
    std::uint32_t target = 0;
    /// A second element index; the component the target sits in.
    std::uint32_t context = 0;
    /// anarkHash() of the command name (low 27 bits of the stored key).
    std::uint32_t command = 0;
    /// Per command: the slide name's anarkHash() (low 27 bits), the
    /// attribute key (type << 27 | hash), or the time in milliseconds.
    std::uint32_t key = 0;
    /// The attribute value's raw bits for COMMAND_SETPROPERTY.
    std::uint32_t value = 0;
};

struct LogicHandler {
    /// anarkHash() of the event name, such as "COUNTDOWN_GO".
    std::uint32_t event = 0;
    std::vector<LogicAction> actions;
};

/// The handlers of one element, usually a contract behaviour: the game
/// fires events at the control bound to that contract.
struct LogicOwner {
    std::uint32_t element = 0;
    std::vector<LogicHandler> handlers;
};

/// The presentation (`.bgf`). Layout:
///
///     u8 endian (1 = big), u8 4, u32 header size (26), "AnarkBGF",
///     u32 version (3), u32 0, u16 width, u16 height, u8 1, u32, u8
///     u32 100, then 25 u32 counts: [0] elements, [1] contracts,
///       [2] attributes, [6] strings, [14] slides, ... (the last six repeat
///       the first six)
///     string section: u32 size, strings as {u32 length, bytes}
///     element section: u32 size, per element (elements + contracts):
///       u32 kind<<27 | name hash, u32 parent (-1 root), u32 id, u8 kind,
///       u8 flags, if flags & 4 {u8, u32}, u32 n, n x {u32 type<<27 | hash,
///       u32 value}
///     contract section, animation section, logic section, parameter
///       section: each u32 size + data; the logic section is owners of
///       {u32 element, u32 n, n x {u32 event key, u32 m, m x 5 u32
///       actions}}; animations are tracks of
///       {u32 element, u32 attribute hash, u8, u8, u16 n,
///        n x {u32 time ms, f32 value, u32, f32 control1, u32, f32 control2}}
///     slide section: u32 size, slides of {char name[16], u32 owner,
///       u8 flags, u32, u32 duration ms, u32 n, n x {u32 element, u8 active,
///       u32 m, m x {u32 key, u32 value}, u32 k, k x {u8 kind, u8,
///       u16 index}}} where kind 0 names an animation track and kind 1 an
///       action of the logic section
struct BgfFile {
    int width = 0;
    int height = 0;
    std::vector<std::string> strings;
    std::vector<BgfElement> elements;
    std::vector<Track> tracks;
    std::vector<Slide> slides;
    std::vector<LogicOwner> logic;

    /// Throws DataError when the file does not have this layout.
    static BgfFile parse(ByteView data, const std::string& source);
};

enum class BsgNodeType : std::uint8_t {
    Group = 1,
    Model = 2,
    Camera = 3,
    Light = 4,
    Text = 5,
    Layer = 8,
};

/// A node of the scene graph (`.bsg`) with its transform as exported. The
/// translation already includes the pivot offset (inferred: where the
/// presentation gives a pivot, the exported translation is position minus
/// pivot and the exported pivot is zero).
struct BsgNode {
    std::int32_t parent = -1;
    std::uint32_t id = 0;
    BsgNodeType type = BsgNodeType::Group;
    glm::vec3 position{0.0F};
    /// Euler angles in radians.
    glm::vec3 rotation{0.0F};
    glm::vec3 scale{1.0F};
    glm::vec3 pivot{0.0F};
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
    float opacity = 1.0F;
    /// Index of the layer the node draws in.
    std::uint8_t layer = 0;
};

/// The scene graph. Layout:
///
///     u8 1, u8 4, u16 0, u32 24, "FGBkranA", u32 2, u32 2, u32 0, u32 0,
///     u32 8, u32 node count, u32 asset count, u32 node section size
///     nodes of 96 bytes: s32 parent index, u32 id, u8 type, 3 bytes,
///       f32 position[3], rotation[3], scale[3], pivot[3], bounds min[3],
///       bounds max[3], opacity, u8 layer, 7 bytes of flags
///     u32 asset section size, assets of {u32 id, u8 type}
struct BsgFile {
    std::vector<BsgNode> nodes;

    static BsgFile parse(ByteView data, const std::string& source);
};

enum class FbfKind : std::uint8_t {
    Group = 0,
    Text = 1,
    Camera = 2,
    Layer = 3,
    Model = 4,
    Material = 5,
    Light = 6,
    Image = 7,
};

/// A text effect block of render data version 1007 and later, as the text
/// loader (0x82f2cbe0) reads it into a 68-byte record and the text setup
/// (0x82f2a1e8) copies it into the text renderer's parameters. What the
/// renderer does with the numbers was not traced; only the layout is
/// verified. In the HUD scene only the first effect is ever switched on,
/// always with a colour of black at half alpha and two numbers between 0.2
/// and 0.7, which reads as a drop shadow; its units are not known.
struct FbfTextEffects {
    /// u8 flag, colour as four f32 (0 to 1), two f32.
    bool firstEnabled = false;
    glm::vec4 firstColor{0.0F};
    glm::vec2 firstValues{0.0F};
    /// u8 flag, colour as four f32, one f32.
    bool secondEnabled = false;
    glm::vec4 secondColor{0.0F};
    float secondValue = 0.0F;
    /// u8 flag, three f32.
    bool thirdEnabled = false;
    glm::vec3 thirdValues{0.0F};
};

struct FbfText {
    /// The text as authored and the string table reference ("Table:IDS_Id")
    /// that replaces it when set; the BGF textstring attribute usually holds
    /// the text shown.
    std::string text;
    std::string stringId;
    std::string font;
    int size = 0;
    /// 0 left, 1 centre, 2 right.
    int horizontalAlign = 0;
    /// 0 top, 1 middle, 2 bottom.
    int verticalAlign = 1;
    float tracking = 0.0F;
    float leading = 0.0F;
    /// The width of the authored text in scene units as the editor measured
    /// it (render data 1008; -1 before). SUPER_STACKER reads it from the
    /// runtime's copy of the record (+216) as the width its layout grows
    /// from; the scenes' values agree with the text measured at 96/72 of its
    /// size ("ARTIST NAME" in horizon_b 15: 115).
    float authoredWidth = -1.0F;
    /// Present when the record carries an effects block.
    std::optional<FbfTextEffects> effects;
};

enum class BlendFactor : std::uint8_t { Zero, One, SourceAlpha, InverseSourceAlpha, Other };

struct FbfMaterial {
    /// Diffuse colour, 0 to 1.
    glm::vec4 diffuse{1.0F};
    BlendFactor sourceBlend = BlendFactor::SourceAlpha;
    BlendFactor destinationBlend = BlendFactor::InverseSourceAlpha;
};

/// How an image repeats outside its square: tiled, mirrored, or not at all
/// (nothing is drawn there). Which stored value means which (0, 1, 2) is
/// inferred from the textures that carry them: the loader keeps the values
/// but their use by the sampler setup was not traced.
enum class TextureWrap : std::uint8_t { Repeat, Mirror, Clamp };

/// The UV transform Anark builds from an image's attributes:
/// uv' = scale * rotate(rotation) * (uv + position) + pivot, then v is
/// flipped (v' = 1 - v) because meshes have v = 0 at the bottom. The
/// runtime rebuilds the image's stored 4x4 matrix from these attributes
/// whenever they change (0x82f1d8f0 calls the builder at 0x82f1c600, whose
/// body is VMX128 vector code objdump cannot decode). The formula here is
/// verified: the disc test rebuilds the stored matrix (a row-vector 4x4,
/// u' = u m0 + v m4 + m12, v' = u m1 + v m5 + m13) of all 15,578 images of
/// every scene that use mapping mode 0, rotated and scaled ones included,
/// which also settles that the scale applies after the rotation. The 41
/// images with another mapping mode (none in the HUD scene) are outside
/// this formula; the builder special-cases modes 2 and 3.
struct UvTransform {
    float rotationDegrees = 0.0F;
    glm::vec2 position{0.0F};
    glm::vec2 scale{1.0F};
    glm::vec2 pivot{0.0F};

    /// Rows of the 2x3 matrix mapping mesh UV to texture UV.
    void matrix(glm::vec3& row0, glm::vec3& row1) const;
};

struct FbfImage {
    /// The game path, such as "GAME:\MEDIA\UI\TEXTURES\HORIZON\HUD\DIALS\NEEDLE.TGA".
    std::string path;
    /// Id of the material the image belongs to.
    std::uint32_t material = 0;
    UvTransform uv;
    TextureWrap wrapU = TextureWrap::Clamp;
    TextureWrap wrapV = TextureWrap::Clamp;
    /// The first of four u32 after the pivot; the matrix builder takes it as
    /// a mapping mode and special-cases 2 and 3.
    std::uint32_t mappingMode = 0;
    /// The matrix the exporter stored (row-major as read), which the runtime
    /// recomputes from the attributes.
    std::array<float, 16> storedMatrix{};
};

struct FbfCamera {
    /// Stored as the exporter wrote it; the HUD layers use an orthographic
    /// camera whose attributes come from the presentation.
    float matrix[16] = {};
};

struct FbfRecord {
    FbfKind kind = FbfKind::Group;
    std::uint32_t id = 0;
    std::string name;
    FbfText text;
    FbfMaterial material;
    FbfImage image;
    /// Index into FbfFile::meshes for models.
    std::uint32_t mesh = 0;
};

struct MeshVertex {
    glm::vec3 position{0.0F};
    glm::vec3 normal{0.0F};
    glm::vec2 uv{0.0F};
};

struct Mesh {
    std::vector<MeshVertex> vertices;
    /// Triangle list. Anark exports rectangles double sided (a front and a
    /// back copy with opposite winding); the reader keeps one copy.
    std::vector<std::uint16_t> indices;
    glm::vec3 boundsMin{0.0F};
    glm::vec3 boundsMax{0.0F};
};

/// Render data. Layout:
///
///     u32 version (1006 to 1008), u32 record count
///     records: u32 kind, u32 id, u32 name length, name, then by kind:
///       Group (kinds 0, 8, 9, 10): nothing
///       Custom renderer (kind 11): u32
///       Text: u32, u32 n + text, u32 n + string id, f32, f32, u32 n + font,
///             u32 size, u32 horizontal align, u32 vertical align, u32,
///             u32, f32 tracking, f32 leading, u32, u8, [1008: f32 authored width],
///             [1007+: u8 effects flag, if set FbfTextEffects as
///             u8, 6 x f32, u8, 5 x f32, u8, 3 x f32]
///       Camera: 64 bytes; Layer: 24 bytes; Light: 104 bytes
///       Model: u32 mesh index, f32
///       Material: f32 diffuse[4], 13 f32 (other colours), u32 model id,
///             u32 layer, u32, u32, u32, u32 source blend, u32 dest blend,
///             u32, u32 (blend values are Xenos D3DBLEND: 0 zero, 1 one,
///             6 source alpha, 7 inverse source alpha)
///       Image: u32 material id, u32 n + path, f32 matrix[16], f32 rotation,
///             f32 position[2], f32 scale[2], f32 pivot[2], u32 mapping
///             mode, u32, u32, u32, u32 wrap u, u32 wrap v (0 repeat,
///             1 mirror, 2 clamp; the loader 0x82f2d3e8 reads them into the
///             image's +220 and +224, whose use by the sampler setup was not
///             traced, so the meaning of each value is inferred from the
///             textures that carry them)
///     mesh section: u32 mesh count, u32 group table size, u32 buffer size,
///       mesh count x 96-byte descriptors (bounds min at 16 and max at 32,
///       primitive group count at 48, a D3DVertexBuffer header at 48 whose
///       fetch constant at 72 holds the byte address in its top 30 bits and
///       the size in dwords in bits 2..25 of the next word, the group table
///       offset at 80), the table of 36-byte primitive groups (a
///       D3DIndexBuffer header at 4: index address at 28, index bytes at
///       32), then the buffer, which the addresses count from: vertices of
///       {f32 position[3], normal[3], uv[2]} and u16 triangle indices.
struct FbfFile {
    std::uint32_t version = 0;
    std::vector<FbfRecord> records;
    std::vector<Mesh> meshes;

    static FbfFile parse(ByteView data, const std::string& source);
};

/// The three files of one scene.
struct SceneFiles {
    BgfFile bgf;
    BsgFile bsg;
    FbfFile fbf;

    /// `path` is the scene's game path without extension, such as
    /// "media/ui/scenes/ui4/947_HUD". Throws DataError if a file is missing
    /// or malformed.
    static SceneFiles load(const Vfs& vfs, std::string_view path);
};

/// The texture file the game loads for a path in a scene: the same name
/// under media/ui/textures with the extension replaced by .xds, lower case.
std::string texturePathForScene(std::string_view scenePath);

} // namespace fh::ui
