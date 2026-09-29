#pragma once

#include "MenuCommon.h"
#include "Minimap.h"

#include <glm/glm.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::ui {

/// The full-screen map's tunables (UI/Map/* in
/// media/gametunablesettings/globalregistry.xml; registered at 0x83221ec0
/// and on, read by the map camera at 0x82584a40, 0x82584bd8 and 0x82587418).
struct WorldMapTuning {
    float distanceMin = 0.0F;
    float distanceMax = 0.0F;
    float cursorSpeedMin = 0.0F;
    float cursorSpeedMax = 0.0F;
    float cursorZoomMin = 0.0F;
    float cursorZoomMax = 0.0F;
    float zoomSpeed = 0.0F;
    float zoomDefault = 0.0F;
    float inputAccelerationStart = 0.0F;
    float inputAccelerationTime = 0.0F;
    float inputLinearity = 0.0F;
    float positionSpring = 0.0F;
    float positionDamping = 0.0F;
    float zoomSpring = 0.0F;
    float zoomDamping = 0.0F;
    float deadArea = 0.0F;
    float viewEdgeLeft = 0.0F;
    float viewEdgeRight = 0.0F;
    float viewEdgeTop = 0.0F;
    float viewEdgeBottom = 0.0F;
    float contextSnapSize = 0.0F;

    /// Throws DataError when a value is missing.
    static WorldMapTuning load(const Vfs& vfs);
};

/// The map camera: a cursor the left stick drives over the ground, a zoom
/// the right stick drives, and a camera that follows the cursor.
///
/// Transcribed from the executable: the stick shaping (0x82583ef0: the
/// magnitude raised to Input_Linearity, capped by an acceleration that
/// starts at Input_Acceleration_Start and grows to 1 over
/// Input_Acceleration_Time while the stick keeps its sign), the cursor
/// (0x82584bd8: speed from Cursor_Speed_Min to Cursor_Speed_Max by the zoom
/// value, the cursor kept inside the road network's cursor_area way
/// (0x825840e0), the zoom value moved by Zoom_Speed and kept between
/// Cursor_Zoom_Min and Cursor_Zoom_Max), the camera (0x82587418: its target
/// follows the cursor once the cursor is more than Dead_Area times the
/// camera distance away; distance from Map_Distance_Min to
/// Map_Distance_Max by the zoom; position and zoom smoothed by springs
/// stepped at 300 Hz (0x82583e88); the target kept so the view stays within
/// the ViewEdge tunables, with a width of 16/9 times the height
/// (0x82584b38)) and the view it sets up (elevation 0, a 90 degree field of
/// view, looking along +z: straight down, north up). What is left out: the
/// camera shake the map's animations can trigger (+336 to +344) and the
/// scripted camera moves of the unlock animations.
class WorldMapCamera {
public:
    /// The cursor's limits: the bounding box of the network's cursor_area
    /// way (x, z of its corners).
    WorldMapCamera(const WorldMapTuning& tuning, glm::vec2 cursorMin, glm::vec2 cursorMax);

    /// Puts the cursor and the camera on `position` at the default zoom, as
    /// opening the map does (0x825878b0).
    void reset(glm::vec2 position);
    /// `move` is the left stick, `zoom` the right stick's y (positive
    /// zooms in); both -1 to 1.
    void update(glm::vec2 move, float zoom, float dt);

    glm::vec2 cursor() const { return m_cursor; }
    /// The ground point below the camera.
    glm::vec2 position() const { return m_position; }
    /// The camera's height above the ground.
    float distance() const { return m_distance; }
    /// The zoom value the sticks drive (Cursor_Zoom_Min to Cursor_Zoom_Max).
    float zoom() const { return m_zoom; }

private:
    /// One stick axis's shaping state (0x82583ef0).
    struct Shaper {
        float held = 0.0F;
        float previous = 0.0F;
        float shape(float input, float dt, const WorldMapTuning& tuning);
    };
    /// A spring at 300 steps a second (0x82583e88).
    struct Spring {
        float carry = 0.0F;
        float position = 0.0F;
        float velocity = 0.0F;
        float step(float target, float dt, float stiffness, float damping);
    };

    float distanceFor(float zoom) const;
    glm::vec2 keepInView(glm::vec2 target, float distance) const;

    WorldMapTuning m_tuning;
    glm::vec2 m_cursorMin;
    glm::vec2 m_cursorMax;
    Shaper m_moveX;
    Shaper m_moveY;
    Shaper m_zoomInput;
    Spring m_x;
    Spring m_z;
    Spring m_zoomSpring;
    glm::vec2 m_cursor{0.0F};
    glm::vec2 m_target{0.0F};
    glm::vec2 m_position{0.0F};
    float m_zoom = 0.0F;
    float m_distance = 0.0F;
};

/// Something on the map with the name its context popup shows (empty for
/// none).
struct WorldMapMarker {
    MapMarker marker;
    std::string name;
};

/// What the map shows besides the roads and the track's fixed activity
/// places: the player's car and the markers the game adds (race starts
/// with activity_type from careerEventActivityType(), other cars with
/// vehicle_type "ai").
struct WorldMapState {
    glm::vec2 playerPosition{0.0F};
    /// Radians, 0 facing +z, increasing towards +x.
    float playerHeading = 0.0F;
    std::vector<WorldMapMarker> markers;
    /// The route to the waypoint, if the game has one, as ground points
    /// from the car to the waypoint. The map draws it with the profile's
    /// route group; working the route out along the roads is the game's
    /// job (the executable's route search was not traced).
    std::vector<glm::vec2> route;
};

enum class WorldMapAction {
    None,
    /// B (EXIT MAP): the map screen's onBack. Opened from the pause menu,
    /// the game goes back to it; opened with the Back button, to driving.
    Back,
    /// A on a marker (SET ROUTE): waypoint() is now that marker's place.
    SetWaypoint,
    /// A on the marker that is the waypoint (CLEAR ROUTE).
    ClearWaypoint,
};

/// The game's full-screen map: the scene media/ui/scenes/ui4/h_map
/// (screen 160) with its CUSTREND_MAPRENDERER drawing the "fullscreen"
/// profile of media/ui/MapProfileFullscreen.xml into the scene's Map
/// rectangle.
///
/// Drawn from the profile: the background (low.tga always, the 5 x 3
/// high.tga tiles within their VisibleRange, both placed by the tiles
/// symbolizer's Size and Offset: Size is the north-south extent and the
/// image's 5:3 shape gives the east-west one, which puts its edges on the
/// ViewEdgeTop and ViewEdgeBottom tunables), the road network's lines, the
/// route group along WorldMapState::route with its flag at the waypoint,
/// and the icon symbolizers of every marker, with the profile's "default"
/// and "intro" animations (the player's radar ping, the icons' grow).
/// Left out, as their state does not exist here: the career icons' ring
/// colour, finished wreath, NEW badge and warning note; the smoke of
/// newly revealed events and the unlock sequences; hover animations; the
/// off-screen arrows of clamped icons; icon snapping; online players and
/// barn find regions; the statistics panel, the detail pins and the player
/// bar.
///
/// Construct with a current OpenGL 4.5 context.
class WorldMap {
public:
    /// Throws fh::DataError when the scene, profile, network or tunables
    /// cannot be read.
    WorldMap(const Vfs& vfs, const std::string& track, const std::string& language = "en");
    ~WorldMap();
    WorldMap(const WorldMap&) = delete;
    WorldMap& operator=(const WorldMap&) = delete;

    /// Opens the map centred on the player at the default zoom.
    void open(const WorldMapState& state);
    void close();
    bool isOpen() const;
    /// Updates what the map shows while it is open.
    void setState(const WorldMapState& state);

    WorldMapAction update(const MenuInput& input, float dt);
    /// Draws over the bound framebuffer.
    void draw(int width, int height);

    std::optional<glm::vec2> waypoint() const;
    void clearWaypoint();
    const WorldMapCamera& camera() const;
    /// The marker the cursor is on, if any.
    const WorldMapMarker* hovered() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fh::ui
