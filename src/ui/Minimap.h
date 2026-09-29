#pragma once

#include "UiRenderer.h"

#include "base/Bytes.h"
#include "render/Gl.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fh {
class Vfs;
}

/// The HUD minimap as the game draws it: CUIMapRenderer (render callback
/// 0x826e2e20) draws the track's road network through a perspective camera
/// into the render target the HUD scene's MATERIAL_VIEWPORT shows, styled
/// by media/ui/MapProfileMinimap.xml.
namespace fh::ui {

class MapCoverage;

/// A tag list of a node or way, and the lookup of one of its values.
using NavTags = std::vector<std::pair<std::string, std::string>>;

/// The value of tag `key` in `tags`, or empty.
std::string_view navTag(const NavTags& tags, std::string_view key);

/// One way through a node: the way's index and the node's place in it.
struct NavLink {
    std::uint32_t way = 0;
    std::uint32_t position = 0;
};

/// One node of a track's road network file.
struct NavNode {
    /// World x, y (height) and z.
    glm::vec3 position{0.0F};
    NavTags tags;
    /// The ways through the node.
    std::vector<NavLink> links;

    /// The value of tag `key`, or empty.
    std::string_view tag(std::string_view key) const { return navTag(tags, key); }
};

/// One way of a track's road network file (`<track>.nav`).
struct NavWay {
    /// Node positions on the ground plane: world x and z.
    std::vector<glm::vec2> points;
    /// The node indices the points come from, in order.
    std::vector<std::uint32_t> nodes;
    NavTags tags;

    /// The value of tag `key`, or empty.
    std::string_view tag(std::string_view key) const { return navTag(tags, key); }
};

/// The road network. Layout, as the loader (0x82d9dd18) reads it, all
/// big-endian:
///
///     u32 magic 0x0E177551, u32 node count, u32 way count, u32 tag count,
///       u32 way-node count, u32 link count, u32 key count, u32 value count,
///       u32 key text bytes, u32 value text bytes
///     nodes of {u32 id, f32 x, y, z, u32 link count, u32 first link,
///       u32 tag count, u32 first tag}
///     ways of {u32 node count, u32 first way node, u32 tag count,
///       u32 first tag}
///     way nodes: u32 node indices
///     links of {u32 way, u32 position in the way} (the ways through each
///       node; the neighbour lookup 0x82da9cc8 reads the second word as the
///       node's index in the way, and every one of colorado's 13238 links
///       names its node there)
///     tags of {u32 key index, u32 value index}
///     u32 key text offsets, u32 value text offsets
///     key text, value text: NUL-terminated strings
///     u32 magic again
///
/// Every way becomes one map entity whose tags are the way's tags
/// (CMapEntityWay, 0x82d91c60); the profiles filter on them (road_type).
struct NavFile {
    std::vector<NavNode> nodes;
    std::vector<NavWay> ways;
    /// The value strings in the file's order.
    std::vector<std::string> values;

    /// Throws DataError when the file does not have this layout.
    static NavFile parse(ByteView data, const std::string& source);
};

/// One drawing rule of a map profile group (a `<Symbolizer>`).
struct MapSymbolizer {
    std::string type;
    int sortOrder = 0;
    /// `<Size>` (a fixed size) or `<DynamicSize>` (grows with distance from
    /// the camera; parsed at 0x82da8ee0).
    float size = 0.0F;
    bool dynamicSize = false;
    bool hasSize = false;
    /// ARGB colours, 0 to 255 per channel.
    std::optional<glm::vec4> primary;
    std::optional<glm::vec4> secondary;
    /// Atlas cell and grid of an icon texture.
    int atlasX = 0;
    int atlasY = 0;
    int atlasColumns = 1;
    int atlasRows = 1;
    /// Game path of the texture, relative to media/ui/textures.
    std::string texture;
    /// `<PlacementRatio>`: where along a line entity an icon goes, 0 at its
    /// first point and 1 at its last.
    std::optional<float> placementRatio;
};

/// The colour, channels 0 to 1, a line symbolizer draws a point with when
/// its entity's colour source gives the point `ratio` (0x82da8200 per
/// point, lerping with 0x82da0408). Verified: the symbolizer factory
/// (0x82d98710) sets the coloured flag (+242) for line_coloured,
/// line_coloured_noalpha, line_fade_l, line_fade_r and line_fade_lr, and
/// such a line lerps each channel from the primary colour to the secondary
/// one, truncating the result; other lines, and entities without a colour
/// source (ratio 0), keep the primary colour. A missing primary colour is
/// opaque white and a missing secondary opaque black (0x82daa278). Road
/// ways take their ratios from MapCoverage::wayRatios.
///
/// Missing: the profile loader (0x82da9100) passes each colour through
/// 0x82cf46b0, a per-channel curve (0x8247c8e0) whose formula was not
/// worked out; the colours here are the profile's values as written.
glm::vec4 lineColour(const MapSymbolizer& symbolizer, float ratio);

/// A `<Group>`: its symbolizers apply to entities whose tag matches the
/// filter.
struct MapGroup {
    std::string name;
    std::vector<MapSymbolizer> symbolizers;
    std::string filterTag;
    std::string filterValue;
};

struct MapProfile {
    std::vector<MapGroup> groups;

    /// The profile named `name` from a MapRenderProfiles document. Throws
    /// DataError when the document or profile is missing.
    static MapProfile parse(std::string_view xml, const std::string& source, std::string_view name);
};

/// The sat-nav camera's tunables (UI/Sat_Nav/* in
/// media/gametunablesettings/globalregistry.xml, read by 0x82584fc0).
struct SatNavTuning {
    float carSpeedMin = 0.0F;
    float carSpeedMax = 0.0F;
    float distanceMin = 0.0F;
    float distanceMax = 0.0F;
    float elevationMinDegrees = 0.0F;
    float elevationMaxDegrees = 0.0F;
    float fovMinDegrees = 0.0F;
    float fovMaxDegrees = 0.0F;
    float playerViewportY = 0.0F;

    /// Throws DataError when a value is missing.
    static SatNavTuning load(const Vfs& vfs);
};

/// Something the map shows at a place: an entity whose tag `key` has the
/// value `value`, which the profile's group filters match (activity_type
/// "race", vehicle_type "ai", ...).
struct MapMarker {
    std::string key;
    std::string value;
    /// World x and z.
    glm::vec2 position{0.0F};
    /// Radians, 0 facing +z, increasing towards +x: the direction "icon"
    /// symbolizers turn with (vehicles).
    float heading = 0.0F;
};

/// A line the map shows, matched against the profile's group filters like
/// a marker: {"type", "route"} for the sat-nav route (CMapEntityRoute),
/// {"type", "track"} for a race's course (CMapEntityTrack).
struct MapLine {
    std::string key;
    std::string value;
    /// World x and z, in order.
    std::vector<glm::vec2> points;
};

/// The activity_type a career event's map entity carries, by its
/// Events.CareerEventStyle (0x8250b9c8 via 0x82534cb0): 1, 4 and 6 "race",
/// 2 "streetrace", 3 "exhibition", 5 "nemesisrace"; empty for the others.
std::string_view careerEventActivityType(int careerEventStyle);

/// The fixed places the activity system puts on the map: every
/// `<TriggerZone>` with a mapTag in the track's media/gamemodes/<track>/*.xml
/// activities, at its object's position in Ribbon_00/GameObjs.xml, tagged
/// activity_type = mapTag (the profile's filter values are the mapTag
/// values: workshop, paintshop, autoshow, carclub, dlccenter, racecentral,
/// streetrace). Commented-out activities are skipped, as the game's parser
/// does; so are barn finds, which the game only marks once a hint has named
/// them (CMapEntitySourceBarnFindHint). Zones sharing an object and tag
/// appear once. Throws DataError when GameObjs.xml cannot be read.
std::vector<MapMarker> loadActivityMarkers(const Vfs& vfs, const std::string& track);

/// The camera the map is drawn through for one frame.
struct MinimapCamera {
    glm::vec3 eye{0.0F};
    glm::vec3 right{1.0F, 0.0F, 0.0F};
    glm::vec3 up{0.0F, 1.0F, 0.0F};
    glm::vec3 forward{0.0F, 0.0F, 1.0F};
    /// cot(fov / 2) across and up.
    float scaleX = 1.0F;
    float scaleY = 1.0F;
    float distance = 0.0F;

    /// The camera 0x82584fc0 and 0x82da2388 set up: `speed` in metres per
    /// second picks the distance, elevation and field of view between the
    /// tunables' ends; the camera looks along the car's heading, tilted up
    /// from straight down by the elevation, from that distance, and is then moved along
    /// the ground so the car lands at the tuned height in the viewport
    /// (0x82da0fe8). `aspect` is the viewport's height over its width, which
    /// the projection divides the across scale by.
    static MinimapCamera follow(
        const SatNavTuning& tuning, glm::vec2 position, float heading, float speed, float aspect);

    /// Depth along the view direction of a world point.
    float depth(const glm::vec3& point) const { return glm::dot(point - eye, forward); }
};

/// Draws the minimap into its own texture.
class MinimapRenderer {
public:
    /// `track` names the folder under media/tracks ("colorado"). Throws
    /// DataError when the network, profile or tunables cannot be read.
    MinimapRenderer(const Vfs& vfs, UiRenderer& renderer, const std::string& track);
    ~MinimapRenderer();
    MinimapRenderer(const MinimapRenderer&) = delete;
    MinimapRenderer& operator=(const MinimapRenderer&) = delete;

    /// Renders the map around the player into a `size` x `size` texture and
    /// returns it, with the track's activity markers and `markers` (race
    /// starts, other cars) drawn by the profile's icon symbolizers. Leaves
    /// the framebuffer and viewport as it found them.
    GLuint render(glm::vec2 position, float heading, float speedMetresPerSecond, int size,
        const std::vector<MapMarker>& markers = {}, const std::vector<MapLine>& lines = {});

    /// The activity markers loaded for the track.
    const std::vector<MapMarker>& activityMarkers() const { return m_activityMarkers; }

    /// The roads driven, which colour the road lines (see lineColour);
    /// null draws every road undriven. The coverage must be of the same
    /// network and outlive its use here.
    void setCoverage(const MapCoverage* coverage);

private:
    struct Vertex {
        glm::vec3 position;
        glm::vec2 uv;
        glm::vec4 color;
    };
    /// Geometry of one symbolizer applied to every entity it matches.
    struct Batch {
        int sortOrder = 0;
        /// 0 line, 1 line with the soft fade on both sides, 2 textured icon.
        int technique = 0;
        std::size_t first = 0;
        std::size_t count = 0;
        const UiTexture* texture = nullptr;
    };

    /// A line through `points` with the colour of each point in `colors`.
    void appendLine(
        const std::vector<glm::vec2>& points, float halfWidth, const std::vector<glm::vec4>& colors, bool caps);
    /// The colour of each point of way `way` under a road symbolizer.
    std::vector<glm::vec4> roadColours(const MapSymbolizer& symbolizer, std::size_t way) const;
    /// A quad centred on `centre` spanning `across` and `up` (half extents)
    /// with the symbolizer's atlas cell, the cell's top towards `up`.
    void appendIcon(const glm::vec3& centre, const glm::vec3& across, const glm::vec3& up,
        const MapSymbolizer& symbolizer, const glm::vec4& color);
    void ensureTarget(int size);
    void buildRoads();

    UiRenderer& m_renderer;
    NavFile m_nav;
    std::vector<MapMarker> m_activityMarkers;
    MapProfile m_profile;
    SatNavTuning m_tuning;
    UiProgram m_program;
    GLuint m_vao = 0;
    GLuint m_vbo = 0;
    std::size_t m_vboCapacity = 0;
    GLuint m_framebuffer = 0;
    GLuint m_color = 0;
    int m_size = 0;
    GLint m_viewProjection = -1;
    GLint m_technique = -1;
    GLint m_texture = -1;
    std::vector<Vertex> m_vertices;
    std::vector<Batch> m_batches;
    std::vector<Batch> m_roadBatches;
    std::size_t m_roadVertexCount = 0;
    bool m_uploadedRoads = false;
    const MapCoverage* m_coverage = nullptr;
    /// The coverage revision the road geometry was built for.
    std::optional<std::uint64_t> m_coverageRevision;
};

} // namespace fh::ui
