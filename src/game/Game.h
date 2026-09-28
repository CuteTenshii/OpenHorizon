#pragma once

#include "base/JobPool.h"
#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "data/Database.h"
#include "game/GameObjects.h"
#include "game/TimeOfDay.h"
#include "physics/WorldCollision.h"
#include "platform/Window.h"
#include "render/FrameRenderer.h"
#include "render/Overlay.h"
#include "render/WorldRenderer.h"
#include "vfs/Vfs.h"
#include "world/TrackWorld.h"

#include <glm/glm.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fh {

struct GameOptions {
    std::filesystem::path gameDir;
    std::string track = "colorado";
    /// Media name of the car to drive (Data_Car.MediaName).
    std::string car = "ALF_8C_08";
    /// A gameplay object to start at (GameObjs.xml GameplayID), unless
    /// `spawnAt` is given.
    std::string spawnObject = "FESTIVAL_FIRST";
    std::optional<glm::vec2> spawnAt;
    std::optional<float> spawnHeading;
    /// Stop after this many frames and, with `screenshot`, save the last one.
    std::optional<int> frames;
    std::filesystem::path screenshot;
    bool manualGearbox = false;
    /// Holds full throttle with a slight steer, for checking the driving
    /// without a player.
    bool demoDrive = false;
    /// Start time in seconds after midnight; the game's own 16:00 otherwise.
    std::optional<float> startTime;
};

/// Free roam: the track's world streamed around a car the player drives.
class Game {
public:
    enum class CameraMode { Chase, Far, Hood, Free };

    Game(Window& window, const GameOptions& options);
    ~Game();
    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    /// Runs until the window closes or the frame limit is reached. Returns
    /// the process exit code.
    int run();

private:
    struct BodySphere {
        glm::vec3 centre;
        float radius;
    };

    void loadWorld();
    void spawnCar();
    CarControls readControls(float dt);
    void stepPhysics(float dt, const CarControls& controls);
    void collideBody();
    void updateCamera(float dt);
    render::FrameParams frameParams() const;
    void drawCar();
    void drawHud(float dt);
    void drawLoading(const std::string& message);
    bool saveScreenshot(const std::filesystem::path& path);

    Window& m_window;
    GameOptions m_options;
    Vfs m_vfs;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<JobPool> m_jobs;
    std::optional<world::TrackWorld> m_world;
    std::optional<GameObjects> m_objects;
    std::optional<TimeOfDay> m_timeOfDay;
    render::ToneSettings m_tone;
    std::unique_ptr<render::FrameRenderer> m_frame;
    std::unique_ptr<render::WorldRenderer> m_worldRenderer;
    std::unique_ptr<render::Overlay> m_overlay;
    std::unique_ptr<physics::WorldCollision> m_collision;

    CarSpec m_spec;
    std::unique_ptr<CarPhysics> m_car;
    std::vector<BodySphere> m_bodySpheres;
    std::vector<physics::SphereContact> m_contacts;
    bool m_carPlaced = false;
    glm::vec3 m_spawnPosition{0.0F};
    float m_spawnHeading = 0.0F;
    float m_physicsAccumulator = 0.0F;
    float m_spawnWait = 0.0F;

    CameraMode m_cameraMode = CameraMode::Chase;
    render::Camera m_camera;
    glm::vec3 m_chaseVelocity{0.0F};
    float m_freeYaw = 0.0F;
    float m_freePitch = -0.2F;
    float m_keyboardSteer = 0.0F;
    bool m_showDebug = false;
    float m_time = 0.0F;
    float m_fps = 0.0F;
    int m_frameCount = 0;
};

} // namespace fh
