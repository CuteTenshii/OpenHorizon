#pragma once

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/EngineSound.h"
#include "audio/MixSnapshots.h"
#include "base/JobPool.h"
#include "car/CarPhysics.h"
#include "car/CarSpec.h"
#include "carmodel/CarModel.h"
#include "data/Database.h"
#include "data/StringTable.h"
#include "game/Activities.h"
#include "game/ActivityProps.h"
#include "game/AiDriver.h"
#include "game/CameraSettings.h"
#include "game/DrivingLine.h"
#include "game/Flyers.h"
#include "game/GameObjects.h"
#include "game/Outposts.h"
#include "game/Race.h"
#include "game/RaceData.h"
#include "game/Smashables.h"
#include "game/SpeedTraps.h"
#include "game/TimeOfDay.h"
#include "game/TrafficConfig.h"
#include "particles/ParticleManager.h"
#include "particles/ParticleRenderer.h"
#include "particles/TyreEffects.h"
#include "physics/WorldCollision.h"
#include "platform/Window.h"
#include "radio/Radio.h"
#include "render/CarRenderer.h"
#include "render/DrivingLineRenderer.h"
#include "render/FrameRenderer.h"
#include "render/FrameStats.h"
#include "render/Overlay.h"
#include "render/PresentTarget.h"
#include "render/SkyObjects.h"
#include "render/WorldRenderer.h"
#include "ui/Hud.h"
#include "ui/LoadingScreen.h"
#include "ui/MenuCommon.h"
#include "ui/Navigation.h"
#include "ui/PauseMenu.h"
#include "ui/UiSounds.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"
#include "world/TrackWorld.h"

#include <glm/glm.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
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
    bool manualGearbox = false;
    /// Start time in seconds after midnight; the game's own 16:00 otherwise.
    std::optional<float> startTime;
    /// Starts this race (Events.HorizonEventID) straight away.
    std::string race;
    /// Lets the AI drive the player's car in a race; the scenario tests use
    /// it to run races through.
    bool autopilot = false;
    /// Kilometres rather than miles per hour on the speedometer.
    bool metric = true;
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

    /// Runs until the window closes or the player quits, on the window's
    /// input and clock. Returns the process exit code.
    int run();
    /// Runs one frame of `dt` seconds with `controls` as the player's input
    /// and draws it into the game's 16:9 picture, without presenting it to
    /// the window. Returns false when the player quits. The scenario tests
    /// drive the game through this with scripted input and a fixed step.
    bool frame(const InputState& controls, float dt);
    /// Saves the last drawn 16:9 picture as PNG.
    bool saveScreenshot(const std::filesystem::path& path);

    /// What the start of play still waits for; see loadingStatus().
    struct LoadingStatus {
        /// The tip loading screen is up.
        bool screenShown = false;
        /// The fade from black that follows it is running.
        bool fading = false;
        /// Jobs queued or running on the job pool: tile builds, texture
        /// decodes, collision and car loads.
        std::size_t pendingJobs = 0;
        /// The tiles around the camera have their geometry and textures at
        /// the level of detail it needs.
        bool nearbyWorldReady = false;
        /// Track effects not linked yet.
        std::size_t programsPending = 0;
        /// The player's car is on the ground with its model and engine
        /// sound (those that could be loaded at all).
        bool playerCarReady = false;
        /// The radio plays, or there is none or it is off.
        bool radioReady = false;
        /// Initial traffic cars not on the road yet.
        std::size_t trafficPending = 0;
    };

    /// Read-only state for the scenario tests.
    bool playerPlaced() const { return m_playerPlaced; }
    /// The player drives: loading and its fade are over.
    bool inControl() const;
    LoadingStatus loadingStatus() const;
    const CarPhysics& playerCar() const { return *m_vehicles.front()->physics; }
    const render::Camera& camera() const { return m_camera; }
    bool menuShown() const { return menuOpen(); }
    bool pauseShown() const { return m_pause && m_pause->isOpen(); }
    /// The pause menu, or nullptr when its scenes could not be loaded.
    const ui::PauseMenu* pauseMenu() const { return m_pause.get(); }
    bool mapShown() const { return m_map && m_map->isOpen(); }
    /// The full-screen map, or nullptr when its scene could not be loaded.
    const ui::WorldMap* worldMap() const { return m_map.get(); }
    /// The road network, the car's place on it and the roads driven; null
    /// when the track's network could not be read.
    const ui::Navigation* navigation() const { return m_navigation.get(); }
    /// Whether the last frame drew the world (the map screen does not).
    bool worldDrawn() const { return m_worldDrawn; }
    float timeOfDaySeconds() const { return m_timeOfDay ? m_timeOfDay->seconds() : 0.0F; }
    const Race* race() const { return m_race.get(); }
    /// The clock the world's animation runs on (render::FrameParams::time).
    float animationTime() const { return m_time; }
    /// Whether the last frame drew the HUD.
    bool hudDrawn() const { return m_hudDrawn; }
    /// Vertices the last frame drew with the system-font overlay outside
    /// the debug view (F3 or Tab).
    std::size_t overlayVerticesDrawn() const { return m_overlayVertices; }
    /// The driving line's chevrons as the last frame drew them, empty when
    /// it drew none.
    const std::vector<DrivingLine::Vertex>& drivingLineDrawn() const { return m_drivingLineDrawn; }
    /// The mixer snapshots' state; null without sound.
    const audio::SnapshotMixer* mixSnapshots() const { return m_snapshots ? &*m_snapshots : nullptr; }
    /// The mixer volume of the category's bus as last set; 0 without sound.
    float audioCategoryVolume(audio::MixCategory category) const;
    /// How loud the player's engine reaches the master (see
    /// audio::EngineSound::engineGain); 0 without its sound.
    float playerEngineGain() const;
    /// The UI sounds played so far, by their ui4audio.xml names.
    std::vector<std::string> uiSoundsPlayed() const;
    bool uiSoundPlaying(std::string_view name) const;
    /// What the HUD would be given this frame.
    ui::HudState currentHudState() const { return hudState(0.0F); }
    /// What the full map would show this frame.
    ui::WorldMapState currentMapState() const { return worldMapState(); }
    /// The speed traps and the Horizon outposts; null when the game mode
    /// lists none.
    const SpeedTraps* speedTraps() const { return m_speedTraps ? &*m_speedTraps : nullptr; }
    const Outposts* outposts() const { return m_outposts ? &*m_outposts : nullptr; }
    /// World chunks of activation zones' set dressing drawn after the last
    /// frame.
    std::size_t activityPropChunksDrawn() const { return m_activityProps ? m_activityProps->chunksDrawn() : 0; }

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
        /// the car is then not drawn.
        std::shared_ptr<render::CarRenderer> model;
        /// Who drives it, which decides its AI's targets and when it goes.
        enum class Role { Player, Racer, Traffic, Festival } role = Role::Racer;
        /// The line an AI follows, for putting it back when it is stuck.
        const RacingLine* line = nullptr;
        int lastGear = 1;
        CarControls controls;
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
    std::unique_ptr<Vehicle> makeVehicle(
        int carId, int colourSequence = 1, std::optional<carmodel::CarModel> model = std::nullopt) const;
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
    /// Runs the tyre effect rules for every car and advances the particles.
    void updateParticles(float dt);
    void drawParticles(const render::FrameParams& frame);
    void attachEngineSound(Vehicle& vehicle, bool spatial) const;
    audio::EngineSoundOptions engineSoundOptions(bool spatial) const;
    void updateAudio(float dt);
    /// The bus of a mixer snapshot category; the master without snapshots.
    audio::BusId categoryBus(audio::MixCategory category) const;
    /// Advances the snapshot fades by `dt` seconds of real time and sets
    /// the category buses to the result.
    void updateMix(float dt);
    void applyMix(audio::MixSnapshotId snapshot);
    void playUiSound(std::string_view name);
    /// Pauses or unpauses the game as CPauseGame does: the world stops (the
    /// callers skip its update) and the Paused mix comes in or goes.
    void setPaused(bool paused);

    void startRace(const RaceEvent& event);
    /// Leaves the race; `toFreeRoam` also brings the FreeRoam mix back.
    void endRace(bool toFreeRoam = true);
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
    /// Loads the game mode's activities and what the world shows of them.
    void loadActivities();
    /// Speed traps and outposts, for the player's car having moved from
    /// `from` this frame.
    void updateActivities(const glm::vec3& from);
    void updateCamera(float dt);
    render::FrameParams frameParams() const;
    void drawVehicles(const render::FrameParams& frame);
    void drawVehicleGlass(const render::FrameParams& frame);
    render::CarPose carPose(const Vehicle& vehicle) const;
    void drawDrivingLine(const glm::mat4& viewProjection);
    void drawHud(float dt);
    bool menuOpen() const;
    /// The pad's menu controls, with the keyboard's arrows, Enter, Escape,
    /// M and WASD/Q/E standing in for the D-pad, A, B, Back and the sticks.
    ui::MenuInput readMenuInput();
    /// Runs the open pause menu or map; returns false when the player
    /// quits the game from the pause menu.
    bool updateMenus(const ui::MenuInput& input, float dt);
    ui::WorldMapState worldMapState() const;
    ui::HudState hudState(float dt) const;
    /// The map entities of discovered speed traps and outposts, with their
    /// names in `names` when given.
    std::vector<ui::MapMarker> activityMapMarkers(std::vector<std::string>* names = nullptr) const;
    void updateRadio(float dt);
    /// Draws the loading screen straight to the window, for use while the
    /// world loads.
    void drawLoading();
    /// Runs `work` on another thread and keeps drawing the loading screen
    /// until it returns; for loads that need no GL context. Rethrows what
    /// `work` throws.
    template <typename Work> auto loadWhileDrawing(Work&& work) -> decltype(work());
    /// Advances the start of play (see "Loading" in Game.cpp).
    void updateLoading(float dt);
    /// What CWaitForStreaming and CWaitForAllCarsToLoad wait for, as far as
    /// this game has it.
    bool streamingDone() const;
    bool radioReady() const;
    /// Initial traffic cars not on the road yet (placement attempts left
    /// plus a car still loading).
    std::size_t trafficPending() const;
    /// Opacity of the fade from black, 0 once it is over.
    float fadeAlpha() const;

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
    /// The sun, moon and stars m_frame draws over the sky.
    std::unique_ptr<render::SkyObjects> m_skyObjects;
    std::unique_ptr<render::FrameStats> m_frameStats;
    render::PresentTarget m_present;
    /// The input of the frame being run.
    const InputState* m_input = nullptr;
    const InputState& input() const { return *m_input; }
    bool m_worldDrawn = false;
    bool m_hudDrawn = false;
    std::size_t m_overlayVertices = 0;
    int m_lastPhysicsSteps = 0;
    std::unique_ptr<render::WorldRenderer> m_worldRenderer;
    /// Lines and text in a system font for the debug view; what the player
    /// sees otherwise comes from the game's scenes.
    std::unique_ptr<render::Overlay> m_overlay;
    /// The game's own HUD scene; without it no HUD is drawn.
    std::unique_ptr<ui::Hud> m_hud;
    /// The pause menu (Start) and the world map (Back, or MAP in the pause
    /// menu); while either is open the simulation stands still.
    std::unique_ptr<ui::PauseMenu> m_pause;
    std::unique_ptr<ui::WorldMap> m_map;
    /// The player's navigation (CNavigationComponent): road discovery for
    /// the maps' road colours.
    std::unique_ptr<ui::Navigation> m_navigation;
    /// The game's loading screen, shown until the start is loaded.
    std::unique_ptr<ui::LoadingScreen> m_loading;
    /// Where the start of play is: waiting for the world, cars and effects
    /// (Streaming), starting the radio and the first traffic (Starting),
    /// fading in from black (Fading), then driving.
    enum class LoadPhase { Streaming, Starting, Fading, Playing };
    LoadPhase m_loadPhase = LoadPhase::Streaming;
    std::chrono::steady_clock::time_point m_loadStart = std::chrono::steady_clock::now();
    /// The fade_in state's FadeOptions (state_definitions.xml).
    struct FadeOptions {
        float duration = 0.0F;
        float hold = 0.0F;
        /// CurveType "S" rather than "Linear".
        bool sCurve = false;
        glm::vec3 colour{0.0F};
    };
    std::optional<FadeOptions> m_fadeIn;
    static std::optional<FadeOptions> loadFadeIn(const Vfs& vfs);
    float m_fadeElapsed = 0.0F;
    /// Initial traffic placements still to try (CEnableTraffic's initial
    /// spawn).
    int m_initialTrafficLeft = 0;
    /// When the loading screen was last drawn, for its tip timing.
    std::chrono::steady_clock::time_point m_loadingDrawn = std::chrono::steady_clock::now();
    /// Half a stick's travel counts as a menu direction; the threshold the
    /// game's front end uses was not traced.
    ui::MenuInputReader m_menuInput{0.5F};
    /// The map returns to the pause menu when it was opened from there.
    bool m_mapFromPause = false;
    /// Whether the map screen has started its ambience loop (CMapScreen
    /// +456).
    bool m_mapAmbienceStarted = false;
    /// The pause (pause_game) is in effect: the pause menu or the map is up.
    bool m_paused = false;
    std::unique_ptr<physics::WorldCollision> m_collision;
    /// The effects of DefaultTrack.xml; the tyre rules keep a pointer to
    /// the manager, so it stays in place once loaded.
    std::optional<particles::ParticleManager> m_particles;
    std::optional<particles::TyreEffects> m_tyreEffects;
    std::unique_ptr<particles::ParticleRenderer> m_particleRenderer;

    // Declared before the vehicles so it outlives their engine sounds, whose
    // destructors release mixer buses.
    std::unique_ptr<audio::AudioDevice> m_audio;
    std::shared_ptr<audio::AudioLibrary> m_audioLibrary;
    /// The mixer snapshots and a bus per category they set, which the
    /// sounds of that category play into.
    std::optional<audio::SnapshotMixer> m_snapshots;
    std::array<audio::BusId, audio::kMixCategoryCount> m_categoryBuses{};
    /// The UI sounds, on the GameUI bus; missing without sound.
    std::unique_ptr<ui::UiSounds> m_uiSounds;
    /// The festival radio; missing without sound or when its files are.
    std::unique_ptr<radio::Radio> m_radio;
    /// What the HUD's radio panel shows next, when the radio's own delay
    /// after a station change or new track has run out.
    std::optional<radio::RadioNowPlaying> m_radioPanel;
    std::vector<std::unique_ptr<Vehicle>> m_vehicles;
    std::optional<Smashables> m_smashables;
    /// The discount signs and which were smashed; missing if the game
    /// mode lists none.
    std::optional<Flyers> m_flyers;
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
    /// Where each race can be started in free roam: the activation zone of
    /// the career_event_activations.xml activity whose zone object is its
    /// HorizonEventID, as an index into m_events.
    struct EventStart {
        std::size_t event = 0;
        glm::vec3 position{0.0F};
        float radius = 0.0F;
        std::optional<float> maxSpeed;
        /// The zone's set dressing in m_activityProps.
        std::optional<std::size_t> props;
    };
    std::vector<EventStart> m_eventStarts;
    /// The start the player is standing in, if any.
    std::optional<std::size_t> m_nearbyStart;
    std::optional<ActivityCatalogue> m_activities;
    std::optional<SpeedTraps> m_speedTraps;
    std::optional<Outposts> m_outposts;
    std::unique_ptr<ActivityProps> m_activityProps;
    /// The discovered outpost whose zone the player is in, if any.
    std::optional<std::size_t> m_nearbyOutpost;
    /// The activities' texts, looked up once.
    struct ActivityTexts {
        std::string enterNow;
        std::vector<std::string> outpostTitles;
        std::vector<std::string> outpostNames;
        std::vector<std::string> outpostMapNames;
        std::vector<std::string> speedTrapNames;
    };
    ActivityTexts m_activityTexts;
    std::unique_ptr<Race> m_race;
    float m_resultsTime = 0.0F;
    /// The driving line of the running race, and the speed profile its
    /// colours compare the player's speed with; the renderer is missing
    /// when the line's texture or shaders could not be loaded.
    std::optional<DrivingLine> m_drivingLine;
    std::unique_ptr<AiDriver> m_lineGuide;
    std::unique_ptr<render::DrivingLineRenderer> m_drivingLineRenderer;
    /// TrackPositioning/GreenLine/ResetBlendSpeed of globalregistry.xml.
    float m_drivingLineReveal = 0.0F;
    std::vector<DrivingLine::Vertex> m_drivingLineDrawn;

    CameraMode m_cameraMode = CameraMode::Chase;
    render::Camera m_camera;
    std::optional<CameraSettings> m_cameraSettings;
    CarCameras m_carCameras;
    /// State of the chase cameras (CCamFollow): the angle of the camera
    /// around the car and its rate, the sprung height and its rate, and the
    /// time since three wheels last touched the ground.
    float m_chaseYaw = 0.0F;
    float m_chaseYawSpeed = 0.0F;
    float m_followHeight = 0.0F;
    float m_chaseHeightSpeed = 0.0F;
    float m_followAirTime = 0.0F;
    /// The height and target offsets after Init applied the ini's height
    /// limits.
    float m_followHeightOffset = 0.0F;
    float m_followTargetYOffset = 0.0F;
    /// The chase camera the state belongs to, and the eye it last placed;
    /// a change of either starts it afresh.
    std::optional<CameraMode> m_followMode;
    glm::vec3 m_followEye{0.0F};
    /// Looking around from the driving cameras: an angle from where the
    /// camera faces, eased back to zero when the stick is let go.
    float m_lookYaw = 0.0F;
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
