#include "Minimap.h"

#include "AnarkFormats.h"
#include "MapCoverage.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <unordered_map>

namespace fh::ui {

namespace {

constexpr std::uint32_t kNavMagic = 0x0E177551;
constexpr const char* kProfilePath = "media/ui/MapProfileMinimap.xml";
constexpr const char* kProfileName = "minimap";
constexpr const char* kRegistryPath = "media/gametunablesettings/globalregistry.xml";

/// The camera's clip planes, the defaults the view record gets at
/// 0x82da0f58.
constexpr float kNear = 10.0F;
constexpr float kFar = 10000.0F;

/// Size scaling of the profile's size elements (0x82da8ee0): a fixed
/// `<Size>` keeps its value; a `<DynamicSize>` is scaled by
/// 0.75 + 0.25 * depth / 200.
constexpr float kDynamicSizeBase = 0.75F;
constexpr float kDynamicSizeReference = 200.0F;

/// The player's own car on the map (CMapEntityVehiclePlayerLocal).
constexpr const char* kPlayerFilterTag = "vehicle_type";
constexpr const char* kPlayerFilterValue = "player_local";

/// The line and icon pixel shaders are ports of the map effect the
/// executable embeds (techniques RenderMap_Line, RenderMap_Line_FadeLeftRight
/// and RenderMap_Normal; pixel shader microcode at 0x821b6bf8, 0x821b71f8 and
/// 0x821b62b4). Lines are ground quads with u running 0 along the line and
/// out to -1 over the round caps, v from -1 to 1 across; the line is solid
/// to half its quad's width and antialiased over about a pixel, and the
/// fading variant adds a soft band from 0.75 to 0 out to the quad's edge.
/// All three write sqrt(colour) (the render target holds linear colour).
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
        // The icon sheet's colour channels are fetched gamma-expanded (its
        // fetch constant marks them so); squaring stands in for the GPU's
        // piecewise-linear curve.
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

float attributeFloat(const pugi::xml_node& node, const char* name, float fallback)
{
    const pugi::xml_attribute attribute = node.attribute(name);
    return attribute ? attribute.as_float(fallback) : fallback;
}

std::optional<glm::vec4> colour(const pugi::xml_node& node)
{
    if (!node) {
        return std::nullopt;
    }
    return glm::vec4(attributeFloat(node, "r", 0.0F), attributeFloat(node, "g", 0.0F), attributeFloat(node, "b", 0.0F),
        attributeFloat(node, "a", 255.0F));
}

/// The technique a line symbolizer draws with; -1 for types that are not
/// lines. Which symbolizer uses which technique is inferred from their
/// names (line_fade_lr and RenderMap_Line_FadeLeftRight); the cap and colour
/// flags are the ones the symbolizer constructors set (0x82daa4d8 and its
/// siblings).
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

/// The colour of entities without a colour source (markers, and the lines
/// the game passes in): the primary colour, as lineColour gives at ratio 0.
/// Channels come out 0 to 1.
glm::vec4 colorOf(const MapSymbolizer& symbolizer)
{
    return lineColour(symbolizer, 0.0F);
}

/// The point `ratio` of the way along a line by length, heading along the
/// segment it falls on.
MapMarker pointAlong(const MapLine& line, float ratio)
{
    float total = 0.0F;
    for (std::size_t i = 1; i < line.points.size(); ++i) {
        total += glm::length(line.points[i] - line.points[i - 1]);
    }
    float remaining = std::clamp(ratio, 0.0F, 1.0F) * total;
    for (std::size_t i = 1; i < line.points.size(); ++i) {
        const glm::vec2 a = line.points[i - 1];
        const glm::vec2 b = line.points[i];
        const float length = glm::length(b - a);
        if (remaining <= length || i + 1 == line.points.size()) {
            const float t = length > 0.0F ? std::min(remaining / length, 1.0F) : 0.0F;
            const glm::vec2 d = b - a;
            return {line.key, line.value, a + d * t, std::atan2(d.x, d.y)};
        }
        remaining -= length;
    }
    return {line.key, line.value, line.points.front(), 0.0F};
}

enum class IconOrientation : std::uint8_t { None, Entity, Facing };

/// How an icon symbolizer turns, as the symbolizer factories set it: the
/// engine's (0x82d98710: "icon" 0, "icon_facing" 1) and the game's
/// (0x826cdfa0: the vehicle icons are built facing, the career icons with
/// the orientation their name gives). Only the career icons' base variant
/// is drawn: the ring colour, the finished wreath, the note and the "new"
/// badge (variants 0, 2, 3, 4) follow career progress this game does not
/// keep, and the "up" orientation takes an angle whose source was not
/// traced.
IconOrientation iconOrientation(std::string_view type)
{
    if (type == "icon") {
        return IconOrientation::Entity;
    }
    if (type == "icon_facing" || type == "icon_facing_career" || type == "icon_ai_vehicle"
        || type == "icon_festival_vehicle" || type == "icon_online_vehicle") {
        return IconOrientation::Facing;
    }
    return IconOrientation::None;
}

std::string lower(std::string text)
{
    std::transform(
        text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

} // namespace

glm::vec4 lineColour(const MapSymbolizer& symbolizer, float ratio)
{
    const glm::vec4 primary = symbolizer.primary.value_or(glm::vec4(255.0F));
    const std::string_view type = symbolizer.type;
    const bool coloured = type == "line_coloured" || type == "line_coloured_noalpha" || type == "line_fade_l"
        || type == "line_fade_r" || type == "line_fade_lr";
    if (!coloured) {
        return primary / 255.0F;
    }
    const glm::vec4 secondary = symbolizer.secondary.value_or(glm::vec4(0.0F, 0.0F, 0.0F, 255.0F));
    return glm::trunc(primary + (secondary - primary) * ratio) / 255.0F;
}

std::string_view careerEventActivityType(int careerEventStyle)
{
    switch (careerEventStyle) {
    case 1:
    case 4:
    case 6:
        return "race";
    case 2:
        return "streetrace";
    case 3:
        return "exhibition";
    case 5:
        return "nemesisrace";
    default:
        return {};
    }
}

std::vector<MapMarker> loadActivityMarkers(const Vfs& vfs, const std::string& track)
{
    const std::string objectsPath = std::format("media/tracks/{}/Ribbon_00/GameObjs.xml", track);
    const Bytes objects = vfs.readOrThrow(objectsPath);
    pugi::xml_document objectDocument;
    if (!objectDocument.load_buffer(objects.data(), objects.size())) {
        throw DataError("{}: not XML", objectsPath);
    }
    std::unordered_map<std::string, glm::vec2> positions;
    for (const pugi::xml_node object : objectDocument.child("GameObjs").children()) {
        const pugi::xml_node pos = object.child("Pos");
        positions.emplace(object.attribute("GameplayID").as_string(),
            glm::vec2(pos.attribute("x").as_float(), pos.attribute("z").as_float()));
    }

    std::vector<MapMarker> markers;
    std::vector<std::string> seen;
    for (const std::string& path : vfs.list(std::format("media/gamemodes/{}", track))) {
        if (!endsWithNoCase(path, ".xml") || path.find("/cutscenes/") != std::string::npos) {
            continue;
        }
        const std::optional<Bytes> data = vfs.read(path);
        pugi::xml_document document;
        if (!data || !document.load_buffer(data->data(), data->size())) {
            continue;
        }
        for (const pugi::xml_node activity : document.child("ActivityManager").children("Activity")) {
            if (std::string_view(activity.attribute("type").as_string()) == "ActivityBarnFind") {
                continue;
            }
            for (const pugi::xml_node zone : activity.children("TriggerZone")) {
                const std::string tag = zone.attribute("mapTag").as_string();
                const std::string object = zone.attribute("object").as_string();
                const auto it = positions.find(object);
                if (tag.empty() || it == positions.end()) {
                    continue;
                }
                const std::string key = std::format("{}/{}", object, tag);
                if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
                    continue;
                }
                seen.push_back(key);
                markers.push_back({"activity_type", tag, it->second, 0.0F});
            }
        }
    }
    return markers;
}

std::string_view navTag(const NavTags& tags, std::string_view key)
{
    for (const auto& [name, value] : tags) {
        if (name == key) {
            return value;
        }
    }
    return {};
}

NavFile NavFile::parse(ByteView data, const std::string& source)
{
    BeReader in(data, source);
    if (in.u32() != kNavMagic) {
        in.fail("not a road network");
    }
    const std::uint32_t nodeCount = in.u32();
    const std::uint32_t wayCount = in.u32();
    const std::uint32_t tagCount = in.u32();
    const std::uint32_t wayNodeCount = in.u32();
    const std::uint32_t linkCount = in.u32();
    const std::uint32_t keyCount = in.u32();
    const std::uint32_t valueCount = in.u32();
    const std::uint32_t keyBytes = in.u32();
    const std::uint32_t valueBytes = in.u32();

    struct NodeRecord {
        std::uint32_t linkCount;
        std::uint32_t firstLink;
        std::uint32_t tagCount;
        std::uint32_t firstTag;
    };
    in.need(static_cast<std::size_t>(nodeCount) * 32);
    std::vector<glm::vec3> nodes(nodeCount);
    std::vector<NodeRecord> nodeRecords(nodeCount);
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        in.skip(4);
        nodes[i].x = in.f32();
        nodes[i].y = in.f32();
        nodes[i].z = in.f32();
        nodeRecords[i] = {in.u32(), in.u32(), in.u32(), in.u32()};
    }
    struct WayRecord {
        std::uint32_t nodeCount;
        std::uint32_t firstNode;
        std::uint32_t tagCount;
        std::uint32_t firstTag;
    };
    in.need(static_cast<std::size_t>(wayCount) * 16);
    std::vector<WayRecord> ways(wayCount);
    for (WayRecord& way : ways) {
        way = {in.u32(), in.u32(), in.u32(), in.u32()};
    }
    in.need(static_cast<std::size_t>(wayNodeCount) * 4);
    std::vector<std::uint32_t> wayNodes(wayNodeCount);
    for (std::uint32_t& node : wayNodes) {
        node = in.u32();
    }
    in.need(static_cast<std::size_t>(linkCount) * 8);
    std::vector<NavLink> links(linkCount);
    for (NavLink& link : links) {
        link = {in.u32(), in.u32()};
    }
    in.need(static_cast<std::size_t>(tagCount) * 8);
    std::vector<std::pair<std::uint32_t, std::uint32_t>> tags(tagCount);
    for (auto& [key, value] : tags) {
        key = in.u32();
        value = in.u32();
    }
    const auto readOffsets = [&](std::uint32_t count) {
        in.need(static_cast<std::size_t>(count) * 4);
        std::vector<std::uint32_t> offsets(count);
        for (std::uint32_t& offset : offsets) {
            offset = in.u32();
        }
        return offsets;
    };
    const std::vector<std::uint32_t> keyOffsets = readOffsets(keyCount);
    const std::vector<std::uint32_t> valueOffsets = readOffsets(valueCount);
    const ByteView keyText = in.bytes(keyBytes);
    const ByteView valueText = in.bytes(valueBytes);
    if (in.u32() != kNavMagic || !in.atEnd()) {
        in.fail("the road network does not end with its magic");
    }
    const auto text = [&](ByteView blob, std::uint32_t offset) {
        if (offset >= blob.size()) {
            in.fail(std::format("text offset {} past {} bytes", offset, blob.size()));
        }
        const auto* start = reinterpret_cast<const char*>(blob.data()) + offset;
        const std::size_t length = ::strnlen(start, blob.size() - offset);
        return std::string(start, length);
    };
    std::vector<std::string> keys;
    keys.reserve(keyOffsets.size());
    for (const std::uint32_t offset : keyOffsets) {
        keys.push_back(text(keyText, offset));
    }
    std::vector<std::string> values;
    values.reserve(valueOffsets.size());
    for (const std::uint32_t offset : valueOffsets) {
        values.push_back(text(valueText, offset));
    }

    NavFile file;
    const auto readTags = [&](std::uint32_t first, std::uint32_t count) {
        if (static_cast<std::uint64_t>(first) + count > tags.size()) {
            in.fail("a tag list reaches past its table");
        }
        NavTags list;
        list.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto [key, value] = tags[first + i];
            if (key >= keys.size() || value >= values.size()) {
                in.fail("a tag names a missing key or value");
            }
            list.emplace_back(keys[key], values[value]);
        }
        return list;
    };
    file.nodes.reserve(nodeCount);
    for (std::uint32_t i = 0; i < nodeCount; ++i) {
        const NodeRecord& record = nodeRecords[i];
        if (static_cast<std::uint64_t>(record.firstLink) + record.linkCount > links.size()) {
            in.fail("a node's links reach past their table");
        }
        NavNode node;
        node.position = nodes[i];
        node.tags = readTags(record.firstTag, record.tagCount);
        node.links.assign(links.begin() + record.firstLink, links.begin() + record.firstLink + record.linkCount);
        for (const NavLink& link : node.links) {
            if (link.way >= ways.size() || link.position >= ways[link.way].nodeCount
                || static_cast<std::uint64_t>(ways[link.way].firstNode) + link.position >= wayNodes.size()
                || wayNodes[ways[link.way].firstNode + link.position] != i) {
                in.fail(std::format("a link of node {} does not lead back to it", i));
            }
        }
        file.nodes.push_back(std::move(node));
    }
    file.ways.reserve(wayCount);
    for (const WayRecord& record : ways) {
        if (static_cast<std::uint64_t>(record.firstNode) + record.nodeCount > wayNodes.size()
            || static_cast<std::uint64_t>(record.firstTag) + record.tagCount > tags.size()) {
            in.fail("a way reaches past its tables");
        }
        NavWay way;
        way.points.reserve(record.nodeCount);
        way.nodes.reserve(record.nodeCount);
        for (std::uint32_t i = 0; i < record.nodeCount; ++i) {
            const std::uint32_t node = wayNodes[record.firstNode + i];
            if (node >= nodes.size()) {
                in.fail(std::format("way node {} of {}", node, nodes.size()));
            }
            way.points.emplace_back(nodes[node].x, nodes[node].z);
            way.nodes.push_back(node);
        }
        way.tags = readTags(record.firstTag, record.tagCount);
        file.ways.push_back(std::move(way));
    }
    file.values = std::move(values);
    return file;
}

MapProfile MapProfile::parse(std::string_view xml, const std::string& source, std::string_view name)
{
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(xml.data(), xml.size());
    if (!result) {
        throw DataError("{}: {}", source, result.description());
    }
    for (const pugi::xml_node profileNode : document.child("MapRenderProfiles").children("Profile")) {
        if (std::string_view(profileNode.attribute("name").as_string()) != name) {
            continue;
        }
        MapProfile profile;
        for (const pugi::xml_node groupNode : profileNode.children("Group")) {
            MapGroup group;
            group.name = groupNode.attribute("name").as_string();
            const pugi::xml_node filter = groupNode.child("Filter");
            group.filterTag = filter.attribute("tag").as_string();
            group.filterValue = filter.attribute("value").as_string();
            for (const pugi::xml_node node : groupNode.children("Symbolizer")) {
                MapSymbolizer symbolizer;
                symbolizer.type = node.attribute("type").as_string();
                symbolizer.sortOrder = node.attribute("sort_order").as_int();
                if (const pugi::xml_node size = node.child("Size")) {
                    symbolizer.size = attributeFloat(size, "value", 0.0F);
                    symbolizer.hasSize = true;
                } else if (const pugi::xml_node dynamic = node.child("DynamicSize")) {
                    symbolizer.size = attributeFloat(dynamic, "value", 0.0F);
                    symbolizer.dynamicSize = true;
                    symbolizer.hasSize = true;
                }
                symbolizer.primary = colour(node.child("ColourPrimary"));
                symbolizer.secondary = colour(node.child("ColourSecondary"));
                if (const pugi::xml_node atlas = node.child("Atlas")) {
                    symbolizer.atlasX = atlas.attribute("x").as_int();
                    symbolizer.atlasY = atlas.attribute("y").as_int();
                    symbolizer.atlasColumns = std::max(1, atlas.attribute("x_slots").as_int(1));
                    symbolizer.atlasRows = std::max(1, atlas.attribute("y_slots").as_int(1));
                }
                symbolizer.texture = node.child("Texture").attribute("value").as_string();
                if (const pugi::xml_node ratio = node.child("PlacementRatio")) {
                    symbolizer.placementRatio = attributeFloat(ratio, "value", 0.0F);
                }
                group.symbolizers.push_back(std::move(symbolizer));
            }
            profile.groups.push_back(std::move(group));
        }
        return profile;
    }
    throw DataError("{}: no profile \"{}\"", source, name);
}

SatNavTuning SatNavTuning::load(const Vfs& vfs)
{
    const Bytes data = vfs.readOrThrow(kRegistryPath);
    pugi::xml_document document;
    const pugi::xml_parse_result result = document.load_buffer(data.data(), data.size());
    if (!result) {
        throw DataError("{}: {}", kRegistryPath, result.description());
    }
    std::unordered_map<std::string, float> values;
    for (const pugi::xml_node value : document.child("Registry").children("Value")) {
        values.emplace(value.attribute("id").as_string(), value.attribute("value").as_float());
    }
    const auto get = [&](const char* id) {
        const auto it = values.find(std::string("UI/Sat_Nav/") + id);
        if (it == values.end()) {
            throw DataError("{}: no UI/Sat_Nav/{}", kRegistryPath, id);
        }
        return it->second;
    };
    SatNavTuning tuning;
    tuning.carSpeedMin = get("Car_Speed_Min");
    tuning.carSpeedMax = get("Car_Speed_Max");
    tuning.distanceMin = get("Map_Distance_Min");
    tuning.distanceMax = get("Map_Distance_Max");
    tuning.elevationMinDegrees = get("Elevation_Min");
    tuning.elevationMaxDegrees = get("Elevation_Max");
    tuning.fovMinDegrees = get("FOV_Min");
    tuning.fovMaxDegrees = get("FOV_Max");
    tuning.playerViewportY = get("Player_Position_In_Viewport_Y");
    return tuning;
}

MinimapCamera MinimapCamera::follow(
    const SatNavTuning& tuning, glm::vec2 position, float heading, float speed, float aspect)
{
    const float span = tuning.carSpeedMax - tuning.carSpeedMin;
    const float t = span > 0.0F ? std::clamp((speed - tuning.carSpeedMin) / span, 0.0F, 1.0F) : 0.0F;
    const auto lerp = [t](float a, float b) { return a + (b - a) * t; };
    constexpr float kRadians = std::numbers::pi_v<float> / 180.0F;
    const float distance = lerp(tuning.distanceMin, tuning.distanceMax);
    const float elevation = lerp(tuning.elevationMinDegrees, tuning.elevationMaxDegrees) * kRadians;
    const float fov = lerp(tuning.fovMinDegrees, tuning.fovMaxDegrees) * kRadians;

    MinimapCamera camera;
    camera.distance = distance;
    // 0x82da2388 turns straight down, (0, -1, 0) (the constant at
    // 0x82041090), about the horizontal axis across the heading by the
    // elevation: the elevation is measured from the vertical, so 70 degrees
    // looks 20 degrees below the horizon. That the turn goes forwards, not
    // backwards, is inferred (the car is placed below the centre of the
    // view, which only works looking ahead).
    const glm::vec3 flat(std::sin(heading), 0.0F, std::cos(heading));
    camera.forward = flat * std::sin(elevation) - glm::vec3(0.0F, 1.0F, 0.0F) * std::cos(elevation);
    camera.right = glm::normalize(glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), camera.forward));
    camera.up = glm::cross(camera.forward, camera.right);
    camera.scaleY = 1.0F / std::tan(fov * 0.5F);
    camera.scaleX = camera.scaleY / aspect;

    const glm::vec3 car(position.x, 0.0F, position.y);
    camera.eye = car - camera.forward * distance;
    // Where the ray through the viewport's tuned height meets the ground,
    // then the target mirrored about the car (0x82da0fe8).
    const glm::vec3 ray = camera.forward + camera.up * (tuning.playerViewportY / camera.scaleY);
    if (ray.y < 0.0F) {
        const float along = -camera.eye.y / ray.y;
        const glm::vec3 hit = camera.eye + ray * along;
        const glm::vec3 target = car * 2.0F - hit;
        camera.eye = glm::vec3(target.x, 0.0F, target.z) - camera.forward * distance;
    }
    return camera;
}

MinimapRenderer::MinimapRenderer(const Vfs& vfs, UiRenderer& renderer, const std::string& track)
    : m_renderer(renderer)
{
    const std::string navPath = std::format("media/tracks/{}/{}.nav", track, track);
    m_nav = NavFile::parse(vfs.readOrThrow(navPath), navPath);
    const Bytes profile = vfs.readOrThrow(kProfilePath);
    m_profile = MapProfile::parse(
        std::string_view(reinterpret_cast<const char*>(profile.data()), profile.size()), kProfilePath, kProfileName);
    m_tuning = SatNavTuning::load(vfs);
    m_activityMarkers = loadActivityMarkers(vfs, track);
    m_program = UiProgram("minimap", kVertexShader, kFragmentShader);
    buildRoads();
    m_viewProjection = m_program.location("uViewProjection");
    m_technique = m_program.location("uTechnique");

    glCreateVertexArrays(1, &m_vao);
    glEnableVertexArrayAttrib(m_vao, 0);
    glEnableVertexArrayAttrib(m_vao, 1);
    glEnableVertexArrayAttrib(m_vao, 2);
    glVertexArrayAttribFormat(m_vao, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vertex, position));
    glVertexArrayAttribFormat(m_vao, 1, 2, GL_FLOAT, GL_FALSE, offsetof(Vertex, uv));
    glVertexArrayAttribFormat(m_vao, 2, 4, GL_FLOAT, GL_FALSE, offsetof(Vertex, color));
    glVertexArrayAttribBinding(m_vao, 0, 0);
    glVertexArrayAttribBinding(m_vao, 1, 0);
    glVertexArrayAttribBinding(m_vao, 2, 0);
}

MinimapRenderer::~MinimapRenderer()
{
    glDeleteBuffers(1, &m_vbo);
    glDeleteVertexArrays(1, &m_vao);
    glDeleteFramebuffers(1, &m_framebuffer);
    glDeleteTextures(1, &m_color);
}

void MinimapRenderer::ensureTarget(int size)
{
    if (size == m_size && m_framebuffer) {
        return;
    }
    glDeleteFramebuffers(1, &m_framebuffer);
    glDeleteTextures(1, &m_color);
    glCreateTextures(GL_TEXTURE_2D, 1, &m_color);
    glTextureStorage2D(m_color, 1, GL_RGBA8, size, size);
    glTextureParameteri(m_color, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_color, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glCreateFramebuffers(1, &m_framebuffer);
    glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_color, 0);
    if (glCheckNamedFramebufferStatus(m_framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        throw std::runtime_error("minimap render target is incomplete");
    }
    m_size = size;
}

void MinimapRenderer::appendLine(
    const std::vector<glm::vec2>& points, float halfWidth, const std::vector<glm::vec4>& colors, bool caps)
{
    if (points.size() < 2 || colors.size() != points.size()) {
        return;
    }
    // 0x82da8200: a quad pair per point, offset along the averaged normal of
    // the segments meeting there, and a cap quad past each end.
    const auto normalOf = [](glm::vec2 from, glm::vec2 to) {
        const glm::vec2 d = to - from;
        const float length = glm::length(d);
        return length > 0.0F ? glm::vec2(d.y, -d.x) / length : glm::vec2(0.0F);
    };
    const auto ground = [](glm::vec2 p) { return glm::vec3(p.x, 0.0F, p.y); };
    std::vector<Vertex> strip;
    // Each point takes its own colour; the caps take their end's.
    const auto emitPair = [&](glm::vec2 centre, glm::vec2 normal, float u, const glm::vec4& color) {
        strip.push_back({ground(centre - normal * halfWidth), glm::vec2(u, -1.0F), color});
        strip.push_back({ground(centre + normal * halfWidth), glm::vec2(u, 1.0F), color});
    };
    const glm::vec2 firstDirection = glm::normalize(points[1] - points[0]);
    const glm::vec2 firstNormal = normalOf(points[0], points[1]);
    if (caps) {
        emitPair(points[0] - firstDirection * halfWidth, firstNormal, -1.0F, colors.front());
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
        emitPair(points[i], normal, 0.0F, colors[i]);
    }
    if (caps) {
        const std::size_t last = points.size() - 1;
        const glm::vec2 lastDirection = glm::normalize(points[last] - points[last - 1]);
        emitPair(
            points[last] + lastDirection * halfWidth, normalOf(points[last - 1], points[last]), -1.0F, colors.back());
    }
    for (std::size_t i = 0; i + 3 < strip.size(); i += 2) {
        m_vertices.insert(
            m_vertices.end(), {strip[i], strip[i + 1], strip[i + 2], strip[i + 1], strip[i + 3], strip[i + 2]});
    }
}

void MinimapRenderer::appendIcon(const glm::vec3& centre, const glm::vec3& across, const glm::vec3& up,
    const MapSymbolizer& symbolizer, const glm::vec4& color)
{
    const float u0 = static_cast<float>(symbolizer.atlasX) / static_cast<float>(symbolizer.atlasColumns);
    const float u1 = static_cast<float>(symbolizer.atlasX + 1) / static_cast<float>(symbolizer.atlasColumns);
    const float v0 = static_cast<float>(symbolizer.atlasY) / static_cast<float>(symbolizer.atlasRows);
    const float v1 = static_cast<float>(symbolizer.atlasY + 1) / static_cast<float>(symbolizer.atlasRows);
    const Vertex topLeft{centre + up - across, glm::vec2(u0, v0), color};
    const Vertex topRight{centre + up + across, glm::vec2(u1, v0), color};
    const Vertex bottomLeft{centre - up - across, glm::vec2(u0, v1), color};
    const Vertex bottomRight{centre - up + across, glm::vec2(u1, v1), color};
    m_vertices.insert(m_vertices.end(), {topLeft, topRight, bottomLeft, topRight, bottomRight, bottomLeft});
}

std::vector<glm::vec4> MinimapRenderer::roadColours(const MapSymbolizer& symbolizer, std::size_t way) const
{
    const std::size_t count = m_nav.ways[way].points.size();
    std::vector<float> ratios;
    if (m_coverage && m_coverage->wayCount() == m_nav.ways.size()) {
        ratios = m_coverage->wayRatios(way);
    }
    ratios.resize(count, 0.0F);
    std::vector<glm::vec4> colours;
    colours.reserve(count);
    for (const float ratio : ratios) {
        colours.push_back(lineColour(symbolizer, ratio));
    }
    return colours;
}

void MinimapRenderer::setCoverage(const MapCoverage* coverage)
{
    if (coverage != m_coverage) {
        m_coverage = coverage;
        buildRoads();
    }
}

void MinimapRenderer::buildRoads()
{
    // The road network does not move and its line widths do not change with
    // the camera (every line of the profile has a fixed <Size>), so its
    // geometry is built once, and again whenever the driven roads change
    // their colours.
    m_vertices.clear();
    m_roadBatches.clear();
    m_uploadedRoads = false;
    m_coverageRevision = m_coverage ? std::optional<std::uint64_t>(m_coverage->revision()) : std::nullopt;
    for (const MapGroup& group : m_profile.groups) {
        if (group.filterTag.empty() || group.filterTag == kPlayerFilterTag) {
            continue;
        }
        std::vector<std::size_t> ways;
        for (std::size_t way = 0; way < m_nav.ways.size(); ++way) {
            if (m_nav.ways[way].tag(group.filterTag) == group.filterValue) {
                ways.push_back(way);
            }
        }
        for (const MapSymbolizer& symbolizer : group.symbolizers) {
            const int technique = lineTechnique(symbolizer.type);
            if (technique < 0 || ways.empty() || symbolizer.dynamicSize) {
                continue;
            }
            Batch batch;
            batch.sortOrder = symbolizer.sortOrder;
            batch.technique = technique;
            batch.first = m_vertices.size();
            // Quad half-width: 2 * size * (k + (1 - k) * distance / ref),
            // which a fixed size (k = 1) reduces to twice the size.
            const float halfWidth = 2.0F * symbolizer.size;
            const bool caps = symbolizer.type != "line_nocaps";
            for (const std::size_t way : ways) {
                appendLine(m_nav.ways[way].points, halfWidth, roadColours(symbolizer, way), caps);
            }
            batch.count = m_vertices.size() - batch.first;
            if (batch.count > 0) {
                m_roadBatches.push_back(batch);
            }
        }
    }
    m_roadVertexCount = m_vertices.size();
}

GLuint MinimapRenderer::render(glm::vec2 position, float heading, float speedMetresPerSecond, int size,
    const std::vector<MapMarker>& markers, const std::vector<MapLine>& lines)
{
    size = std::max(8, size);
    ensureTarget(size);
    const MinimapCamera camera = MinimapCamera::follow(m_tuning, position, heading, speedMetresPerSecond, 1.0F);

    if ((m_coverage ? std::optional<std::uint64_t>(m_coverage->revision()) : std::nullopt) != m_coverageRevision) {
        buildRoads();
    }
    m_vertices.resize(m_roadVertexCount);
    m_batches = m_roadBatches;

    std::vector<MapMarker> all = m_activityMarkers;
    all.insert(all.end(), markers.begin(), markers.end());
    all.push_back({kPlayerFilterTag, kPlayerFilterValue, position, heading});
    for (const MapGroup& group : m_profile.groups) {
        std::vector<const MapLine*> matchedLines;
        for (const MapLine& line : lines) {
            if (!group.filterTag.empty() && line.key == group.filterTag && line.value == group.filterValue
                && line.points.size() >= 2) {
                matchedLines.push_back(&line);
            }
        }
        for (const MapSymbolizer& symbolizer : group.symbolizers) {
            const int technique = lineTechnique(symbolizer.type);
            if (technique < 0 || matchedLines.empty()) {
                continue;
            }
            Batch batch;
            batch.sortOrder = symbolizer.sortOrder;
            batch.technique = technique;
            batch.first = m_vertices.size();
            for (const MapLine* line : matchedLines) {
                appendLine(line->points, 2.0F * symbolizer.size,
                    std::vector<glm::vec4>(line->points.size(), colorOf(symbolizer)), symbolizer.type != "line_nocaps");
            }
            batch.count = m_vertices.size() - batch.first;
            if (batch.count > 0) {
                m_batches.push_back(batch);
            }
        }
        std::vector<const MapMarker*> matched;
        for (const MapMarker& marker : all) {
            if (!group.filterTag.empty() && marker.key == group.filterTag && marker.value == group.filterValue) {
                matched.push_back(&marker);
            }
        }
        if (matched.empty() && matchedLines.empty()) {
            continue;
        }
        for (const MapSymbolizer& symbolizer : group.symbolizers) {
            const IconOrientation orientation = iconOrientation(symbolizer.type);
            if (orientation == IconOrientation::None || symbolizer.texture.empty()) {
                continue;
            }
            std::string path = lower("media/ui/textures/" + symbolizer.texture);
            std::replace(path.begin(), path.end(), '\\', '/');
            Batch batch;
            batch.sortOrder = symbolizer.sortOrder;
            batch.technique = 2;
            batch.texture = m_renderer.texture(texturePathForScene(path));
            batch.first = m_vertices.size();
            if (!batch.texture) {
                continue;
            }
            const float scale = symbolizer.dynamicSize ? kDynamicSizeBase : 1.0F;
            const float reference = symbolizer.dynamicSize ? kDynamicSizeReference : 1.0F;
            // Icons on line entities sit at their PlacementRatio along the
            // line, turned with the segment there.
            std::vector<MapMarker> onLines;
            if (symbolizer.placementRatio) {
                for (const MapLine* line : matchedLines) {
                    onLines.push_back(pointAlong(*line, *symbolizer.placementRatio));
                }
            }
            std::vector<const MapMarker*> targets = matched;
            for (const MapMarker& marker : onLines) {
                targets.push_back(&marker);
            }
            for (const MapMarker* marker : targets) {
                const glm::vec3 centre(marker->position.x, 0.0F, marker->position.y);
                const float depth = camera.depth(centre);
                const float halfSize = symbolizer.size * (scale + (1.0F - scale) * depth / reference);
                if (orientation == IconOrientation::Entity) {
                    // "icon" (mode 0, 0x82da76a0) lies on the ground turned
                    // with its entity. Which way the atlas cell faces is not
                    // traced: its top is put forward.
                    const glm::vec3 forward(std::sin(marker->heading), 0.0F, std::cos(marker->heading));
                    const glm::vec3 side(forward.z, 0.0F, -forward.x);
                    appendIcon(centre, side * halfSize, forward * halfSize, symbolizer, colorOf(symbolizer));
                } else {
                    // Facing (mode 1): upright on screen, along the camera's
                    // right and up vectors.
                    appendIcon(centre, camera.right * halfSize, camera.up * halfSize, symbolizer, colorOf(symbolizer));
                }
            }
            batch.count = m_vertices.size() - batch.first;
            if (batch.count > 0) {
                m_batches.push_back(batch);
            }
        }
    }
    // Lower sort orders first (inferred from the profile: shadows at 90,
    // roads at 100 to 130, the player's arrow at 600 and 610).
    std::stable_sort(
        m_batches.begin(), m_batches.end(), [](const Batch& a, const Batch& b) { return a.sortOrder < b.sortOrder; });

    const std::size_t bytes = m_vertices.size() * sizeof(Vertex);
    if (bytes > m_vboCapacity) {
        glDeleteBuffers(1, &m_vbo);
        glCreateBuffers(1, &m_vbo);
        m_vboCapacity = bytes + 4096 * sizeof(Vertex);
        glNamedBufferData(m_vbo, static_cast<GLsizeiptr>(m_vboCapacity), nullptr, GL_DYNAMIC_DRAW);
        glVertexArrayVertexBuffer(m_vao, 0, m_vbo, 0, sizeof(Vertex));
        m_uploadedRoads = false;
    }
    // The roads go up once; only what follows them changes per frame.
    const std::size_t roadBytes = m_roadVertexCount * sizeof(Vertex);
    if (!m_uploadedRoads && roadBytes > 0) {
        glNamedBufferSubData(m_vbo, 0, static_cast<GLsizeiptr>(roadBytes), m_vertices.data());
        m_uploadedRoads = true;
    }
    if (bytes > roadBytes) {
        glNamedBufferSubData(m_vbo, static_cast<GLintptr>(roadBytes), static_cast<GLsizeiptr>(bytes - roadBytes),
            m_vertices.data() + m_roadVertexCount);
    }

    // Clip space: across along the camera's right, up along its up (negated
    // so the texture's first row is the top, as UI textures are laid out),
    // w the depth, z spanning the clip planes.
    glm::mat4 viewProjection(0.0F);
    const glm::vec3 x = camera.right * camera.scaleX;
    const glm::vec3 y = -camera.up * camera.scaleY;
    const float zScale = (kFar + kNear) / (kFar - kNear);
    const float zOffset = -2.0F * kFar * kNear / (kFar - kNear);
    for (int column = 0; column < 3; ++column) {
        viewProjection[column][0] = x[column];
        viewProjection[column][1] = y[column];
        viewProjection[column][2] = camera.forward[column] * zScale;
        viewProjection[column][3] = camera.forward[column];
    }
    viewProjection[3][0] = -glm::dot(x, camera.eye);
    viewProjection[3][1] = -glm::dot(y, camera.eye);
    viewProjection[3][2] = -glm::dot(camera.forward, camera.eye) * zScale + zOffset;
    viewProjection[3][3] = -glm::dot(camera.forward, camera.eye);

    GLint previousFramebuffer = 0;
    GLint previousViewport[4] = {};
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousFramebuffer);
    glGetIntegerv(GL_VIEWPORT, previousViewport);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    glViewport(0, 0, size, size);
    // No background symbolizer is in the profile and no clear colour was
    // found: the target starts transparent.
    const float clear[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    glClearNamedFramebufferfv(m_framebuffer, GL_COLOR, 0, clear);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glUseProgram(m_program.id());
    glProgramUniformMatrix4fv(m_program.id(), m_viewProjection, 1, GL_FALSE, &viewProjection[0][0]);
    glBindVertexArray(m_vao);
    for (const Batch& batch : m_batches) {
        glProgramUniform1i(m_program.id(), m_technique, batch.technique);
        if (batch.texture) {
            glBindTextureUnit(0, batch.texture->id());
            glBindSampler(0, 0);
        }
        glDrawArrays(GL_TRIANGLES, static_cast<GLint>(batch.first), static_cast<GLsizei>(batch.count));
    }
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previousFramebuffer));
    glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
    return m_color;
}

} // namespace fh::ui
