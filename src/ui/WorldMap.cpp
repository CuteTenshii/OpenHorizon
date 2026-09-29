#include "WorldMap.h"

#include "AnarkFormats.h"
#include "Presentation.h"
#include "UiRenderer.h"
#include "VectorFont.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <unordered_map>

namespace fh::ui {

namespace {

constexpr const char* kScene = "media/ui/scenes/ui4/h_map";
constexpr const char* kProfilePath = "media/ui/MapProfileFullscreen.xml";
constexpr const char* kProfileName = "fullscreen";
constexpr const char* kRegistryPath = "media/gametunablesettings/globalregistry.xml";
constexpr const char* kHelpButtons = "HelpButtons";

/// The view record the camera fills (0x82587418): elevation 0, a field of
/// view of pi/2, and the camera's clip planes as the view record's defaults
/// (0x82da0f58, the same as the minimap's).
constexpr float kFieldOfView = std::numbers::pi_v<float> / 2.0F;
constexpr float kNear = 10.0F;
constexpr float kFar = 10000.0F;

/// The width the view-edge clamp gives the view per unit of camera
/// distance (0x82584b38 multiplies by the constant at 0x8201af00, 16/9).
constexpr float kViewWidthPerHeight = 16.0F / 9.0F;

/// Spring steps (the step at 0x8201ae34, 1/300 s).
constexpr float kSpringStep = 1.0F / 300.0F;

/// Size scaling of <DynamicSize> (0x82da8ee0, as in the minimap): the
/// size times 0.75 + 0.25 * depth / 200.
constexpr float kDynamicSizeBase = 0.75F;
constexpr float kDynamicSizeReference = 200.0F;

/// The map entity of the player's own car (CMapEntityVehiclePlayerLocal).
constexpr const char* kVehicleTag = "vehicle_type";
constexpr const char* kPlayerValue = "player_local";

/// The route line's entity (CMapRoute) and the network's cursor area.
constexpr const char* kRouteTag = "type";
constexpr const char* kRouteValue = "route";
constexpr const char* kAreaTag = "area_type";
constexpr const char* kCursorArea = "cursor_area";

/// The map shaders: the minimap's port of the executable's map effect
/// (RenderMap_Line, RenderMap_Line_FadeLeftRight, RenderMap_Normal; see
/// Minimap.cpp), which the full-screen renderer shares.
constexpr const char* kVertexShader = R"(#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUv;
layout(location = 2) in vec4 aColor;
uniform mat4 uViewProjection;
out vec2 vUv;
out vec4 vColor;
void main()
{
    gl_Position = uViewProjection * vec4(aPosition, 1.0);
    vUv = aUv;
    vColor = aColor;
}
)";

constexpr const char* kFragmentShader = R"(#version 450 core
in vec2 vUv;
in vec4 vColor;
uniform int uTechnique;
layout(binding = 0) uniform sampler2D uTexture;
out vec4 oColor;
void main()
{
    if (uTechnique == 2) {
        vec4 texel = texture(uTexture, vUv);
        vec3 linear = texel.rgb * texel.rgb * vColor.rgb;
        oColor = vec4(sqrt(linear), texel.a * vColor.a);
        return;
    }
    float along = clamp(-vUv.x, 0.0, 1.0);
    float across = vUv.y;
    float dist = sqrt(along * along + across * across);
    vec2 gradAlong = vec2(dFdx(along), dFdy(along));
    vec2 gradAcross = vec2(dFdx(across), dFdy(across));
    float inverseGradient = inversesqrt(dot(gradAlong, gradAlong) + dot(gradAcross, gradAcross));
    float thin = clamp(0.5 * inverseGradient, 0.0, 1.0);
    float edge = clamp((1.0 - 2.0 * dist) * 0.5 * inverseGradient + 0.5, 0.0, 1.0);
    float coverage = edge;
    if (uTechnique == 1) {
        float band = 0.75 * clamp(2.0 - 2.0 * dist, 0.0, 1.0);
        coverage = edge * (1.0 - band) + band;
    }
    oColor = vec4(sqrt(vColor.rgb), vColor.a * thin * coverage);
}
)";

std::string lower(std::string text)
{
    std::transform(
        text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// A profile animation: frames at times (the running sum of each frame's
/// advance, in seconds) with the values of the keys they set.
struct MapAnimation {
    enum class Loop { None, Loop, PingPong };
    Loop loop = Loop::None;
    struct Key {
        float time = 0.0F;
        float value = 0.0F;
    };
    std::unordered_map<std::string, std::vector<Key>> tracks;
    float duration = 0.0F;

    /// The value of key `type` at `seconds`: linear between the frames
    /// that set it, holding the ends. The keys' ease attribute is not
    /// applied (its curves were not traced).
    std::optional<float> value(const std::string& type, float seconds) const
    {
        const auto it = tracks.find(type);
        if (it == tracks.end() || it->second.empty()) {
            return std::nullopt;
        }
        float t = seconds;
        if (duration > 0.0F && loop == Loop::Loop) {
            t = std::fmod(seconds, duration);
        } else if (duration > 0.0F && loop == Loop::PingPong) {
            t = std::fmod(seconds, 2.0F * duration);
            if (t > duration) {
                t = 2.0F * duration - t;
            }
        }
        const std::vector<Key>& keys = it->second;
        if (t <= keys.front().time) {
            return keys.front().value;
        }
        for (std::size_t i = 1; i < keys.size(); ++i) {
            if (t <= keys[i].time) {
                const float span = keys[i].time - keys[i - 1].time;
                const float f = span > 0.0F ? (t - keys[i - 1].time) / span : 1.0F;
                return keys[i - 1].value + (keys[i].value - keys[i - 1].value) * f;
            }
        }
        return keys.back().value;
    }
};

/// A symbolizer with what the full-screen profile adds to the minimap's.
struct FullSymbolizer {
    MapSymbolizer base;
    /// Animations by event ("default", "intro", "hover", ...).
    std::unordered_map<std::string, std::string> animations;
    glm::vec3 dynamicOffset{0.0F};
    glm::vec3 finalOffset{0.0F};
    bool clamp = false;
    bool clip = false;
    std::optional<float> placementRatio;
    /// Tiles symbolizers.
    int tilesAcross = 1;
    int tilesDown = 1;
    float tilesSize = 0.0F;
    glm::vec3 offset{0.0F};
    std::optional<glm::vec4> visibleRange;
};

struct FullGroup {
    std::string name;
    std::string filterTag;
    std::string filterValue;
    std::vector<FullSymbolizer> symbolizers;
};

struct FullProfile {
    std::vector<FullGroup> groups;
    std::unordered_map<std::string, MapAnimation> animations;
};

float floatAttribute(const pugi::xml_node& node, const char* name, float fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? attribute.as_float(fallback) : fallback;
}

std::optional<glm::vec4> colourOf(const pugi::xml_node& node)
{
    if (!node) {
        return std::nullopt;
    }
    return glm::vec4(floatAttribute(node, "r", 0.0F), floatAttribute(node, "g", 0.0F), floatAttribute(node, "b", 0.0F),
        floatAttribute(node, "a", 255.0F));
}

glm::vec3 vectorOf(const pugi::xml_node& node)
{
    return {floatAttribute(node, "x", 0.0F), floatAttribute(node, "y", 0.0F), floatAttribute(node, "z", 0.0F)};
}

FullProfile parseProfile(std::string_view xml)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        throw DataError("{}: not XML", kProfilePath);
    }
    for (const pugi::xml_node profileNode : document.child("MapRenderProfiles").children("Profile")) {
        if (std::string_view(profileNode.attribute("name").as_string()) != kProfileName) {
            continue;
        }
        FullProfile profile;
        for (const pugi::xml_node animationNode : profileNode.child("animations").children("animation")) {
            MapAnimation animation;
            const std::string_view loop = animationNode.attribute("loop").as_string();
            animation.loop = loop == "loop" ? MapAnimation::Loop::Loop
                : loop == "pingpong"        ? MapAnimation::Loop::PingPong
                                            : MapAnimation::Loop::None;
            float time = 0.0F;
            for (const pugi::xml_node frame : animationNode.children("frame")) {
                time += floatAttribute(frame, "advance", 0.0F);
                for (const pugi::xml_node key : frame.children("key")) {
                    animation.tracks[key.attribute("type").as_string()].push_back(
                        {time, floatAttribute(key, "value", 0.0F)});
                }
            }
            animation.duration = time;
            // A name used twice ("pulse") keeps its first definition, as a
            // lookup by name finds.
            profile.animations.emplace(animationNode.attribute("name").as_string(), std::move(animation));
        }
        for (const pugi::xml_node groupNode : profileNode.children("Group")) {
            FullGroup group;
            group.name = groupNode.attribute("name").as_string();
            const pugi::xml_node filter = groupNode.child("Filter");
            group.filterTag = filter.attribute("tag").as_string();
            group.filterValue = filter.attribute("value").as_string();
            for (const pugi::xml_node node : groupNode.children("Symbolizer")) {
                FullSymbolizer symbolizer;
                MapSymbolizer& base = symbolizer.base;
                base.type = node.attribute("type").as_string();
                base.sortOrder = node.attribute("sort_order").as_int();
                if (const pugi::xml_node size = node.child("Size")) {
                    base.size = floatAttribute(size, "value", 0.0F);
                    base.hasSize = true;
                } else if (const pugi::xml_node dynamic = node.child("DynamicSize")) {
                    base.size = floatAttribute(dynamic, "value", 0.0F);
                    base.dynamicSize = true;
                    base.hasSize = true;
                }
                base.primary = colourOf(node.child("ColourPrimary"));
                base.secondary = colourOf(node.child("ColourSecondary"));
                if (const pugi::xml_node atlas = node.child("Atlas")) {
                    base.atlasX = atlas.attribute("x").as_int();
                    base.atlasY = atlas.attribute("y").as_int();
                    base.atlasColumns = std::max(1, atlas.attribute("x_slots").as_int(1));
                    base.atlasRows = std::max(1, atlas.attribute("y_slots").as_int(1));
                }
                base.texture = node.child("Texture").attribute("value").as_string();
                for (const pugi::xml_node child : node.children()) {
                    const std::string_view name = child.name();
                    if (name == "Animation" || name == "animation") {
                        symbolizer.animations.emplace(
                            child.attribute("event").as_string(), child.attribute("animation").as_string());
                    }
                }
                symbolizer.dynamicOffset = vectorOf(node.child("DynamicOffset"));
                symbolizer.finalOffset = vectorOf(node.child("DynamicFinalOffset"));
                if (const pugi::xml_node clamping = node.child("Clamping")) {
                    symbolizer.clamp = clamping.attribute("clamp").as_bool();
                    symbolizer.clip = clamping.attribute("clip").as_bool();
                }
                if (const pugi::xml_node ratio = node.child("PlacementRatio")) {
                    symbolizer.placementRatio = floatAttribute(ratio, "value", 0.0F);
                }
                if (const pugi::xml_node tiles = node.child("Tiles")) {
                    symbolizer.tilesAcross = std::max(1, tiles.attribute("horizontal").as_int(1));
                    symbolizer.tilesDown = std::max(1, tiles.attribute("vertical").as_int(1));
                }
                symbolizer.tilesSize = base.size;
                symbolizer.offset = vectorOf(node.child("Offset"));
                if (const pugi::xml_node range = node.child("VisibleRange")) {
                    symbolizer.visibleRange = glm::vec4(floatAttribute(range, "min_distance", 0.0F),
                        floatAttribute(range, "min_fade_distance", 0.0F), floatAttribute(range, "max_distance", 0.0F),
                        floatAttribute(range, "max_fade_distance", 0.0F));
                }
                group.symbolizers.push_back(std::move(symbolizer));
            }
            profile.groups.push_back(std::move(group));
        }
        return profile;
    }
    throw DataError("{}: no profile \"{}\"", kProfilePath, kProfileName);
}

enum class IconKind { None, Entity, Upright };

/// How a symbolizer is drawn here. "icon" lies on the ground turned with
/// its entity (engine mode 0, as in the minimap). The upright kinds face
/// the camera; the full map's camera never turns (north up), so "up" (the
/// engine's icon_up, whose angle source was not traced) and "facing" both
/// come out screen-upright. The career variants that follow progress
/// (colour, finish, new, note), the online vehicles and the splat are left
/// out.
IconKind iconKind(std::string_view type)
{
    if (type == "icon") {
        return IconKind::Entity;
    }
    if (type == "icon_up" || type == "icon_facing" || type == "icon_up_career" || type == "icon_facing_career"
        || type == "icon_career" || type == "icon_ai_vehicle" || type == "icon_festival_vehicle") {
        return IconKind::Upright;
    }
    return IconKind::None;
}

int lineTechnique(std::string_view type)
{
    if (type == "line" || type == "line_coloured" || type == "line_coloured_noalpha" || type == "line_nocaps") {
        return 0;
    }
    if (type == "line_fade_lr") {
        return 1;
    }
    return -1;
}

glm::vec4 colourFor(const MapSymbolizer& symbolizer)
{
    return symbolizer.primary.value_or(glm::vec4(255.0F)) / 255.0F;
}

/// The texture files of a tiles symbolizer's image: "<name>-<column>-<row>"
/// next to it, as the disc holds them (high-0-0 to high-4-2, low-0-0).
std::string tilePath(const std::string& texture, int column, int row)
{
    std::string path = lower("media/ui/textures/" + texture);
    std::replace(path.begin(), path.end(), '\\', '/');
    const std::size_t dot = path.rfind('.');
    return std::format("{}-{}-{}.xds", path.substr(0, dot), column, row);
}

std::string texturePath(const std::string& texture)
{
    std::string path = lower("media/ui/textures/" + texture);
    std::replace(path.begin(), path.end(), '\\', '/');
    return texturePathForScene(path);
}

} // namespace

WorldMapTuning WorldMapTuning::load(const Vfs& vfs)
{
    const Bytes data = vfs.readOrThrow(kRegistryPath);
    pugi::xml_document document;
    if (!document.load_buffer(data.data(), data.size())) {
        throw DataError("{}: not XML", kRegistryPath);
    }
    std::unordered_map<std::string, float> values;
    for (const pugi::xml_node value : document.child("Registry").children("Value")) {
        values.emplace(value.attribute("id").as_string(), value.attribute("value").as_float());
    }
    const auto get = [&](const char* id) {
        const auto it = values.find(std::string("UI/Map/") + id);
        if (it == values.end()) {
            throw DataError("{}: no UI/Map/{}", kRegistryPath, id);
        }
        return it->second;
    };
    WorldMapTuning tuning;
    tuning.distanceMin = get("Map_Distance_Min");
    tuning.distanceMax = get("Map_Distance_Max");
    tuning.cursorSpeedMin = get("Cursor_Speed_Min");
    tuning.cursorSpeedMax = get("Cursor_Speed_Max");
    tuning.cursorZoomMin = get("Cursor_Zoom_Min");
    tuning.cursorZoomMax = get("Cursor_Zoom_Max");
    tuning.zoomSpeed = get("Zoom_Speed");
    tuning.zoomDefault = get("Zoom_Default");
    tuning.inputAccelerationStart = get("Input_Acceleration_Start");
    tuning.inputAccelerationTime = get("Input_Acceleration_Time");
    tuning.inputLinearity = get("Input_Linearity");
    tuning.positionSpring = get("Position_Inertia_Spring");
    tuning.positionDamping = get("Position_Inertia_Damping");
    tuning.zoomSpring = get("Zoom_Inertia_Spring");
    tuning.zoomDamping = get("Zoom_Inertia_Damping");
    tuning.deadArea = get("Dead_Area");
    tuning.viewEdgeLeft = get("ViewEdgeLeft");
    tuning.viewEdgeRight = get("ViewEdgeRight");
    tuning.viewEdgeTop = get("ViewEdgeTop");
    tuning.viewEdgeBottom = get("ViewEdgeBottom");
    tuning.contextSnapSize = get("contextSnapSize");
    return tuning;
}

float WorldMapCamera::Shaper::shape(float input, float dt, const WorldMapTuning& tuning)
{
    const float sign = input >= 0.0F ? 1.0F : -1.0F;
    const float magnitude = sign * input;
    const float previousSign = previous >= 0.0F ? 1.0F : -1.0F;
    if (magnitude != 0.0F && previousSign == sign) {
        held += dt;
    } else {
        held = 0.0F;
    }
    const float shaped = std::pow(magnitude, tuning.inputLinearity);
    float out = 1.0F;
    if (tuning.inputAccelerationTime != 0.0F) {
        const float start = tuning.inputAccelerationStart;
        const float acceleration = held / tuning.inputAccelerationTime * (1.0F - start) + start;
        out = std::max(0.0F, std::min(acceleration, shaped));
    }
    previous = input;
    return out * sign;
}

float WorldMapCamera::Spring::step(float target, float dt, float stiffness, float damping)
{
    carry += dt;
    while (carry > kSpringStep) {
        carry -= kSpringStep;
        velocity += (stiffness * (target - position) - damping * velocity) * kSpringStep;
        position += velocity * kSpringStep;
    }
    return position;
}

WorldMapCamera::WorldMapCamera(const WorldMapTuning& tuning, glm::vec2 cursorMin, glm::vec2 cursorMax)
    : m_tuning(tuning)
    , m_cursorMin(cursorMin)
    , m_cursorMax(cursorMax)
{
    reset(glm::vec2(0.0F));
}

float WorldMapCamera::distanceFor(float zoom) const
{
    return m_tuning.distanceMin + (m_tuning.distanceMax - m_tuning.distanceMin) * zoom;
}

glm::vec2 WorldMapCamera::keepInView(glm::vec2 target, float distance) const
{
    const float halfWidth = distance * kViewWidthPerHeight;
    const glm::vec2 low(m_tuning.viewEdgeLeft + halfWidth, m_tuning.viewEdgeBottom + distance);
    const glm::vec2 high(m_tuning.viewEdgeRight - halfWidth, m_tuning.viewEdgeTop - distance);
    return glm::min(glm::max(target, low), high);
}

void WorldMapCamera::reset(glm::vec2 position)
{
    m_moveX = {};
    m_moveY = {};
    m_zoomInput = {};
    m_cursor = glm::clamp(position, m_cursorMin, m_cursorMax);
    m_target = m_cursor;
    m_zoom = m_tuning.zoomDefault;
    m_zoomSpring = {0.0F, m_zoom, 0.0F};
    m_distance = distanceFor(m_zoom);
    m_target = keepInView(m_target, m_distance);
    m_x = {0.0F, m_target.x, 0.0F};
    m_z = {0.0F, m_target.y, 0.0F};
    m_position = keepInView(m_target, m_distance);
}

void WorldMapCamera::update(glm::vec2 move, float zoom, float dt)
{
    // The cursor (0x82584bd8).
    const float x = m_moveX.shape(move.x, dt, m_tuning);
    const float y = m_moveY.shape(move.y, dt, m_tuning);
    const float z = m_zoomInput.shape(zoom, dt, m_tuning);
    const float speed = m_tuning.cursorSpeedMin + (m_tuning.cursorSpeedMax - m_tuning.cursorSpeedMin) * m_zoom;
    m_cursor += glm::vec2(x, y) * speed * dt;
    m_cursor = glm::clamp(m_cursor, m_cursorMin, m_cursorMax);
    m_zoom = std::clamp(m_zoom - z * m_tuning.zoomSpeed * dt, m_tuning.cursorZoomMin, m_tuning.cursorZoomMax);

    // The camera (0x82587418).
    const float targetDistance = distanceFor(m_zoom);
    const glm::vec2 offset = m_cursor - m_target;
    const float length = glm::length(offset);
    const float excess = length - m_tuning.deadArea * targetDistance;
    if (excess > 0.0F && length > 0.0F) {
        m_target += offset / length * excess;
    }
    const float smoothedZoom = m_zoomSpring.step(m_zoom, dt, m_tuning.zoomSpring, m_tuning.zoomDamping);
    m_distance = distanceFor(smoothedZoom);
    m_target = keepInView(m_target, m_distance);
    const glm::vec2 smoothed(m_x.step(m_target.x, dt, m_tuning.positionSpring, m_tuning.positionDamping),
        m_z.step(m_target.y, dt, m_tuning.positionSpring, m_tuning.positionDamping));
    m_position = keepInView(smoothed, m_distance);
}

struct WorldMap::Impl {
    Impl(const Vfs& vfs, const std::string& track, const std::string& language);
    ~Impl();
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    struct Vertex {
        glm::vec3 position;
        glm::vec2 uv;
        glm::vec4 color;
    };
    struct Batch {
        int sortOrder = 0;
        int technique = 0;
        std::size_t first = 0;
        std::size_t count = 0;
        const UiTexture* texture = nullptr;
    };

    void bindScene();
    void showScene();
    void updateHelp();
    /// The place of marker `index`: the track's activity markers first,
    /// then the state's.
    glm::vec2 markerPosition(std::size_t index) const
    {
        return index < activityMarkers.size() ? activityMarkers[index].position
                                              : state.markers[index - activityMarkers.size()].marker.position;
    }
    /// The viewport model's rectangle in scene units: centre and half size.
    std::pair<glm::vec2, glm::vec2> viewportRect() const;
    void updateHover();
    void placeOverlay(int width, int height);
    GLuint renderMap(int width, int height);
    void buildFrame();
    void appendLine(const std::vector<glm::vec2>& points, float halfWidth, const glm::vec4& color, bool caps);
    void appendQuad(const glm::vec3& centre, const glm::vec3& across, const glm::vec3& up, glm::vec2 uv0, glm::vec2 uv1,
        const glm::vec4& color);
    void ensureTarget(int width, int height);
    /// World point to viewport coordinates, -1 to 1, y up.
    glm::vec2 project(glm::vec2 point, float aspect) const;

    UiStrings strings;
    FontLibrary fonts;
    Presentation scene;
    UiRenderer renderer;
    NavFile nav;
    FullProfile profile;
    WorldMapTuning tuning;
    std::unique_ptr<WorldMapCamera> camera;
    std::vector<MapMarker> activityMarkers;

    int rootContract = -1;
    int overlayContract = -1;
    int cursor = -1;
    int cursorSnap = -1;
    int contextPopup = -1;
    int popupTitle = -1;
    int viewportModel = -1;
    int viewportMaterial = -1;
    HelpBar help;

    UiProgram program;
    GLint viewProjectionLocation = -1;
    GLint techniqueLocation = -1;
    GLuint vao = 0;
    GLuint vbo = 0;
    std::size_t vboCapacity = 0;
    GLuint framebuffer = 0;
    GLuint colour = 0;
    glm::ivec2 targetSize{0};
    std::vector<Vertex> vertices;
    std::vector<Batch> batches;

    WorldMapState state;
    bool open = false;
    float time = 0.0F;
    std::optional<glm::vec2> waypoint;
    std::optional<std::size_t> hovered;
    std::optional<std::size_t> helpHovered;
    bool helpShown = false;
    bool helpWaypointHere = false;
};

WorldMap::Impl::Impl(const Vfs& vfs, const std::string& track, const std::string& language)
    : strings(vfs, language)
    , fonts(vfs, language)
    , scene(SceneFiles::load(vfs, kScene))
    , renderer(vfs, fonts)
{
    const std::string navPath = std::format("media/tracks/{}/{}.nav", track, track);
    nav = NavFile::parse(vfs.readOrThrow(navPath), navPath);
    const Bytes profileData = vfs.readOrThrow(kProfilePath);
    profile = parseProfile(std::string_view(reinterpret_cast<const char*>(profileData.data()), profileData.size()));
    tuning = WorldMapTuning::load(vfs);
    try {
        activityMarkers = loadActivityMarkers(vfs, track);
    } catch (const DataError& e) {
        logWarning("map: no activity markers: {}", e.what());
    }

    // The cursor's limits: the cursor_area way's bounding box (0x825840e0).
    glm::vec2 low(std::numeric_limits<float>::max());
    glm::vec2 high(std::numeric_limits<float>::lowest());
    for (const NavWay& way : nav.ways) {
        if (way.tag(kAreaTag) == kCursorArea) {
            for (const glm::vec2& point : way.points) {
                low = glm::min(low, point);
                high = glm::max(high, point);
            }
        }
    }
    if (low.x > high.x) {
        throw DataError("{}: no cursor_area way", navPath);
    }
    camera = std::make_unique<WorldMapCamera>(tuning, low, high);

    program = UiProgram("worldmap", kVertexShader, kFragmentShader);
    viewProjectionLocation = program.location("uViewProjection");
    techniqueLocation = program.location("uTechnique");
    glCreateVertexArrays(1, &vao);
    for (GLuint attribute = 0; attribute < 3; ++attribute) {
        glEnableVertexArrayAttrib(vao, attribute);
        glVertexArrayAttribBinding(vao, attribute, 0);
    }
    glVertexArrayAttribFormat(vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribFormat(vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribFormat(vao, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    bindScene();
}

WorldMap::Impl::~Impl()
{
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &colour);
}

void WorldMap::Impl::bindScene()
{
    rootContract = scene.contract(0, "UIContract");
    const int overlay = scene.contractParameter(rootContract, "COMPONENT_MAP_OVERLAY");
    overlayContract = scene.contract(overlay, "MAP_OVERLAY");
    if (overlayContract < 0) {
        throw DataError("{}: no MAP_OVERLAY contract", kScene);
    }
    cursor = scene.contractParameter(overlayContract, "COMPONENT_CURSOR");
    cursorSnap = scene.contractParameter(overlayContract, "COMPONENT_CURSOR_SNAP");
    contextPopup = scene.contractParameter(overlayContract, "COMPONENT_CONTEXT_POPUP");
    popupTitle = scene.contractParameter(overlayContract, "TEXT_POPUP_TITLE");
    int rendererContract = -1;
    for (std::size_t i = 0; i < scene.elementCount() && rendererContract < 0; ++i) {
        rendererContract = scene.contract(static_cast<int>(i), "CUSTREND_MAPRENDERER");
    }
    viewportModel = scene.contractParameter(rendererContract, "MODEL_VIEWPORT");
    viewportMaterial = scene.contractParameter(rendererContract, "MATERIAL_VIEWPORT");
    help.bind(scene, scene.contractParameter(rootContract, "COMPONENT_HELP_BUTTON_BAR"));
}

void WorldMap::Impl::showScene()
{
    scene.reset();
    // The player bar (level, car, credits) and the tip line show data this
    // reimplementation does not keep; the scene's own events hide them.
    scene.fireEvent(rootContract, "HIDE_PLAYER_BAR");
    scene.fireEvent(rootContract, "HIDE_TIP");
    scene.fireEvent(overlayContract, "SHOW_CURSOR");
    helpShown = false;
    updateHelp();
}

void WorldMap::Impl::updateHelp()
{
    // 0x827a8e88: A for the item under the cursor (SET ROUTE, or CLEAR
    // ROUTE when it is the destination), B EXIT MAP, then MOVE on the left
    // stick and ZOOM on the right. The X and Y entries (fast travel,
    // leaderboards, details) and FILTER belong to systems this game does
    // not have.
    const bool waypointHere = hovered && waypoint && markerPosition(*hovered) == *waypoint;
    if (helpShown && helpHovered == hovered && helpWaypointHere == waypointHere) {
        return;
    }
    std::vector<HelpBar::Entry> entries;
    if (hovered) {
        entries.push_back(
            {HelpIcon::A, strings.get(kHelpButtons, waypointHere ? "IDS_ClearDestination" : "IDS_SetDestination")});
    }
    entries.push_back({HelpIcon::B, strings.get(kHelpButtons, "IDS_ExitMap")});
    entries.push_back({HelpIcon::LeftStickNoArrow, strings.get(kHelpButtons, "IDS_Move")});
    entries.push_back({HelpIcon::RightStickNoArrow, strings.get(kHelpButtons, "IDS_Zoom")});
    help.set(scene, entries);
    helpShown = true;
    helpHovered = hovered;
    helpWaypointHere = waypointHere;
}

glm::vec2 WorldMap::Impl::project(glm::vec2 point, float aspect) const
{
    // Straight down from the camera: across is east, up is north, both
    // scaled by cot(fov / 2) over the distance.
    const float scale = 1.0F / std::tan(kFieldOfView * 0.5F) / camera->distance();
    const glm::vec2 offset = point - camera->position();
    return {offset.x * scale * aspect, offset.y * scale};
}

void WorldMap::Impl::updateHover()
{
    // The item under the cursor: the nearest marker within contextSnapSize
    // of the cursor, taken as a fraction of the view's height (the snap's
    // own tests, UI/Map/Snappy/*, were not traced).
    const float radius = tuning.contextSnapSize * 2.0F * camera->distance();
    const glm::vec2 at = camera->cursor();
    std::optional<std::size_t> best;
    float bestDistance = radius;
    const std::size_t count = activityMarkers.size() + state.markers.size();
    for (std::size_t i = 0; i < count; ++i) {
        const MapMarker& marker
            = i < activityMarkers.size() ? activityMarkers[i] : state.markers[i - activityMarkers.size()].marker;
        if (marker.key == kVehicleTag) {
            continue;
        }
        const float d = glm::length(marker.position - at);
        if (d <= bestDistance) {
            bestDistance = d;
            best = i;
        }
    }
    if (best != hovered) {
        scene.fireEvent(overlayContract, best ? "SNAP" : "UNSNAP");
        std::string name;
        if (best && *best >= activityMarkers.size()) {
            name = state.markers[*best - activityMarkers.size()].name;
        }
        if (!name.empty()) {
            scene.setText(popupTitle, name);
            scene.goToSlide(contextPopup, "ShowPin");
        } else if (hovered) {
            scene.goToSlide(contextPopup, "HidePin");
        }
        hovered = best;
    }
}

void WorldMap::Impl::appendLine(
    const std::vector<glm::vec2>& points, float halfWidth, const glm::vec4& color, bool caps)
{
    if (points.size() < 2) {
        return;
    }
    const auto normalOf = [](glm::vec2 from, glm::vec2 to) {
        const glm::vec2 d = to - from;
        const float length = glm::length(d);
        return length > 0.0F ? glm::vec2(d.y, -d.x) / length : glm::vec2(0.0F);
    };
    const auto ground = [](glm::vec2 p) { return glm::vec3(p.x, 0.0F, p.y); };
    std::vector<Vertex> strip;
    const auto emitPair = [&](glm::vec2 centre, glm::vec2 normal, float u) {
        strip.push_back({ground(centre - normal * halfWidth), glm::vec2(u, -1.0F), color});
        strip.push_back({ground(centre + normal * halfWidth), glm::vec2(u, 1.0F), color});
    };
    const glm::vec2 firstNormal = normalOf(points[0], points[1]);
    if (caps && glm::length(points[1] - points[0]) > 0.0F) {
        emitPair(points[0] - glm::normalize(points[1] - points[0]) * halfWidth, firstNormal, -1.0F);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
        glm::vec2 normal;
        if (i == 0) {
            normal = firstNormal;
        } else if (i + 1 == points.size()) {
            normal = normalOf(points[i - 1], points[i]);
        } else {
            const glm::vec2 sum = normalOf(points[i - 1], points[i]) + normalOf(points[i], points[i + 1]);
            const float length = glm::length(sum);
            normal = length > 0.0F ? sum / length : normalOf(points[i], points[i + 1]);
        }
        emitPair(points[i], normal, 0.0F);
    }
    const std::size_t last = points.size() - 1;
    if (caps && glm::length(points[last] - points[last - 1]) > 0.0F) {
        emitPair(points[last] + glm::normalize(points[last] - points[last - 1]) * halfWidth,
            normalOf(points[last - 1], points[last]), -1.0F);
    }
    for (std::size_t i = 0; i + 3 < strip.size(); i += 2) {
        vertices.insert(
            vertices.end(), {strip[i], strip[i + 1], strip[i + 2], strip[i + 1], strip[i + 3], strip[i + 2]});
    }
}

void WorldMap::Impl::appendQuad(const glm::vec3& centre, const glm::vec3& across, const glm::vec3& up, glm::vec2 uv0,
    glm::vec2 uv1, const glm::vec4& color)
{
    const Vertex topLeft{centre + up - across, uv0, color};
    const Vertex topRight{centre + up + across, glm::vec2(uv1.x, uv0.y), color};
    const Vertex bottomLeft{centre - up - across, glm::vec2(uv0.x, uv1.y), color};
    const Vertex bottomRight{centre - up + across, uv1, color};
    vertices.insert(vertices.end(), {topLeft, topRight, bottomLeft, topRight, bottomRight, bottomLeft});
}

void WorldMap::Impl::buildFrame()
{
    vertices.clear();
    batches.clear();
    const float distance = camera->distance();
    const glm::vec3 right(1.0F, 0.0F, 0.0F);
    const glm::vec3 north(0.0F, 0.0F, 1.0F);
    const auto begin = [&](int sortOrder, int technique, const UiTexture* texture) {
        Batch batch;
        batch.sortOrder = sortOrder;
        batch.technique = technique;
        batch.texture = texture;
        batch.first = vertices.size();
        return batch;
    };
    const auto finish = [&](Batch& batch) {
        batch.count = vertices.size() - batch.first;
        if (batch.count > 0) {
            batches.push_back(batch);
        }
    };

    std::vector<MapMarker> markers = activityMarkers;
    for (const WorldMapMarker& marker : state.markers) {
        markers.push_back(marker.marker);
    }
    markers.push_back({kVehicleTag, kPlayerValue, state.playerPosition, state.playerHeading});
    // The route's own entity: the line along the route, and its icons at
    // their placement ratio along it; with no route worked out, the flag
    // at the waypoint.
    std::vector<glm::vec2> route = state.route;
    if (route.empty() && waypoint) {
        route.push_back(*waypoint);
    }

    for (const FullGroup& group : profile.groups) {
        const bool isRoute = group.filterTag == kRouteTag && group.filterValue == kRouteValue;
        for (const FullSymbolizer& symbolizer : group.symbolizers) {
            const MapSymbolizer& base = symbolizer.base;
            if (base.type == "tiles") {
                // Size is the image's north-south extent and Offset its
                // south-west corner.
                const float alpha = [&] {
                    if (!symbolizer.visibleRange) {
                        return 1.0F;
                    }
                    const glm::vec4 range = *symbolizer.visibleRange;
                    // Fading out over max_fade_distance up to max_distance
                    // (inferred from the attribute names).
                    if (range.w <= 0.0F) {
                        return distance <= range.z ? 1.0F : 0.0F;
                    }
                    return std::clamp((range.z - distance) / range.w, 0.0F, 1.0F);
                }();
                if (alpha <= 0.0F) {
                    continue;
                }
                // The tiles' texel grid gives the east-west extent: the low
                // image is 2048 x 1229 and the high one 5 x 3 tiles of 1024,
                // both 5:3, which with Size 11500 spans the Offset's -9583.3
                // to +9583.3.
                const UiTexture* first = renderer.texture(tilePath(base.texture, 0, 0));
                if (!first || first->height() <= 0) {
                    continue;
                }
                const float height = symbolizer.tilesSize;
                const float width = height * static_cast<float>(symbolizer.tilesAcross * first->width())
                    / static_cast<float>(symbolizer.tilesDown * first->height());
                const float tileWidth = width / static_cast<float>(symbolizer.tilesAcross);
                const float tileHeight = height / static_cast<float>(symbolizer.tilesDown);
                for (int column = 0; column < symbolizer.tilesAcross; ++column) {
                    for (int row = 0; row < symbolizer.tilesDown; ++row) {
                        const UiTexture* texture = renderer.texture(tilePath(base.texture, column, row));
                        if (!texture) {
                            continue;
                        }
                        Batch batch = begin(base.sortOrder, 2, texture);
                        // Row 0 is the northern edge, as the image's rows run
                        // from its top.
                        const glm::vec3 centre(symbolizer.offset.x + tileWidth * (static_cast<float>(column) + 0.5F),
                            0.0F, symbolizer.offset.z + height - tileHeight * (static_cast<float>(row) + 0.5F));
                        appendQuad(centre, right * tileWidth * 0.5F, north * tileHeight * 0.5F, glm::vec2(0.0F),
                            glm::vec2(1.0F), glm::vec4(1.0F, 1.0F, 1.0F, alpha));
                        finish(batch);
                    }
                }
                continue;
            }
            const int technique = lineTechnique(base.type);
            if (technique >= 0) {
                Batch batch = begin(base.sortOrder, technique, nullptr);
                const float halfWidth = 2.0F * base.size;
                const bool caps = base.type != "line_nocaps";
                if (isRoute) {
                    appendLine(route, halfWidth, colourFor(base), caps);
                } else if (!group.filterTag.empty()) {
                    for (const NavWay& way : nav.ways) {
                        if (way.tag(group.filterTag) == group.filterValue) {
                            appendLine(way.points, halfWidth, colourFor(base), caps);
                        }
                    }
                }
                finish(batch);
                continue;
            }
            const IconKind kind = iconKind(base.type);
            if (kind == IconKind::None || base.texture.empty() || (symbolizer.clamp && symbolizer.clip)) {
                continue;
            }
            // The icon's animations: "default" runs from the moment the map
            // opens, "intro" plays once as it opens.
            float alpha = 1.0F;
            glm::vec2 scale(1.0F);
            for (const char* event : {"default", "intro"}) {
                const auto it = symbolizer.animations.find(event);
                if (it == symbolizer.animations.end()) {
                    continue;
                }
                const auto animation = profile.animations.find(it->second);
                if (animation == profile.animations.end()) {
                    continue;
                }
                alpha *= animation->second.value("alpha", time).value_or(1.0F);
                scale.x *= animation->second.value("scaleX", time).value_or(1.0F);
                scale.y *= animation->second.value("scaleY", time).value_or(1.0F);
            }
            if (alpha <= 0.0F) {
                continue;
            }
            const UiTexture* texture = renderer.texture(texturePath(base.texture));
            if (!texture) {
                continue;
            }
            std::vector<std::pair<glm::vec2, float>> places;
            if (isRoute) {
                if (!route.empty() && symbolizer.placementRatio && *symbolizer.placementRatio >= 1.0F) {
                    places.emplace_back(route.back(), 0.0F);
                } else if (!route.empty() && symbolizer.placementRatio && *symbolizer.placementRatio <= 0.0F) {
                    places.emplace_back(route.front(), 0.0F);
                }
            } else {
                for (const MapMarker& marker : markers) {
                    if (!group.filterTag.empty() && marker.key == group.filterTag
                        && marker.value == group.filterValue) {
                        places.emplace_back(marker.position, marker.heading);
                    }
                }
            }
            if (places.empty()) {
                continue;
            }
            Batch batch = begin(base.sortOrder, 2, texture);
            const float grow = base.dynamicSize
                ? kDynamicSizeBase + (1.0F - kDynamicSizeBase) * distance / kDynamicSizeReference
                : 1.0F;
            const float halfSize = base.size * grow;
            const glm::vec2 uv0(static_cast<float>(base.atlasX) / static_cast<float>(base.atlasColumns),
                static_cast<float>(base.atlasY) / static_cast<float>(base.atlasRows));
            const glm::vec2 uv1(static_cast<float>(base.atlasX + 1) / static_cast<float>(base.atlasColumns),
                static_cast<float>(base.atlasY + 1) / static_cast<float>(base.atlasRows));
            glm::vec4 color = colourFor(base);
            color.a *= alpha;
            for (const auto& [position, heading] : places) {
                // The offsets, in the profile's units scaled as the dynamic
                // size is (inferred), x east and z north.
                const glm::vec3 shift = (symbolizer.dynamicOffset + symbolizer.finalOffset) * grow;
                const glm::vec3 centre(position.x + shift.x, 0.0F, position.y + shift.z);
                if (kind == IconKind::Entity) {
                    const glm::vec3 forward(std::sin(heading), 0.0F, std::cos(heading));
                    const glm::vec3 side(forward.z, 0.0F, -forward.x);
                    appendQuad(centre, side * halfSize * scale.x, forward * halfSize * scale.y, uv0, uv1, color);
                } else {
                    appendQuad(centre, right * halfSize * scale.x, north * halfSize * scale.y, uv0, uv1, color);
                }
            }
            finish(batch);
        }
    }
    // Lower sort orders first, as in the minimap.
    std::stable_sort(
        batches.begin(), batches.end(), [](const Batch& a, const Batch& b) { return a.sortOrder < b.sortOrder; });
}

void WorldMap::Impl::ensureTarget(int width, int height)
{
    if (targetSize == glm::ivec2(width, height) && framebuffer) {
        return;
    }
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &colour);
    glCreateTextures(GL_TEXTURE_2D, 1, &colour);
    glTextureStorage2D(colour, 1, GL_RGBA8, width, height);
    glTextureParameteri(colour, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(colour, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateFramebuffers(1, &framebuffer);
    glNamedFramebufferTexture(framebuffer, GL_COLOR_ATTACHMENT0, colour, 0);
    if (glCheckNamedFramebufferStatus(framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("map render target is incomplete");
    }
    targetSize = {width, height};
}

GLuint WorldMap::Impl::renderMap(int width, int height)
{
    width = std::max(8, width);
    height = std::max(8, height);
    ensureTarget(width, height);
    const float aspect = static_cast<float>(height) / static_cast<float>(width);
    buildFrame();

    const std::size_t bytes = vertices.size() * sizeof(Vertex);
    if (bytes > vboCapacity) {
        glDeleteBuffers(1, &vbo);
        glCreateBuffers(1, &vbo);
        vboCapacity = bytes + 4096 * sizeof(Vertex);
        glNamedBufferData(vbo, static_cast<GLsizeiptr>(vboCapacity), nullptr, GL_DYNAMIC_DRAW);
        glVertexArrayVertexBuffer(vao, 0, vbo, 0, sizeof(Vertex));
    }
    if (bytes > 0) {
        glNamedBufferSubData(vbo, 0, static_cast<GLsizeiptr>(bytes), vertices.data());
    }

    // Looking straight down (forward -y) with north up: across along +x,
    // up along +z, negated so the target's first row is the top as the UI
    // samples it; w is the height above the ground.
    const float scaleY = 1.0F / std::tan(kFieldOfView * 0.5F);
    const float scaleX = scaleY * aspect;
    const glm::vec3 eye(camera->position().x, camera->distance(), camera->position().y);
    const glm::vec3 x(scaleX, 0.0F, 0.0F);
    const glm::vec3 y(0.0F, 0.0F, -scaleY);
    const glm::vec3 forward(0.0F, -1.0F, 0.0F);
    const float zScale = (kFar + kNear) / (kFar - kNear);
    const float zOffset = -2.0F * kFar * kNear / (kFar - kNear);
    glm::mat4 viewProjection(0.0F);
    for (int column = 0; column < 3; ++column) {
        viewProjection[column][0] = x[column];
        viewProjection[column][1] = y[column];
        viewProjection[column][2] = forward[column] * zScale;
        viewProjection[column][3] = forward[column];
    }
    viewProjection[3][0] = -glm::dot(x, eye);
    viewProjection[3][1] = -glm::dot(y, eye);
    viewProjection[3][2] = -glm::dot(forward, eye) * zScale + zOffset;
    viewProjection[3][3] = -glm::dot(forward, eye);

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glViewport(0, 0, width, height);
    const float clear[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    glClearNamedFramebufferfv(framebuffer, GL_COLOR, 0, clear);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(program.id());
    glProgramUniformMatrix4fv(program.id(), viewProjectionLocation, 1, GL_FALSE, &viewProjection[0][0]);
    glBindVertexArray(vao);
    for (const Batch& batch : batches) {
        glProgramUniform1i(program.id(), techniqueLocation, batch.technique);
        if (batch.texture) {
            glBindTextureUnit(0, batch.texture->id());
            glBindSampler(0, 0);
        }
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first), static_cast<GLsizei>(batch.count));
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    return colour;
}

void WorldMap::Impl::placeOverlay(int width, int height)
{
    // The cursor, its snap ring and the name popup sit where the cursor is
    // on the map, in the scene's units (1280 x 720, y up); the scene
    // places them at its centre and the map overlay control moves them
    // (inferred from their authored positions round the origin).
    const auto [centre, half] = viewportRect();
    (void)width;
    (void)height;
    const float aspect = half.y > 0.0F ? half.y / half.x : 1.0F;
    const auto place = [&](int element, glm::vec2 point) {
        if (element < 0) {
            return;
        }
        const glm::vec2 ndc = project(point, aspect);
        scene.setAttribute(element, attr::PositionX, centre.x + ndc.x * half.x);
        scene.setAttribute(element, attr::PositionY, centre.y + ndc.y * half.y);
    };
    place(cursor, camera->cursor());
    place(contextPopup, camera->cursor());
    if (hovered) {
        place(cursorSnap, markerPosition(*hovered));
    } else {
        place(cursorSnap, camera->cursor());
    }
}

std::pair<glm::vec2, glm::vec2> WorldMap::Impl::viewportRect() const
{
    const glm::mat4& world = scene.worldTransform(viewportModel);
    glm::vec3 low(0.0F);
    glm::vec3 high(0.0F);
    const std::uint32_t id = scene.files().bgf.elements[static_cast<std::size_t>(viewportModel)].id;
    for (const FbfRecord& record : scene.files().fbf.records) {
        if (record.id == id && record.kind == FbfKind::Model && record.mesh < scene.files().fbf.meshes.size()) {
            low = scene.files().fbf.meshes[record.mesh].boundsMin;
            high = scene.files().fbf.meshes[record.mesh].boundsMax;
        }
    }
    const glm::vec3 a(world * glm::vec4(low, 1.0F));
    const glm::vec3 b(world * glm::vec4(high, 1.0F));
    return {glm::vec2(a + b) * 0.5F, glm::abs(glm::vec2(b - a)) * 0.5F};
}

WorldMap::WorldMap(const Vfs& vfs, const std::string& track, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, track, language))
{
}

WorldMap::~WorldMap() = default;

void WorldMap::open(const WorldMapState& state)
{
    Impl& impl = *m_impl;
    impl.state = state;
    impl.camera->reset(state.playerPosition);
    impl.time = 0.0F;
    impl.hovered.reset();
    impl.open = true;
    impl.showScene();
}

void WorldMap::close()
{
    m_impl->open = false;
}

bool WorldMap::isOpen() const
{
    return m_impl->open;
}

void WorldMap::setState(const WorldMapState& state)
{
    m_impl->state = state;
    if (m_impl->hovered && *m_impl->hovered >= m_impl->activityMarkers.size() + state.markers.size()) {
        m_impl->hovered.reset();
    }
}

WorldMapAction WorldMap::update(const MenuInput& input, float dt)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return WorldMapAction::None;
    }
    WorldMapAction action = WorldMapAction::None;
    impl.camera->update(input.leftStick, input.rightStick.y, dt);
    impl.time += dt;
    impl.updateHover();
    if (input.back) {
        action = WorldMapAction::Back;
    } else if (input.accept && impl.hovered) {
        const glm::vec2 at = impl.markerPosition(*impl.hovered);
        if (impl.waypoint && *impl.waypoint == at) {
            impl.waypoint.reset();
            action = WorldMapAction::ClearWaypoint;
        } else {
            impl.waypoint = at;
            action = WorldMapAction::SetWaypoint;
        }
    }
    impl.updateHelp();
    impl.scene.update(dt);
    impl.scene.takeFiredEvents();
    if (action == WorldMapAction::Back) {
        impl.open = false;
    }
    return action;
}

void WorldMap::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return;
    }
    impl.placeOverlay(width, height);
    std::vector<DrawItem> items = impl.scene.evaluate();
    // The map's target covers the viewport model's rectangle in pixels
    // (the scene's 1280 x 720 units fitted to the window).
    const float scale = std::min(static_cast<float>(width) / 1280.0F, static_cast<float>(height) / 720.0F);
    const glm::vec2 half = impl.viewportRect().second;
    const int targetWidth = static_cast<int>(half.x * 2.0F * scale);
    const int targetHeight = static_cast<int>(half.y * 2.0F * scale);
    const GLuint texture = impl.renderMap(targetWidth, targetHeight);
    impl.renderer.setTextureOverride("@worldmap", texture);
    // The renderer points the material's texture stage at its target
    // (0x82892a60); this material has no image of its own, so the target
    // becomes its one layer, mapped over the rectangle's UVs (v runs up
    // the mesh, the target's rows down). The material's diffuse colour
    // applies as it does to every textured material.
    for (DrawItem& item : items) {
        if (item.isText || item.material != impl.viewportMaterial) {
            continue;
        }
        if (item.model.layers.empty()) {
            item.model.layers.emplace_back();
        }
        item.model.layers.front().texture = "@worldmap";
        item.model.layers.front().imageName = "@worldmap";
    }
    impl.help.apply(items);
    applyTextScale(items);
    impl.renderer.begin(width, height);
    impl.renderer.draw(items);
    impl.renderer.end();
}

std::optional<glm::vec2> WorldMap::waypoint() const
{
    return m_impl->waypoint;
}

void WorldMap::clearWaypoint()
{
    m_impl->waypoint.reset();
}

const WorldMapCamera& WorldMap::camera() const
{
    return *m_impl->camera;
}

const WorldMapMarker* WorldMap::hovered() const
{
    const Impl& impl = *m_impl;
    if (!impl.hovered || *impl.hovered < impl.activityMarkers.size()) {
        return nullptr;
    }
    return &impl.state.markers[*impl.hovered - impl.activityMarkers.size()];
}

} // namespace fh::ui
