#pragma once

#include "UiRenderer.h"

#include "base/Bytes.h"
#include "render/Gl.h"

#include <glm/glm.hpp>

#include <array>
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

/// One way of a track's road network file (`<track>.nav`).
struct NavWay {
    /// Node positions on the ground plane: world x and z.
    std::vector<glm::vec2> points;
    std::vector<std::pair<std::string, std::string>> tags;

    /// The value of tag `key`, or empty.
    std::string_view tag(std::string_view key) const;
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
///       node; the second word's meaning is inferred from the data)
///     tags of {u32 key index, u32 value index}
///     u32 key text offsets, u32 value text offsets
///     key text, value text: NUL-terminated strings
///     u32 magic again
///
/// Every way becomes one map entity whose tags are the way's tags
/// (CMapEntityWay, 0x82d91c60); the profiles filter on them (road_type).
struct NavFile {
    std::vector<NavWay> ways;

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
};

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
    /// tunables' ends; the camera looks along the car's heading, pitched
    /// down by the elevation, from that distance, and is then moved along
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
    /// returns it. Leaves the framebuffer and viewport as it found them.
    GLuint render(glm::vec2 position, float heading, float speedMetresPerSecond, int size);

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

    void appendLine(const std::vector<glm::vec2>& points, float halfWidth, const glm::vec4& color, bool caps);
    void appendIcon(
        glm::vec2 centre, glm::vec2 direction, float halfSize, const MapSymbolizer& symbolizer, const glm::vec4& color);
    void ensureTarget(int size);
    void buildRoads();

    UiRenderer& m_renderer;
    NavFile m_nav;
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
};

} // namespace fh::ui
