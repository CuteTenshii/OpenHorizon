#pragma once

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/EngineSound.h"
#include "base/JobPool.h"
#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "carmodel/CarModel.h"
#include "data/Database.h"
#include "data/StringTable.h"
#include "game/AiDriver.h"
#include "game/CameraSettings.h"
#include "game/GameObjects.h"
#include "game/Race.h"
#include "game/RaceData.h"
#include "game/Smashables.h"
#include "game/TimeOfDay.h"
#include "game/TrafficConfig.h"
#include "physics/WorldCollision.h"
#include "platform/Window.h"
#include "render/CarRenderer.h"
#include "render/FrameRenderer.h"
#include "render/FrameStats.h"
#include "render/Overlay.h"
#include "render/WorldRenderer.h"
#include "vfs/Vfs.h"
#include "world/TrackWorld.h"

#include <glm/glm.hpp>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

namespace fh {

struct GameOptions {
    std::filesystem::path gameDir;
    std::string track = "colorado";
    /// Media name of the car to drive (Data_Car.MediaName).
    std::string car = "ALF_8C_08";
    /// A gameplay object to start at (GameObjs.xml GameplayID), unless
    /// `spawnAt` is given.
    std::string spawnObject;
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
    /// Starts this race (Events.HorizonEventID) straight away.
    std::string race;
    /// Lets the AI drive the player's car in a race, for testing.
    bool autopilot = false;
};

/// Free roam and races: the track's world streamed around the player's car,
/// with AI opponents when a race runs.
class Game {
public:
    enum class CameraMode { Chase, Far, Bumper, Hood, Free };

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
    /// One car in the world: the player's or an AI opponent's.
    struct Vehicle {
        std::unique_ptr<CarSpec> spec;
        std::unique_ptr<CarPhysics> physics;
        std::vector<BodySphere> spheres;
        std::unique_ptr<AiDriver> ai;
        /// The engine, exhaust and tyres as heard; missing without audio.
        std::unique_ptr<audio::EngineSound> engine;
        /// The car's own model; missing when it could not be loaded, and
        /// the car is then drawn as an outline.
        std::shared_ptr<render::CarRenderer> model;
        /// Who drives it, which decides its AI's targets and when it goes.
        enum class Role { Player, Racer, Traffic, Festival } role = Role::Racer;
        /// The line an AI follows, for putting it back when it is stuck.
        const RacingLine* line = nullptr;
        int lastGear = 1;
        CarControls controls;
        glm::vec4 colour{1.0F};
        /// Factory paint (Combo_Colors), linear RGB.
        glm::vec3 paint{0.8F};
        bool metallic = false;
        std::string name;
    };

    void loadWorld();
    /// Where free roam starts: the CPlaceCar of the free-roam flow's
    /// "place_car_post_credits" state, if the file has it.
    std::optional<std::pair<glm::vec3, float>> freeRoamStart() const;
    /// `model` is the car's decoded model when it was loaded elsewhere; it is
    /// read here otherwise, unless the cache already has it.
    std::unique_ptr<Vehicle> makeVehicle(int carId, const glm::vec4& colour, int colourSequence = 1,
        std::optional<carmodel::CarModel> model = std::nullopt) const;
    void spawnPlayer();
    void placeVehicle(Vehicle& vehicle, const glm::vec3& position, float heading);
    Vehicle& player() { return *m_vehicles.front(); }
    const Vehicle& player() const { return *m_vehicles.front(); }

    CarControls readPlayerControls(float dt);
    /// Returns the number of fixed steps taken.
    int stepPhysics(float dt);
    void collideWithWorld(Vehicle& vehicle);
    void collideVehicles();
    void collideSmashables(Vehicle& vehicle);
    void updateDebris(float dt);
    void attachEngineSound(Vehicle& vehicle, bool spatial) const;
    static audio::EngineSoundOptions engineSoundOptions(bool spatial);
    void updateAudio(float dt);

    void startRace(const RaceEvent& event);
    void endRace();
    void updateRace(float dt);
    void loadTraffic();
    void updateTraffic(float dt);
    /// Puts the car whose model and sound finished loading on the road.
    void finishTrafficSpawn();
    bool spawnTraffic(Vehicle::Role role, const TrafficConfig::Settings& settings,
        const TrafficConfig::Density& density);
    void updateAiDrivers(float dt);
    /// Distance to the nearest vehicle ahead in the same lane, or a large
    /// value when the road ahead is clear.
    float gapAhead(const Vehicle& vehicle) const;

    void updateEventStarts();
    void updateCamera(float dt);
    render::FrameParams frameParams() const;
    void drawVehicles(const render::FrameParams& frame);
    void drawVehicleGlass(const render::FrameParams& frame);
    render::CarPose carPose(const Vehicle& vehicle) const;
    void drawEventStarts(const glm::mat4& viewProjection);
    void drawDrivingLine();
    void drawHud(float dt);
    void drawRaceHud();
    void drawLoading(const std::string& message);
    bool saveScreenshot(const std::filesystem::path& path);

    Window& m_window;
    GameOptions m_options;
    Vfs m_vfs;
    std::unique_ptr<Database> m_database;
    std::unique_ptr<Localisation> m_strings;
    std::unique_ptr<JobPool> m_jobs;
    std::optional<world::TrackWorld> m_world;
    std::optional<GameObjects> m_objects;
    std::optional<TimeOfDay> m_timeOfDay;
    render::ToneSettings m_tone;
    std::unique_ptr<render::FrameRenderer> m_frame;
    std::unique_ptr<render::FrameStats> m_frameStats;
    std::unique_ptr<render::WorldRenderer> m_worldRenderer;
    std::unique_ptr<render::Overlay> m_overlay;
    std::unique_ptr<physics::WorldCollision> m_collision;

    // Declared before the vehicles so it outlives their engine sounds, whose
    // destructors release mixer buses.
    std::unique_ptr<audio::AudioDevice> m_audio;
    std::shared_ptr<audio::AudioLibrary> m_audioLibrary;
    std::vector<std::unique_ptr<Vehicle>> m_vehicles;
    std::optional<Smashables> m_smashables;
    /// Renderer handle of each loose smashable, by body index.
    std::unordered_map<std::size_t, int> m_debrisHandles;
    /// World chunks the renderer leaves out: those of loose smashables.
    std::vector<bool> m_hiddenChunks;
    bool m_hiddenChanged = false;
    float m_debrisAccumulator = 0.0F;
    std::vector<physics::SphereContact> m_contacts;
    bool m_playerPlaced = false;
    glm::vec3 m_spawnPosition{0.0F};
    float m_spawnHeading = 0.0F;
    float m_spawnWait = 0.0F;
    float m_physicsAccumulator = 0.0F;

    /// A traffic car chosen and placed on the main thread, whose model and
    /// engine sound load on a worker: decoding them took up to 0.4 s, which
    /// froze the game every time a car appeared.
    struct TrafficSpawn {
        Vehicle::Role role = Vehicle::Role::Traffic;
        int carId = 0;
        int colourSequence = 1;
        const RacingLine* line = nullptr;
        float lineIndex = 0.0F;
        float lane = 0.0F;
        float speed = 0.0F;
        int skill = 0;
        std::optional<carmodel::CarModel> model;
        std::unique_ptr<audio::EngineSound> engine;
        std::atomic<bool> ready{false};
    };
    // After the audio device, so a pending engine sound goes first.
    std::shared_ptr<TrafficSpawn> m_pendingSpawn;

    std::optional<TrafficConfig> m_trafficConfig;
    /// Every racing line in both directions: the roads traffic drives.
    std::vector<std::unique_ptr<RacingLine>> m_trafficLines;
    /// Cars of the festival drivers who roam the map (FreeRoamDrivers).
    std::vector<int> m_festivalCars;
    float m_trafficTimer = 0.0F;
    std::mt19937 m_random{20121023};
    /// Car models by car id, shared by every car of the same model.
    mutable std::unordered_map<int, std::shared_ptr<render::CarRenderer>> m_modelCache;
    std::vector<RaceEvent> m_events;
    /// Where each race can be started in free roam: the gameplay object
    /// named by its HorizonEventID, as an index into m_events.
    struct EventStart {
        std::size_t event = 0;
        glm::vec3 position{0.0F};
    };
    std::vector<EventStart> m_eventStarts;
    /// The start the player is standing in, if any.
    std::optional<std::size_t> m_nearbyStart;
    /// Names of the starts in view this frame, at their screen positions.
    struct ScreenLabel {
        glm::vec2 position{0.0F};
        std::string text;
        float alpha = 1.0F;
    };
    std::vector<ScreenLabel> m_labels;
    std::unique_ptr<Race> m_race;
    /// Speed profile of the race line for the player's car, which colours
    /// the driving line.
    std::unique_ptr<AiDriver> m_lineGuide;
    float m_resultsTime = 0.0F;

    CameraMode m_cameraMode = CameraMode::Chase;
    render::Camera m_camera;
    std::optional<CameraSettings> m_cameraSettings;
    CarCameras m_carCameras;
    /// The chase camera's heading around the car and its rate, and its
    /// vertical speed, for the springs camerasettings.ini tunes.
    float m_chaseYaw = 0.0F;
    float m_chaseYawSpeed = 0.0F;
    float m_chaseHeightSpeed = 0.0F;
    /// Looking around from the driving cameras: offsets from where the
    /// camera faces, eased back to zero when the stick is let go.
    float m_lookYaw = 0.0F;
    float m_lookPitch = 0.0F;
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
