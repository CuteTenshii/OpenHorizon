#include "Game.h"

#include "base/Host.h"
#include "base/Log.h"
#include "particles/ParticleEmitters.h"
#include "render/Gl.h"
#include "render/GlProgram.h"
#include "ui/Minimap.h"

#include <glm/gtc/matrix_transform.hpp>
#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>
#include <future>
#include <string_view>
#include <thread>

namespace openhorizon {

namespace {

constexpr float kPi = 3.14159265F;
constexpr float kMsToKmh = 3.6F;
/// How long a race goes on after the player finishes before free roam
/// comes back. The game shows its post-race scenes (h_post_race_*) then,
/// which are not implemented; this time is chosen, not the game's.
constexpr float kResultsSeconds = 8.0F;
/// The debug view's text and panels in the system font, not the game's.
const glm::vec4 kHudText{0.96F, 0.96F, 0.94F, 1.0F};
const glm::vec4 kHudPanel{0.02F, 0.02F, 0.03F, 0.6F};
constexpr float kMphToMs = 0.44704F;
/// Height the ground is searched from when a start point gives none.
constexpr float kSearchFromHeight = 3000.0F;
/// Road class traffic uses until the road network's segments, which name
/// each road's class, are decoded: 5 is the A-road of aiopenworld.xml.
constexpr int kTrafficRoadClass = 5;
/// Spawn distance for classes that give none; the town classes give 240
/// and 270 m, and open roads are assumed to use a little more.
constexpr float kTrafficSpawnDistance = 300.0F;
/// Festival drivers have no speed in the file; they drive like quick road
/// users.
constexpr float kFestivalDriverSpeed = 27.0F;
/// Caps on cars kept at once, for the frame rate.
constexpr int kMaxTrafficCars = 12;
constexpr int kMaxFestivalCars = 4;

std::string formatTime(float seconds)
{
    const int total = static_cast<int>(seconds * 1000.0F);
    return std::format("{}:{:02d}.{:03d}", total / 60000, (total / 1000) % 60, total % 1000);
}

std::string ordinal(int n)
{
    const int lastTwo = n % 100;
    const char* suffix = "th";
    if (lastTwo < 11 || lastTwo > 13) {
        suffix = n % 10 == 1 ? "st" : n % 10 == 2 ? "nd" : n % 10 == 3 ? "rd" : "th";
    }
    return std::format("{}{}", n, suffix);
}

/// TrackPositioning/GreenLine/ResetBlendSpeed of globalregistry.xml: the
/// seconds the driving line takes to draw out. Throws DataError without it.
float drivingLineRevealSeconds(const Vfs& vfs)
{
    constexpr const char* kPath = "media/gametunablesettings/globalregistry.xml";
    const std::optional<std::string> text = vfs.readText(kPath);
    pugi::xml_document doc;
    if (!text || !doc.load_buffer(text->data(), text->size())) {
        throw DataError("{} is missing or broken", kPath);
    }
    const pugi::xml_node value = doc.find_node([](const pugi::xml_node& node) {
        return std::string_view(node.name()) == "Value"
            && std::string_view(node.attribute("id").as_string()) == "TrackPositioning/GreenLine/ResetBlendSpeed";
    });
    if (!value) {
        throw DataError("{} has no TrackPositioning/GreenLine/ResetBlendSpeed", kPath);
    }
    return value.attribute("value").as_float();
}

bool secondTicked(float time, float dt)
{
    return static_cast<int>(time) != static_cast<int>(time - dt);
}

/// CarPhysics' gravity, which its static wheel loads use.
constexpr float kStandardGravity = 9.80665F;

/// The wheels as the tyre sounds take them (audio::TyreInput); `surfaces`
/// names the collision surface types, or is null without track collision.
std::array<audio::TyreInput, CarPhysics::kWheels> tyreInputs(
    const CarPhysics& car, const collision::SurfaceTable* surfaces)
{
    const CarSpec& spec = car.spec();
    std::array<audio::TyreInput, CarPhysics::kWheels> inputs{};
    for (std::size_t w = 0; w < inputs.size(); ++w) {
        const CarPhysics::WheelState& wheel = car.wheels()[w];
        audio::TyreInput& input = inputs[w];
        input.onGround = wheel.onGround;
        // Render geometry has no surface type, and Tires.xml maps sounds
        // only to named surfaces, so a wheel on it stays silent.
        const collision::SurfaceType* type = surfaces && wheel.ground.surface >= 0
            ? surfaces->find(static_cast<std::uint8_t>(wheel.ground.surface))
            : nullptr;
        input.surface = type ? std::string_view(type->name) : std::string_view();
        // The tyre sounds take slips over the tyre's peak. CarPhysics
        // normalises slipMagnitude by the peaks at each axle's static load
        // and keeps them private; the same computation from the car's spec
        // gives the angle and ratio on the same scale.
        const bool front = w < 2;
        const float staticLoad
            = car.mass() * kStandardGravity * (front ? spec.frontWeight : 1.0F - spec.frontWeight) * 0.5F;
        const float peakAngleDeg = std::max(1.0F, spec.compound.lateral.peakSlip(staticLoad));
        const float peakRatio = std::max(0.02F, spec.compound.longitudinal.peakSlip(staticLoad));
        input.slipAngle = glm::degrees(wheel.slipAngle) / peakAngleDeg;
        input.slipRatio = wheel.slipRatio / peakRatio;
        input.combinedSlip = wheel.slipMagnitude;
        input.load = wheel.load;
        input.spin = wheel.spin;
        input.position = wheel.hubWorld;
    }
    return inputs;
}

audio::TyreCarInput tyreCarInput(const CarPhysics& car)
{
    audio::TyreCarInput input;
    input.mass = car.mass();
    // The game flips SlipRatio when its drivetrain speed is negative; the
    // wheels' mean spin carries the same sign whichever axle drives.
    float spin = 0.0F;
    for (const CarPhysics::WheelState& wheel : car.wheels()) {
        spin += wheel.spin;
    }
    input.reversing = spin < 0.0F;
    input.position = car.position();
    return input;
}

} // namespace

template <typename Work> auto Game::loadWhileDrawing(Work&& work) -> decltype(work())
{
    auto result = std::async(std::launch::async, std::forward<Work>(work));
    // One frame at 60 Hz between checks; drawLoading() itself waits for
    // vsync when it is on.
    while (result.wait_for(std::chrono::milliseconds(16)) != std::future_status::ready) {
        drawLoading();
    }
    return result.get();
}

Game::Game(Window& window, const GameOptions& options)
    : m_window(window)
    , m_options(options)
{
    configureAllocator();
    m_vfs.mountDisc(options.gameDir);
    // The loading screen comes first and stays up through everything below
    // (see "Loading" further down).
    try {
        m_loading = std::make_unique<ui::LoadingScreen>(m_vfs, "en", std::random_device{}());
        // Loading into free roam shows the "freeroam" tips: CLoadIntoFreeRoam
        // (enter at 0x829D0F78) sets tip group 0, which the group name table
        // at 0x83329630 names "freeroam".
        m_loading->show("freeroam");
    } catch (const std::exception& e) {
        logWarning("loading screen: {}", e.what());
        m_loading.reset();
    }
    drawLoading();
    m_jobs = std::make_unique<JobPool>();
    loadWhileDrawing([this] {
        m_database = std::make_unique<Database>(m_vfs.readOrThrow("media/db/gamedb.slt"));
        m_strings = std::make_unique<Localisation>(m_vfs, "en");
    });
    m_fadeIn = loadStateFade(m_vfs, "fade_in");
    if (!m_fadeIn) {
        logWarning("fade: no fade_in state in media/gamemodes/state_definitions.xml; no fade after loading");
    }
    m_frame = std::make_unique<render::FrameRenderer>();
    m_shadowPass = std::make_unique<render::ShadowPass>();
    try {
        m_skyObjects = std::make_unique<render::SkyObjects>(m_vfs);
        m_frame->setSkyObjects(m_skyObjects.get());
    } catch (const std::exception& e) {
        logWarning("sky objects: {}; no sun, moon or stars", e.what());
    }
    try {
        const render::CloudDefinitions clouds = render::CloudDefinitions::load(m_vfs);
        m_clouds = std::make_unique<render::Clouds>(m_vfs, clouds.sets.front());
        m_frame->setClouds(m_clouds.get(), &m_cloudParameters, &m_cloudMotion);
    } catch (const std::exception& e) {
        logWarning("clouds: {}; no clouds", e.what());
    }
    m_frameStats = std::make_unique<render::FrameStats>();
    m_overlay = std::make_unique<render::Overlay>("sans-serif:bold", 26);
    drawLoading();
    try {
        m_hud = std::make_unique<ui::Hud>(m_vfs, m_options.track);
    } catch (const std::exception& e) {
        logWarning("hud: {}; no HUD", e.what());
    }
    drawLoading();
    try {
        m_drivingLineRenderer = std::make_unique<render::DrivingLineRenderer>(m_vfs);
        m_drivingLineReveal = drivingLineRevealSeconds(m_vfs);
    } catch (const std::exception& e) {
        logWarning("driving line: {}; not drawn", e.what());
    }
    try {
        m_pause = std::make_unique<ui::PauseMenu>(m_vfs);
        m_map = std::make_unique<ui::WorldMap>(m_vfs, m_options.track, "en", m_database.get());
    } catch (const std::exception& e) {
        logWarning("menus: {}; no pause menu or map", e.what());
        m_pause.reset();
        m_map.reset();
    }
    drawLoading();
    try {
        m_navigation = std::make_unique<ui::Navigation>(m_vfs, m_options.track);
    } catch (const std::exception& e) {
        logWarning("navigation: {}; no road discovery", e.what());
    }

    try {
        m_audio = std::make_unique<audio::AudioDevice>();
        m_audioLibrary = std::make_shared<audio::AudioLibrary>(m_vfs);
    } catch (const std::exception& e) {
        logWarning("audio: no sound ({})", e.what());
        m_audio.reset();
    }
    if (m_audio) {
        try {
            m_snapshots.emplace(audio::MixSnapshots::load(m_vfs));
            for (audio::BusId& bus : m_categoryBuses) {
                bus = m_audio->mixer().createBus();
            }
            // Free roam's first load runs under the Loading mix
            // (Audio.loading at the start of prepare_free_roam in
            // game_free_roam_flow.xml).
            m_snapshots->apply(audio::MixSnapshotId::Loading);
            m_crashMix.emplace(audio::CrashMix::load(m_vfs));
            updateMix(0.0F);
        } catch (const std::exception& e) {
            logWarning("audio: mixer snapshots: {}", e.what());
            m_snapshots.reset();
        }
        try {
            m_uiSounds = std::make_unique<ui::UiSounds>(
                m_vfs, m_audio->mixer(), *m_audioLibrary, categoryBus(audio::MixCategory::GameUI));
        } catch (const std::exception& e) {
            logWarning("audio: ui sounds: {}", e.what());
        }
        drawLoading();
        try {
            radio::Radio::Options radioOptions;
            // The game seeds its radio from a runtime value that was not
            // traced; any seed gives the game's sequencing.
            radioOptions.seed = std::random_device{}();
            // The music, idents and DJ are one stream here, so DJModifier
            // cannot be set apart from GameMusic; every snapshot this game
            // applies leaves DJModifier at 1.
            radioOptions.bus = categoryBus(audio::MixCategory::GameMusic);
            m_radio = loadWhileDrawing([&] { return std::make_unique<radio::Radio>(m_vfs, *m_audio, radioOptions); });
        } catch (const std::exception& e) {
            logWarning("radio: {}", e.what());
        }
    }

    const int carId = carIdByMediaName(*m_database, options.car);
    if (carId < 0) {
        throw DataError("no car named {} in the game database", options.car);
    }
    if (m_loading) {
        m_loading->setStatus("Loading cars");
    }
    // The model's files decode off this thread; its upload and programs
    // need the GL context.
    std::optional<carmodel::CarModel> playerModel = loadWhileDrawing([&]() -> std::optional<carmodel::CarModel> {
        const std::optional<Row> media
            = m_database->queryOne("SELECT MediaName FROM Data_Car WHERE Id = ?", {std::to_string(carId)});
        try {
            return carmodel::CarModel::load(m_vfs, m_database.get(), media ? media->text("MediaName") : options.car);
        } catch (const std::exception& e) {
            logWarning("car model {}: {}", options.car, e.what());
            return std::nullopt;
        }
    });
    m_vehicles.push_back(makeVehicle(carId, 1, std::move(playerModel)));
    player().role = Vehicle::Role::Player;
    drawLoading();
    try {
        m_cameraSettings = CameraSettings::load(m_vfs);
        m_carCameras = CarCameras::load(*m_cameraSettings, *m_database, carId);
    } catch (const std::exception& e) {
        logWarning("camera: {}", e.what());
    }
    loadWhileDrawing([this] { attachEngineSound(player(), false); });
    player().physics->assists().automaticGearbox = !options.manualGearbox;
    logInfo("car: {} ({:.0f} kg, {:.0f} N·m)", player().name, player().spec->massKg,
        player().spec->reference.peakTorqueNm);
    loadWorld();
    loadProfile();
}

Game::~Game()
{
    // Ending the game without QUIT or run() (the scenarios, a thrown
    // error): the same save on leaving, while every system still exists.
    try {
        saveProfileOnExit();
    } catch (const std::exception& e) {
        logWarning("profile: not saved on exit: {}", e.what());
    }
    // Both wait out their running jobs on the pool, which must outlive them;
    // the world the jobs read is a member destroyed after this body.
    m_worldRenderer.reset();
    m_collision.reset();
    m_jobs.reset();
}

std::unique_ptr<Game::Vehicle> Game::makeVehicle(
    int carId, int colourSequence, std::optional<carmodel::CarModel> model) const
{
    auto vehicle = std::make_unique<Vehicle>();
    vehicle->spec = std::make_unique<CarSpec>(loadCarSpec(*m_database, carId, &m_vfs));
    vehicle->physics = std::make_unique<CarPhysics>(*vehicle->spec);
    const CarSpec& spec = *vehicle->spec;
    // The requested factory colour, or the car's first when the sequence
    // is out of its range. RGB is packed 0xRRGGBB in sRGB.
    const std::optional<Row> paint = m_database->queryOne(
        "SELECT RGB, Metallic FROM Combo_Colors WHERE Ordinal = ? ORDER BY Sequence = ? DESC, Sequence LIMIT 1",
        {std::to_string(carId), std::to_string(colourSequence)});
    if (paint) {
        const auto rgb = static_cast<std::uint32_t>(paint->integer("RGB"));
        const auto linear = [](std::uint32_t channel) {
            return std::pow(static_cast<float>(channel & 0xFFU) / 255.0F, 2.2F);
        };
        vehicle->paint = glm::vec3(linear(rgb >> 16), linear(rgb >> 8), linear(rgb));
        vehicle->metallic = paint->i("Metallic") != 0;
    }
    if (const auto cached = m_modelCache.find(carId); cached != m_modelCache.end()) {
        vehicle->model = cached->second;
    } else {
        try {
            if (!model) {
                model = carmodel::CarModel::load(m_vfs, m_database.get(), spec.mediaName);
            }
            vehicle->model = std::make_shared<render::CarRenderer>(*model);
        } catch (const std::exception& e) {
            logWarning("car model {}: {}", spec.mediaName, e.what());
        }
        m_modelCache.emplace(carId, vehicle->model);
    }
    const std::optional<Row> names = m_database->queryOne(
        "SELECT c.Year, m.DisplayName AS Make, c.DisplayName FROM Data_Car c "
        "LEFT JOIN List_CarMake m ON m.Id = c.MakeID WHERE c.Id = ?",
        {std::to_string(carId)});
    if (names) {
        vehicle->name = std::format("{} {} {}", names->i("Year"),
            m_strings->resolve("List_CarMake", names->text("Make")),
            m_strings->resolve("Data_Car", names->text("DisplayName")));
    } else {
        vehicle->name = spec.mediaName;
    }

    for (const CarSpec::CollisionSphere& sphere : worldContactSpheres(spec)) {
        vehicle->spheres.push_back({glm::vec3(sphere.centre[0], sphere.centre[1], sphere.centre[2]), sphere.radius});
    }
    vehicle->bodySpheres = carContactSpheres(spec);
    vehicle->bodyBox = carContactIsBox(spec);
    for (const CarSpec::CollisionSphere& sphere : vehicle->bodySpheres) {
        const glm::vec3 centre(sphere.centre[0], sphere.centre[1], sphere.centre[2]);
        vehicle->bodyReach = std::max(vehicle->bodyReach, glm::length(centre) + sphere.radius);
    }
    const glm::vec3 far = glm::max(glm::abs(glm::vec3(spec.boundsMin[0], spec.boundsMin[1], spec.boundsMin[2])),
        glm::abs(glm::vec3(spec.boundsMax[0], spec.boundsMax[1], spec.boundsMax[2])));
    vehicle->bodyReach = std::max(vehicle->bodyReach, glm::length(far));
    return vehicle;
}

void Game::drawLoading()
{
    m_window.pumpEvents();
    m_present.resize(m_window.width(), m_window.height());
    m_present.bind();
    // Black behind the loading screen, and alone without its scenes.
    glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    if (m_loading && m_loading->isShown()) {
        const auto now = std::chrono::steady_clock::now();
        m_loading->update(std::chrono::duration<float>(now - m_loadingDrawn).count());
        m_loadingDrawn = now;
        m_loading->draw(m_present.width(), m_present.height());
    }
    m_present.present();
    m_window.swapBuffers();
}

void Game::loadCrowd()
{
    try {
        const auto start = std::chrono::steady_clock::now();
        // The files decode off this thread; the upload needs the GL context.
        m_crowd = loadWhileDrawing([this] {
            return std::make_unique<spectators::CrowdSet>(spectators::CrowdSet::load(m_vfs, m_options.track));
        });
        m_crowdRenderer = std::make_unique<render::CrowdRenderer>(*m_crowd);
        logInfo("crowd: ready in {:.1f} s",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    } catch (const std::exception& e) {
        logWarning("crowd: {}; no crowds", e.what());
        m_crowdRenderer.reset();
        m_crowd.reset();
    }
    drawLoading();
}

void Game::loadWorld()
{
    if (m_loading) {
        m_loading->setStatus("Loading the world");
    }
    drawLoading();
    const auto start = std::chrono::steady_clock::now();
    // Progress arrives from whichever loader thread finished a batch; only
    // the main thread may draw.
    const std::thread::id mainThread = std::this_thread::get_id();
    m_world = world::TrackWorld::load(m_vfs, m_options.track, [&](std::size_t, std::size_t) {
        if (std::this_thread::get_id() == mainThread) {
            drawLoading();
        }
    });
    if (!m_world) {
        throw DataError("track {} could not be loaded", m_options.track);
    }
    for (const std::string& warning : m_world->warnings) {
        logWarning("world: {}", warning);
    }
    logInfo("world: {} chunks indexed in {:.1f} s", m_world->index.chunks().size(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    m_worldRenderer = std::make_unique<render::WorldRenderer>(*m_world, *m_jobs);
    loadEnvironmentCubes();
    drawLoading();
    m_collision = std::make_unique<physics::WorldCollision>(*m_world, *m_jobs, m_vfs, m_options.track);
    drawLoading();
    try {
        m_smashables = Smashables::load(m_vfs, m_options.track, *m_world);
        m_collision->setExcludedChunks(m_smashables->chunkMask());
    } catch (const std::exception& e) {
        logWarning("smashables: {}", e.what());
    }
    if (m_audio && m_collision->surfaces()) {
        try {
            m_gameplaySounds = std::make_unique<GameplaySounds>(m_vfs, *m_database, m_audio->mixer(), *m_audioLibrary,
                *m_collision->surfaces(),
                GameplaySounds::Buses{
                    categoryBus(audio::MixCategory::Collision), categoryBus(audio::MixCategory::GameUI)});
        } catch (const std::exception& e) {
            logWarning("gameplay sounds: {}", e.what());
        }
    }
    try {
        m_flyers = Flyers::load(m_vfs, m_options.track);
    } catch (const std::exception& e) {
        logWarning("flyers: {}", e.what());
    }
    try {
        m_particles.emplace(particles::ParticleManager::load(m_vfs));
        m_tyreEffects.emplace(*m_particles, particles::SurfaceEffectTable::load(m_vfs));
        m_particleRenderer = std::make_unique<particles::ParticleRenderer>(m_vfs);
        logInfo("particles: {} systems", m_particles->systems().size());
    } catch (const std::exception& e) {
        logWarning("particles: {}", e.what());
        m_tyreEffects.reset();
        m_particles.reset();
    }
    try {
        const auto animatedStart = std::chrono::steady_clock::now();
        if (std::optional<animated::AnimatedObjects> objects = animated::AnimatedObjects::load(*m_world->archive)) {
            m_animatedObjects.emplace(std::move(*objects));
            m_animatedRenderer
                = std::make_unique<render::AnimatedObjectRenderer>(*m_animatedObjects, *m_world, *m_jobs);
            logInfo("animated objects: {} objects placed {} times ({} sets unreadable, {} placements without "
                    "their object) in {:.2f} s",
                m_animatedObjects->objects().size(), m_animatedObjects->instances().size(),
                m_animatedObjects->failedSets(), m_animatedObjects->missingObjects(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - animatedStart).count());
        }
    } catch (const std::exception& e) {
        logWarning("animated objects: {}", e.what());
        m_animatedRenderer.reset();
        m_animatedObjects.reset();
    }
    m_hiddenChunks.assign(m_world->index.chunks().size(), false);
    drawLoading();
    loadCrowd();

    m_objects = GameObjects::load(m_vfs, m_options.track);
    m_timeOfDay.emplace(TimeOfDayCurves::load(m_vfs, m_options.track));
    if (m_options.startTime) {
        m_timeOfDay->setSeconds(*m_options.startTime);
    }
    m_postProcessing.emplace(PostProcessing::load(m_vfs, m_options.track));

    drawLoading();
    m_events = loadRaceEvents(*m_database, *m_strings, m_options.track);
    loadTraffic();
    loadActivities();

    if (m_options.spawnAt) {
        // No height is given, so the ground is searched from well above the
        // highest point of any track.
        m_spawnPosition = glm::vec3(m_options.spawnAt->x, kSearchFromHeight, m_options.spawnAt->y);
        m_spawnHeading = m_options.spawnHeading.value_or(0.0F);
    } else if (const GameObject* object
               = m_options.spawnObject.empty() ? nullptr : m_objects->find(m_options.spawnObject)) {
        m_spawnPosition = object->position;
        m_spawnHeading = m_options.spawnHeading.value_or(object->heading());
    } else if (const auto roamStart = freeRoamStart(); roamStart && m_options.spawnObject.empty()) {
        m_spawnPosition = roamStart->first;
        m_spawnHeading = m_options.spawnHeading.value_or(roamStart->second);
    } else {
        logWarning("spawn object {} not found; starting at the map centre", m_options.spawnObject);
        const world::Footprint footprint = m_world->index.footprint();
        m_spawnPosition = glm::vec3(footprint.centre().x, 0.0F, footprint.centre().y);
    }
    // A race given on the command line starts at its own grid, so load the
    // ground there instead.
    if (!m_options.race.empty()) {
        for (const RaceEvent& event : m_events) {
            if (iequals(event.horizonId, m_options.race)) {
                try {
                    const RaceRoute route = RaceRoute::load(m_vfs, m_options.track, event.routeId);
                    if (!route.grid.empty()) {
                        m_spawnPosition = route.grid.front().position;
                    }
                } catch (const std::exception& e) {
                    logWarning("race {}: {}", event.horizonId, e.what());
                }
            }
        }
    }
    logInfo("spawn: X {:.1f}, Z {:.1f}, heading {:.0f}°", m_spawnPosition.x, m_spawnPosition.z,
        m_spawnHeading * 180.0F / kPi);
    m_camera.position = m_spawnPosition + glm::vec3(0.0F, 30.0F, 0.0F);
    m_camera.target = m_spawnPosition;
}

std::optional<std::pair<glm::vec3, float>> Game::freeRoamStart() const
{
    const std::optional<std::string> text = m_vfs.readText("media/gamemodes/game_free_roam_flow.xml");
    if (!text) {
        return std::nullopt;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(text->data(), text->size())) {
        return std::nullopt;
    }
    const pugi::xpath_node place
        = doc.select_node("//State[@id='place_car_post_credits']/Behaviour[@id='CPlaceCar']");
    if (!place) {
        return std::nullopt;
    }
    const pugi::xml_node node = place.node();
    const glm::vec3 position(
        node.attribute("x").as_float(), node.attribute("y").as_float(), node.attribute("z").as_float());
    // Degrees about the up axis, measured from north the way headings are
    // here (checked against the road the start sits on).
    return std::pair(position, glm::radians(node.attribute("rotation").as_float()));
}

void Game::placeVehicle(Vehicle& vehicle, const glm::vec3& position, float heading)
{
    // Start just above the ground so the suspension settles onto it.
    glm::vec3 at = position;
    if (const std::optional<float> ground = m_collision->groundHeight(position.x, position.z, position.y + 50.0F)) {
        at.y = *ground + 0.05F;
    }
    vehicle.physics->reset(at, heading, *m_collision);
    vehicle.controls = CarControls{};
}

const CarPhysics* Game::parkedCar() const
{
    // Traffic that falls behind and a race start drop vehicles, so the one
    // placed is looked for rather than kept.
    const auto it = std::find_if(m_vehicles.begin(), m_vehicles.end(),
        [&](const auto& vehicle) { return vehicle->physics.get() == m_parkedCar; });
    return m_parkedCar && it != m_vehicles.end() ? m_parkedCar : nullptr;
}

void Game::spawnPlayer()
{
    placeVehicle(player(), m_spawnPosition, m_spawnHeading);
    m_playerPlaced = true;
    m_chaseVelocity = glm::vec3(0.0F);
    logInfo("car placed at height {:.1f}", player().physics->position().y);
    if (!m_options.parkedCar.empty()) {
        try {
            std::unique_ptr<Vehicle> parked = makeVehicle(carIdByMediaName(*m_database, m_options.parkedCar), 1);
            parked->role = Vehicle::Role::Traffic;
            const glm::vec2 at = m_options.parkedAt;
            placeVehicle(*parked, glm::vec3(at.x, m_spawnPosition.y, at.y), m_options.parkedHeading);
            // Nobody drives it; its handbrake holds it where it stands.
            parked->controls.handbrake = 1.0F;
            m_parkedCar = parked->physics.get();
            m_vehicles.push_back(std::move(parked));
        } catch (const std::exception& e) {
            logWarning("parked car {}: {}", m_options.parkedCar, e.what());
        }
    }
    if (!m_options.race.empty()) {
        const auto it = std::find_if(m_events.begin(), m_events.end(),
            [&](const RaceEvent& e) { return iequals(e.horizonId, m_options.race); });
        if (it == m_events.end()) {
            logWarning("race {} not found", m_options.race);
        } else {
            startRace(*it);
        }
    }
}

// Loading
//
// What the game does between the title and driving, from prepare_free_roam
// in media/gamemodes/game_free_roam_flow.xml and the behaviours its states
// run (media/gamemodes/state_definitions.xml, code traced in the
// executable):
//
// 1. Audio.loading (the Loading mix), with the loading screen up:
//    CLoadIntoFreeRoam (0x829D0F78) shows it with tip group "freeroam".
// 2. wait_for_streaming: CWaitForStreaming. Its enter (0x82A42388) resets
//    the world loader's "streamed" flag (0x824B7E48 clears +607 of the
//    object at 0x833CAAE4); its update (0x82A42440) leaves once 0x824C1690
//    is true: every player car in the loader's list passes 0x825258B8 (its
//    model resources loaded) and the flag is set again. Where the flag is
//    set was not found; inferred from the name and the reset that it means
//    the streaming around the camera has caught up.
// 3. wait_for_all_cars_loaded: CWaitForAllCarsToLoad. Enter (0x82A35EE0)
//    clears the car manager's flag +124 and asks for a check; update
//    (0x82A35F10) leaves once the flag is set, which the manager's message
//    handler (0x82659D50) does on message (10, 0x10008). Inferred: every
//    car in the world, opponents included, has its model loaded.
// 4. start_radio (Audio.play_freeroam_music), snap_car_to_road,
//    Audio.exterior_freeroam (the FreeRoam mix).
// 5. enable_traffic_spawning_with_initial_spawn: CEnableTraffic with
//    initialSpawn, whose enter (0x829BC248) enables traffic and makes its
//    initial spawn at once; the counts are aiopenworld.xml's
//    numInitialTrafficCars and numInitialFestivalCars (inferred from the
//    names; the spawn's own code was not followed).
// 6. black_out (CBlackOut, 0x829A26C0: the screen fade set to black), then
//    show_empty_screen_with_hud_resource, which replaces the loading scene.
// 7. fade_in: CFadeIn with the state's FadeOptions (1 s, S curve, from
//    black); see fadeAlpha().
// 8. show_hud_screen, start_driving, disable_car_parking_brake,
//    unblock_pause_entry: the HUD shows and the player drives.
//
// The game waits only in 2, 3 and 7. Here the loading screen also stays up
// until the radio plays and the initial traffic cars are on the road, whose
// loads the game hides behind its black-out and fade instead: this game's
// radio starts only once its stream is decoded and traffic models load on
// workers, so without the wait both would start in view. It also waits for
// every track effect to be linked (the game's shaders are precompiled on
// the disc) and for the job pool to drain, which covers tile rebuilds at
// every level of detail in view and their textures, not just those nearby.

/// Linking track effects in slices keeps the loading screen moving; the
/// slice is our choice, a quarter of a 60 Hz frame, not the game's.
constexpr std::chrono::microseconds kPrecompileSlice{4000};

float Game::fadeAlpha() const
{
    if (m_loadPhase == LoadPhase::Streaming || m_loadPhase == LoadPhase::Starting) {
        return 1.0F;
    }
    if (m_loadPhase == LoadPhase::Playing || !m_fadeIn) {
        return 0.0F;
    }
    return fadeInOpacity(*m_fadeIn, m_fadeElapsed);
}

bool Game::streamingDone() const
{
    if (!m_playerPlaced || !m_worldRenderer->nearbyReady() || m_worldRenderer->programsPending() > 0
        || m_jobs->outstanding() > 0 || !m_collision->readyAt(player().physics->position())) {
        return false;
    }
    // Car models load with their car here, so a car in the world has its
    // model or has failed to get one; only a traffic car still loading is
    // not in yet.
    return !m_pendingSpawn;
}

bool Game::radioReady() const
{
    if (!m_radio) {
        return true;
    }
    const radio::RadioNowPlaying& playing = m_radio->nowPlaying();
    return playing.off || !playing.title.empty() || playing.djSpeaking;
}

std::size_t Game::trafficPending() const
{
    // After loading, a car loading is ordinary traffic, not the initial
    // spawn.
    const bool initialLoading = m_pendingSpawn && m_loadPhase == LoadPhase::Starting;
    return static_cast<std::size_t>(std::max(m_initialTrafficLeft, 0)) + (initialLoading ? 1U : 0U);
}

void Game::updateLoading(float dt)
{
    switch (m_loadPhase) {
    case LoadPhase::Streaming:
    case LoadPhase::Starting:
        if (m_worldRenderer->programsPending() > 0) {
            m_worldRenderer->precompile(kPrecompileSlice);
        }
        if (m_loading) {
            // This engine's own stages, in place of PLEASE WAIT (see
            // ui::LoadingScreen::setStatus).
            const std::size_t pending = m_worldRenderer->programsPending();
            m_programsToLink = std::max(m_programsToLink, pending);
            if (pending > 0) {
                m_loading->setStatus(
                    std::format("Compiling shaders {}/{}", m_programsToLink - pending, m_programsToLink));
            } else if (m_loadPhase == LoadPhase::Streaming) {
                m_loading->setStatus("Loading the world");
            } else if (trafficPending() > 0) {
                m_loading->setStatus("Loading cars");
            } else {
                m_loading->setStatus(std::nullopt);
            }
        }
        break;
    case LoadPhase::Fading:
        m_fadeElapsed += dt;
        if (fadeAlpha() <= 0.0F) {
            m_loadPhase = LoadPhase::Playing;
            logInfo("loading: driving after {:.1f} s",
                std::chrono::duration<double>(std::chrono::steady_clock::now() - m_loadStart).count());
        }
        return;
    case LoadPhase::Playing:
        return;
    }

    if (m_loadPhase == LoadPhase::Streaming) {
        if (!streamingDone()) {
            return;
        }
        if (m_radio) {
            m_radio->startFreeRoam();
        }
        // A race given in the options is already set up and has applied
        // its own mix.
        if (!m_race) {
            applyMix(audio::MixSnapshotId::FreeRoam);
        }
        if (m_trafficConfig && !m_trafficLines.empty()) {
            if (const TrafficConfig::Settings* settings = m_trafficConfig->find(m_race ? "race" : "freeroam")) {
                m_initialTrafficLeft = settings->initialTrafficCars + settings->initialFestivalCars;
            }
        }
        m_loadPhase = LoadPhase::Starting;
        logInfo("loading: streamed after {:.1f} s",
            std::chrono::duration<double>(std::chrono::steady_clock::now() - m_loadStart).count());
    }

    // The initial spawn places its cars one after another, each loading on
    // a worker; a car that finds no free road is not tried again.
    finishTrafficSpawn();
    if (!m_pendingSpawn && m_initialTrafficLeft > 0) {
        const TrafficConfig::Settings& settings = *m_trafficConfig->find(m_race ? "race" : "freeroam");
        const int traffic = static_cast<int>(std::count_if(
            m_vehicles.begin(), m_vehicles.end(), [](const auto& v) { return v->role == Vehicle::Role::Traffic; }));
        const bool festival = traffic >= settings.initialTrafficCars;
        const auto& list = festival ? settings.festival : settings.traffic;
        const auto density = list.find(kTrafficRoadClass);
        spawnTraffic(festival ? Vehicle::Role::Festival : Vehicle::Role::Traffic, settings,
            density == list.end() ? TrafficConfig::Density{} : density->second);
        --m_initialTrafficLeft;
    }
    if (trafficPending() > 0 || !radioReady() || !streamingDone()) {
        return;
    }
    if (m_loading) {
        m_loading->hide();
    }
    m_loadPhase = LoadPhase::Fading;
    m_fadeElapsed = 0.0F;
    const render::ProgramCacheStats shaders = render::programCacheStats();
    logInfo("loading: screen down after {:.1f} s; programs: {} from the cache, {} compiled, {} cached binaries "
            "refused, {:.0f} ms building them",
        std::chrono::duration<double>(std::chrono::steady_clock::now() - m_loadStart).count(), shaders.fromCache,
        shaders.compiled, shaders.rejected, std::chrono::duration<double, std::milli>(shaders.time).count());
    // Loading built and freed far more than it keeps (translations, file
    // contents, the loading screen's music).
    releaseFreedMemory();
}

bool Game::inControl() const
{
    return m_loadPhase == LoadPhase::Playing;
}

Game::LoadingStatus Game::loadingStatus() const
{
    LoadingStatus status;
    status.screenShown = m_loading ? m_loading->isShown() : m_loadPhase <= LoadPhase::Starting;
    status.fading = m_loadPhase == LoadPhase::Fading;
    status.pendingJobs = m_jobs->outstanding();
    status.nearbyWorldReady = m_worldRenderer && m_worldRenderer->nearbyReady();
    status.programsPending = m_worldRenderer ? m_worldRenderer->programsPending() : 0;
    status.playerCarReady = m_playerPlaced && m_modelCache.contains(player().spec->id) && (player().engine || !m_audio);
    status.radioReady = radioReady() && (!m_radio || m_loadPhase != LoadPhase::Streaming);
    status.trafficPending = trafficPending();
    status.message = m_loading ? m_loading->message() : std::string();
    return status;
}

CarControls Game::readPlayerControls(float /*dt*/)
{
    const InputState& in = input();
    CarControls controls;
    float keyThrottle = (in.held(Key::W) || in.held(Key::Up)) ? 1.0F : 0.0F;
    float keyBrake = (in.held(Key::S) || in.held(Key::Down)) ? 1.0F : 0.0F;
    float keySteer = 0.0F;
    if (in.held(Key::A) || in.held(Key::Left)) {
        keySteer -= 1.0F;
    }
    if (in.held(Key::D) || in.held(Key::Right)) {
        keySteer += 1.0F;
    }
    if (m_cameraMode == CameraMode::Free) {
        keyThrottle = keyBrake = keySteer = 0.0F;
    }
    // A key is a stick pushed fully over. The game smooths only the pedals
    // and handbrake (physicssettings.ini ControllerDelays); steering goes to
    // the car raw, whose own steering limits the lock and its rate.
    m_keyboardSteer = keySteer;

    controls.throttle = std::max(keyThrottle, in.axis(Axis::RightTrigger));
    controls.brake = std::max(keyBrake, in.axis(Axis::LeftTrigger));
    const float stick = in.axis(Axis::LeftX);
    controls.steer = std::abs(stick) > std::abs(m_keyboardSteer) ? stick : m_keyboardSteer;
    controls.handbrake = (in.held(Key::Space) || in.held(Button::A)) ? 1.0F : 0.0F;
    controls.shiftUp = in.pressed(Key::E) || in.pressed(Button::B);
    controls.shiftDown = in.pressed(Key::Q) || in.pressed(Button::X);
    // A stopped car with neither pedal pressed holds itself on the
    // handbrake, so it does not roll away on a slope. This is not the
    // game's rule (its physics and assist step have none; its parking
    // brake is only set around UI and event hand-overs) but asked for by
    // players; the 1 m/s threshold is chosen, not from the game.
    constexpr float kHoldSpeed = 1.0F;
    if (m_playerPlaced && controls.throttle == 0.0F && controls.brake == 0.0F
        && std::abs(player().physics->forwardSpeed()) < kHoldSpeed) {
        controls.handbrake = 1.0F;
    }
    return controls;
}

int Game::stepPhysics(float dt)
{
    int steps = 0;
    constexpr float kStep = 1.0F / CarPhysics::kStepRate;
    // At most 1/30 s of simulation per frame. When the machine cannot keep
    // up, each slow frame would otherwise owe more steps than the last and
    // the frame rate collapses; dropping the backlog slows the game instead.
    constexpr float kMaxSteps = CarPhysics::kStepRate / 30.0F;
    m_physicsAccumulator = std::min(m_physicsAccumulator + dt, kMaxSteps * kStep);
    std::vector<CarControls> controls;
    controls.reserve(m_vehicles.size());
    for (const auto& vehicle : m_vehicles) {
        controls.push_back(vehicle->controls);
    }
    while (m_physicsAccumulator >= kStep) {
        if (m_gameplaySounds) {
            m_gameplaySounds->advance(kStep);
        }
        for (std::size_t i = 0; i < m_vehicles.size(); ++i) {
            m_vehicles[i]->physics->step(controls[i], *m_collision);
            collideWithWorld(*m_vehicles[i]);
            collideSmashables(*m_vehicles[i]);
            // A gear request is one press, not one per step.
            controls[i].shiftUp = false;
            controls[i].shiftDown = false;
        }
        collideVehicles();
        m_physicsAccumulator -= kStep;
        ++steps;
    }
    return steps;
}

void Game::collideWithWorld(Vehicle& vehicle)
{
    static const CollisionSettings settings = loadCollisionSettings(m_vfs);
    const collision::SurfaceTable* surfaces = m_collision->surfaces();
    if (m_carSurfaceFriction.empty() && surfaces) {
        // The game looks the pair up by the car's surface and the one hit
        // (0x82E04258, 0x82E0B890); a pair the file does not list falls back
        // to Default/Default, whose friction is the 0.5 the game uses when
        // it finds no pair (0x82E0B9DC).
        for (std::size_t id = 0; id < surfaces->surfaces().size(); ++id) {
            const collision::CategoryPair* pair = surfaces->pair("Car", surfaces->categoryOf(id));
            const float friction = pair ? pair->friction : 0.5F;
            m_carSurfaceFriction.emplace_back(friction, pair ? pair->lowSpeedFriction.value_or(friction) : friction);
        }
    }
    const glm::mat4 transform = vehicle.physics->transform();
    m_worldContacts.clear();
    m_worldContactSurfaces.clear();
    for (const BodySphere& sphere : vehicle.spheres) {
        const glm::vec3 centre = glm::vec3(transform * glm::vec4(sphere.centre, 1.0F));
        m_contacts.clear();
        m_collision->sphereContacts(centre, sphere.radius, m_contacts);
        // The deepest few are enough; a sphere against a wall touches many
        // coplanar triangles.
        const std::size_t count = std::min<std::size_t>(m_contacts.size(), 3);
        for (std::size_t i = 0; i < count; ++i) {
            const physics::SphereContact& contact = m_contacts[i];
            WorldContact& out = m_worldContacts.emplace_back();
            m_worldContactSurfaces.push_back(contact.surface);
            out.point = contact.point;
            out.normal = contact.normal;
            out.depth = contact.depth;
            // Render geometry and trunks have no surface type; they get the
            // game's friction for a pair it does not know.
            const auto surface = static_cast<std::size_t>(contact.surface);
            if (contact.surface >= 0 && surface < m_carSurfaceFriction.size()) {
                out.friction = m_carSurfaceFriction[surface].first;
                out.lowSpeedFriction = m_carSurfaceFriction[surface].second;
            }
        }
    }
    reportWorldContacts(vehicle);
    const float impulse = vehicle.physics->resolveWorldContacts(m_worldContacts, settings);
    if (impulse > vehicle.physics->mass() * 3.0F) {
        const glm::vec3 at = vehicle.physics->position();
        logDebug("impact {:.0f} N·s at {:.1f} {:.1f} {:.1f}, {:.1f} m/s", impulse, at.x, at.y, at.z,
            vehicle.physics->speed());
    }
}

void Game::collideSmashables(Vehicle& vehicle)
{
    if (!m_smashables) {
        return;
    }
    const glm::mat4 transform = vehicle.physics->transform();
    std::vector<glm::vec4> spheres;
    spheres.reserve(vehicle.spheres.size());
    for (const BodySphere& sphere : vehicle.spheres) {
        spheres.emplace_back(glm::vec3(transform * glm::vec4(sphere.centre, 1.0F)), sphere.radius);
    }
    for (std::size_t index : m_smashables->collideCar(*vehicle.physics, spheres)) {
        const Smashables::Body& body = m_smashables->bodies()[index];
        logDebug("smashed {} ({:.0f} kg) at {:.1f} m/s", body.type, body.mass, vehicle.physics->speed());
        if (m_gameplaySounds) {
            m_gameplaySounds->smashed(body.type, body.position, vehicle.physics->speed());
        }
        // Only the player's car collects: the reward is a discount on the
        // player's upgrades and the notification is the player's.
        if (vehicle.role == Vehicle::Role::Player && m_flyers && m_flyers->collect(body.gameplayId)) {
            logInfo("discount sign {} smashed: {}/{}, upgrade discount {}", body.gameplayId,
                m_flyers->collectedCount(), m_flyers->total(), m_flyers->upgradeDiscount());
            requestProfileSave("Activity Discount Sign Smashed");
        }
        for (std::uint32_t chunk : body.chunks) {
            m_hiddenChunks[chunk] = true;
        }
        m_hiddenChanged = true;
        m_debrisHandles[index] = m_worldRenderer->addDynamic(body.finestChunks);
    }
}

void Game::updateDebris(float dt)
{
    if (!m_smashables) {
        return;
    }
    // Debris needs less precision than the cars; 120 Hz keeps it stable.
    constexpr float kStep = 1.0F / 120.0F;
    m_debrisAccumulator = std::min(m_debrisAccumulator + dt, 0.1F);
    while (m_debrisAccumulator >= kStep) {
        m_smashables->step(kStep, *m_collision);
        m_debrisAccumulator -= kStep;
    }
    for (const auto& [index, handle] : m_debrisHandles) {
        m_worldRenderer->setDynamicTransform(handle, m_smashables->bodies()[index].displacement());
    }
    if (m_hiddenChanged) {
        m_worldRenderer->setHiddenChunks(m_hiddenChunks);
        m_hiddenChanged = false;
    }
}

audio::EngineSoundOptions Game::engineSoundOptions(bool spatial) const
{
    audio::EngineSoundOptions options;
    options.spatial = spatial;
    // Opponents' gear changes and skids would crowd the mix; the player's
    // own carry the feedback.
    options.transmission = !spatial;
    // The car in focus is the player's (the one heard unplaced); the
    // snapshots set it apart from the others (FocusCar, OppCar). That the
    // FocusCar volume scales the engine is verified (0x82C7E28C multiplies
    // it into each engine layer's volume); the other categories are matched
    // to the sounds by their names.
    using audio::MixCategory;
    options.buses.engine = categoryBus(spatial ? MixCategory::OppCar : MixCategory::FocusCar);
    options.buses.backfire = categoryBus(MixCategory::Backfire);
    options.buses.transmission = categoryBus(MixCategory::Transmission);
    options.buses.tyres = categoryBus(MixCategory::Tire);
    return options;
}

audio::BusId Game::categoryBus(audio::MixCategory category) const
{
    return m_snapshots ? m_categoryBuses[static_cast<std::size_t>(category)] : audio::kMasterBus;
}

void Game::updateMix(float dt)
{
    if (!m_snapshots || !m_audio) {
        return;
    }
    if (m_crashMix && m_playerPlaced && m_crashMix->update(player().physics->speed())) {
        m_snapshots->apply(audio::MixSnapshotId::Crash);
    }
    m_snapshots->update(dt);
    for (std::size_t c = 0; c < audio::kMixCategoryCount; ++c) {
        m_audio->mixer().setBusVolume(m_categoryBuses[c], m_snapshots->volume(static_cast<audio::MixCategory>(c)));
    }
}

void Game::applyMix(audio::MixSnapshotId snapshot)
{
    if (m_snapshots) {
        m_snapshots->apply(snapshot);
    }
}

void Game::playUiSound(std::string_view name)
{
    if (m_uiSounds) {
        m_uiSounds->play(name);
    }
}

void Game::setPaused(bool paused)
{
    if (paused == m_paused) {
        return;
    }
    m_paused = paused;
    // CPauseGame (0x829DC758) applies the Paused snapshot (id 6) when the
    // game becomes paused and restores the one it replaced when it is
    // unpaused; the pause flow's pause_game and unpause_game do not ask it
    // to leave the snapshot alone.
    if (!m_snapshots) {
        return;
    }
    if (paused) {
        m_snapshots->apply(audio::MixSnapshotId::Paused);
    } else {
        m_snapshots->restore();
    }
}

void Game::attachEngineSound(Vehicle& vehicle, bool spatial) const
{
    if (!m_audio) {
        return;
    }
    try {
        vehicle.engine = std::make_unique<audio::EngineSound>(
            *m_audio, m_audioLibrary, vehicle.spec->mediaName, engineSoundOptions(spatial));
    } catch (const std::exception& e) {
        logWarning("audio: {}: {}", vehicle.spec->mediaName, e.what());
    }
}

void Game::updateParticles(float dt)
{
    if (!m_particles || !m_tyreEffects) {
        return;
    }
    // The tyre rules take the game's wheel state; what CarPhysics offers in
    // its place is noted per field (see particles/TyreEffects.h).
    constexpr float kGravity = 9.80665F;
    constexpr float kRadiansPerSecondToRpm = 9.549296F;
    for (const auto& vehicle : m_vehicles) {
        const CarPhysics& car = *vehicle->physics;
        particles::TyreCarInput input;
        input.velocity = car.velocity();
        input.right = car.right();
        input.up = car.up();
        input.forward = car.forward();
        input.position = car.position();
        const float quarterWeight = car.mass() * kGravity * 0.25F;
        for (std::size_t w = 0; w < CarPhysics::kWheels; ++w) {
            const CarPhysics::WheelState& wheel = car.wheels()[w];
            particles::TyreWheelInput& out = input.wheels[w];
            // Render geometry has no surface type and so no tyre effects.
            out.onGround = wheel.onGround && wheel.ground.surface >= 0;
            out.surface = static_cast<std::size_t>(std::max(wheel.ground.surface, 0));
            const glm::quat steer = car.orientation() * glm::angleAxis(wheel.steerAngle, glm::vec3(0.0F, 1.0F, 0.0F));
            out.frame.axle = steer * glm::vec3(1.0F, 0.0F, 0.0F);
            out.frame.row1 = steer * glm::vec3(0.0F, 1.0F, 0.0F);
            out.frame.row2 = steer * glm::vec3(0.0F, 0.0F, 1.0F);
            out.frame.position = wheel.hubWorld;
            out.radius = car.wheelRadius(static_cast<int>(w));
            out.treadSpeed = wheel.spin * out.radius;
            // Inferred stand-ins: the game's slide value (wheel +0x28) as the
            // combined slip of the tyre, the friction circle value as the
            // slip normalised to the tyre's peak.
            out.slide = std::sqrt(wheel.slipRatio * wheel.slipRatio
                + std::tan(wheel.slipAngle) * std::tan(wheel.slipAngle));
            out.frictionCircle = wheel.slipMagnitude;
            out.normalisedLoad = quarterWeight > 0.0F ? wheel.load / quarterWeight : 0.0F;
            out.wheelVelocity = car.worldPointVelocity(wheel.hubWorld);
            out.wheelRpm = wheel.spin * kRadiansPerSecondToRpm;
        }
        m_tyreEffects->emit(input);
    }
    m_particles->update(dt);
}

void Game::drawParticles(const render::FrameParams& frame)
{
    if (!m_particles || !m_particleRenderer) {
        return;
    }
    particles::ParticleFrame pf;
    pf.viewProjection = frame.projection * frame.view;
    pf.cameraPosition = m_camera.position;
    pf.viewForward = glm::normalize(m_camera.target - m_camera.position);
    pf.nearPlane = m_camera.nearPlane;
    pf.towardsSun = -frame.lighting.sunDirection;
    pf.sunColour = frame.lighting.sunColour;
    pf.ambientColour = frame.lighting.ambientTop;
    m_particleRenderer->draw(*m_particles, pf);
}

void Game::updateRadio(float dt)
{
    if (!m_radio) {
        return;
    }
    const InputState& in = input();
    // "To change radio stations press left and right on the d-pad", as the
    // game's loading tip says; Page Up and Down stand in on a keyboard.
    // While paused the pad drives the menus instead.
    if (!m_paused && (in.pressed(Button::DpadRight) || in.pressed(Key::PageDown))) {
        m_radio->nextStation();
    }
    if (!m_paused && (in.pressed(Button::DpadLeft) || in.pressed(Key::PageUp))) {
        m_radio->previousStation();
    }
    m_radio->update(dt);
    if (std::optional<radio::RadioNowPlaying> shown = m_radio->takeHudRequest()) {
        logInfo("radio: {}{}", shown->stationName,
            shown->title.empty() ? std::string() : std::format(": {} by {}", shown->title, shown->artist));
        m_radioPanel = std::move(shown);
    }
}

void Game::updateAudio(float dt)
{
    if (!m_audio) {
        return;
    }
    const glm::vec3 forward = glm::normalize(m_camera.target - m_camera.position);
    const glm::vec3 right = glm::normalize(glm::cross(m_camera.up, forward));
    m_audio->mixer().setListener(m_camera.position, right);
    for (const auto& vehicle : m_vehicles) {
        if (!vehicle->engine) {
            continue;
        }
        const CarPhysics& car = *vehicle->physics;
        if (car.gear() != vehicle->lastGear) {
            vehicle->engine->shiftGear(vehicle->lastGear, car.gear());
            vehicle->lastGear = car.gear();
        }
        vehicle->engine->updateTyres(
            tyreInputs(car, m_collision ? m_collision->surfaces() : nullptr), tyreCarInput(car), dt);
        vehicle->engine->setTransform(car.position(), glm::mat3_cast(car.orientation()));
        vehicle->engine->update(car.engineRpm(), car.throttleApplied(), dt);
    }
    if (m_gameplaySounds) {
        m_gameplaySounds->update();
    }
    // Sounds unplayed for a while go; whatever needs them again decodes them again.
    const auto now = audio::AudioLibrary::Clock::now();
    if (now - m_audioReleasedAt >= kAudioReleaseInterval) {
        m_audioReleasedAt = now;
        m_audioLibrary->releaseUnused(now, kAudioIdleBeforeRelease);
    }
}

std::uint32_t Game::soundBodyId(const Vehicle& vehicle) const
{
    // Body 0 is the static world; a car is its slot plus one, which stays
    // put while two bodies touch.
    const auto it = std::find_if(
        m_vehicles.begin(), m_vehicles.end(), [&](const auto& other) { return other.get() == &vehicle; });
    return static_cast<std::uint32_t>(it - m_vehicles.begin()) + 1;
}

void Game::reportWorldContacts(const Vehicle& vehicle)
{
    if (!m_gameplaySounds || m_worldContacts.empty()) {
        return;
    }
    const CarPhysics& car = *vehicle.physics;
    // One report per surface the body touches, from the contact meeting it
    // fastest: the game reports contacts by pair of bodies and surfaces
    // (see audio::CollisionSounds), but which of a body's contact points
    // its physics hands over was not traced. Render geometry and trunks
    // have no surface type, so collisiondata.xml has nothing for them.
    std::vector<std::pair<int, std::size_t>> fastest;
    std::vector<float> approach;
    for (std::size_t i = 0; i < m_worldContacts.size(); ++i) {
        const int surface = m_worldContactSurfaces[i];
        if (surface < 0) {
            continue;
        }
        const WorldContact& contact = m_worldContacts[i];
        const float speed = -glm::dot(car.worldPointVelocity(contact.point), contact.normal);
        const auto it
            = std::find_if(fastest.begin(), fastest.end(), [&](const auto& entry) { return entry.first == surface; });
        if (it == fastest.end()) {
            fastest.emplace_back(surface, i);
            approach.push_back(speed);
        } else if (const auto slot = static_cast<std::size_t>(it - fastest.begin()); speed > approach[slot]) {
            it->second = i;
            approach[slot] = speed;
        }
    }
    const std::size_t carSurface = m_gameplaySounds->carSurface(vehicle.spec->mediaName);
    for (const auto& [surface, index] : fastest) {
        const WorldContact& contact = m_worldContacts[index];
        audio::CollisionReport report;
        report.bodyA = soundBodyId(vehicle);
        report.bodyB = 0;
        report.surfaceA = carSurface;
        report.surfaceB = static_cast<std::size_t>(surface);
        report.position = contact.point;
        // Inferred: the velocity at which the body meets the static world
        // at the point, as the parameter's name says.
        report.relativeVelocity = car.worldPointVelocity(contact.point);
        report.carSpeed = car.speed();
        report.aiOnly = vehicle.role != Vehicle::Role::Player;
        m_gameplaySounds->report(report);
    }
}

void Game::reportCarContacts(const Vehicle& vehicleA, const Vehicle& vehicleB, std::span<const CarContact> contacts)
{
    if (!m_gameplaySounds || contacts.empty()) {
        return;
    }
    const CarPhysics& carA = *vehicleA.physics;
    const CarPhysics& carB = *vehicleB.physics;
    // The contact where the two meet fastest (the normal points out of the
    // second car towards the first); as for the world, which point the
    // game's physics reports was not traced.
    const auto closing = [&](const CarContact& contact) {
        return -glm::dot(
            carA.worldPointVelocity(contact.point) - carB.worldPointVelocity(contact.point), contact.normal);
    };
    const CarContact& contact = *std::max_element(contacts.begin(), contacts.end(),
        [&](const CarContact& a, const CarContact& b) { return closing(a) < closing(b); });
    audio::CollisionReport report;
    report.bodyA = soundBodyId(vehicleA);
    report.bodyB = soundBodyId(vehicleB);
    report.surfaceA = m_gameplaySounds->carSurface(vehicleA.spec->mediaName);
    report.surfaceB = m_gameplaySounds->carSurface(vehicleB.spec->mediaName);
    report.position = contact.point;
    report.relativeVelocity = carA.worldPointVelocity(contact.point) - carB.worldPointVelocity(contact.point);
    // Inferred: the first car's speed; no car-against-car event follows
    // CarSpeedMph.
    report.carSpeed = carA.speed();
    report.aiOnly = vehicleA.role != Vehicle::Role::Player && vehicleB.role != Vehicle::Role::Player;
    m_gameplaySounds->report(report);
}

void Game::collideVehicles()
{
    // Collision tuning from media/physics/physicssettings.ini, read once.
    static const CollisionSettings settings = loadCollisionSettings(m_vfs);
    // The game's pair response (see resolveCarContacts) applies no friction
    // between bodies, and takes CarCarRestitution as it is: the disc's file
    // misspells that key, so the default of -0.2 stays, and each contact
    // removes 0.8 of the approach speed.
    const float restitution = settings.carCarRestitution;
    // How much of a hit turns each car, by the settings' names (Player,
    // Traffic, RaceCar, RaceCarInRace, RaceCarVsTrafficInRace
    // TorqueScale). The game asks each car's dynamics for its scale
    // (virtuals at 0x82E04ACC and 0x82E04AD8); which setting each role gets
    // there has not been traced.
    const auto torqueScale = [&](const Vehicle& self, const Vehicle& other) {
        switch (self.role) {
        case Vehicle::Role::Player:
            return settings.playerTorqueScale;
        case Vehicle::Role::Traffic:
            return settings.trafficTorqueScale;
        case Vehicle::Role::Racer:
        case Vehicle::Role::Festival:
            break;
        }
        if (!m_race) {
            return settings.raceCarTorqueScale;
        }
        return other.role == Vehicle::Role::Traffic ? settings.raceCarTorqueScaleVsTrafficInRace
                                                    : settings.raceCarTorqueScaleInRace;
    };
    // The game's own shapes for car bodies (the shape types 1 and 2 of its
    // contact table at 0x8339AB60) have not been traced; Vehicle::bodySpheres
    // and bodyBox stand in for them.
    std::vector<CarContact> contacts;
    for (std::size_t a = 0; a < m_vehicles.size(); ++a) {
        for (std::size_t b = a + 1; b < m_vehicles.size(); ++b) {
            Vehicle& vehicleA = *m_vehicles[a];
            Vehicle& vehicleB = *m_vehicles[b];
            CarPhysics& carA = *vehicleA.physics;
            CarPhysics& carB = *vehicleB.physics;
            if (glm::length(carA.position() - carB.position()) > vehicleA.bodyReach + vehicleB.bodyReach) {
                continue;
            }
            contacts.clear();
            findCarContacts(
                carA, vehicleA.bodySpheres, vehicleA.bodyBox, carB, vehicleB.bodySpheres, vehicleB.bodyBox, contacts);
            if (!contacts.empty()) {
                reportCarContacts(vehicleA, vehicleB, contacts);
                resolveCarContacts(carA, carB, contacts, torqueScale(vehicleA, vehicleB),
                    torqueScale(vehicleB, vehicleA), restitution);
            }
        }
    }
}

void Game::startRace(const RaceEvent& event)
{
    endRace(false);
    // Racers take the slots after the player; traffic comes back later.
    m_vehicles.resize(1);
    RaceRoute route;
    try {
        route = RaceRoute::load(m_vfs, m_options.track, event.routeId);
    } catch (const std::exception& e) {
        logError("race {}: {}", event.horizonId, e.what());
        return;
    }
    if (route.grid.empty()) {
        logError("race {}: route {} has no start grid", event.horizonId, event.routeId);
        return;
    }
    std::optional<RacingLine> line = RacingLine::load(m_vfs, m_options.track, event.routeId);
    if (!line) {
        // Street races ship no racing line: go through the checkpoints, or
        // the waypoints, to the finish.
        std::vector<glm::vec3> points{route.grid.front().position};
        for (const RouteTransform& t : route.checkpoints.empty() ? route.waypoints : route.checkpoints) {
            points.push_back(t.position);
        }
        if (route.finish) {
            points.push_back(route.finish->position);
        }
        line = RacingLine::through(points, false);
    }
    m_race = std::make_unique<Race>(event, std::move(route), std::move(*line));
    // The race's own barriers close off the side roads of its route, and
    // its route's props (barriers, chevrons, gantries) come out in place of
    // free roam's, as loading the race's route does (0x82662448).
    m_collision->setActiveRoute(event.routeId);
    m_worldRenderer->setEventRoute(event.routeId);
    // The race's crowds join those always shown (spectators::CrowdSession:
    // the sets tagged with the event or listing its route; inferred from the
    // sets' tags and routes, the game's gate for them was not traced).
    if (m_crowdRenderer) {
        m_crowdRenderer->setSession(spectators::CrowdSession::raceOn(event.horizonId, event.routeId));
    }
    const RaceRoute& grid = m_race->route();

    // The player takes the grid slot the event names, the back of the grid
    // when it names none; opponents fill the rest in their listed order.
    const int slots = static_cast<int>(grid.grid.size());
    const int playerSlot
        = std::clamp(event.playerGridPosition > 0 ? event.playerGridPosition - 1 : slots - 1, 0, slots - 1);
    const auto headingOf = [](const RouteTransform& t) { return std::atan2(t.facing.x, t.facing.z); };
    const RouteTransform& own = grid.grid[static_cast<std::size_t>(playerSlot)];
    placeVehicle(player(), own.position, headingOf(own));
    m_race->addRacer(0, true, own.position);

    int opponent = 0;
    for (int slot = 0; slot < slots && opponent < static_cast<int>(event.opponents.size()); ++slot) {
        if (slot == playerSlot) {
            continue;
        }
        const RouteTransform& t = grid.grid[static_cast<std::size_t>(slot)];
        const int carId = event.opponents[static_cast<std::size_t>(opponent)];
        try {
            const std::size_t o = static_cast<std::size_t>(opponent);
            std::unique_ptr<Vehicle> ai
                = makeVehicle(carId, o < event.opponentColours.size() ? event.opponentColours[o] : 1);
            ai->physics->assists().speedSensitiveSteering = false;
            // Alternate sides of the line, as a grid does, so the field does
            // not drive nose to tail.
            const float lane = (opponent % 2 == 0 ? -1.0F : 1.0F) * 1.2F;
            ai->ai = std::make_unique<AiDriver>(m_race->line(), *ai->spec, event.aiSkill, lane);
            ai->role = Vehicle::Role::Racer;
            ai->line = &m_race->line();
            attachEngineSound(*ai, true);
            placeVehicle(*ai, t.position, headingOf(t));
            m_vehicles.push_back(std::move(ai));
            m_race->addRacer(m_vehicles.size() - 1, false, t.position);
        } catch (const std::exception& e) {
            logWarning("race {}: opponent {}: {}", event.horizonId, carId, e.what());
        }
        ++opponent;
    }
    // The player's line colours compare the car's speed with what our own
    // AI would take each point at; the game's speed planner (0x824a4e40)
    // was not traced.
    m_lineGuide = std::make_unique<AiDriver>(m_race->line(), *player().spec, 100, 0.0F);
    m_drivingLine.emplace(m_drivingLineReveal);
    if (m_options.autopilot) {
        player().ai = std::make_unique<AiDriver>(m_race->line(), *player().spec, 90, 0.0F);
        // The AI's speed profile assumes the full lock, as for every other
        // AI car; the pad assist would cut it to a few degrees at speed.
        player().physics->assists().speedSensitiveSteering = false;
    }
    m_timeOfDay->setSeconds(event.timeOfDay);
    // The race flows apply Audio.exterior_race (the Race snapshot) just
    // before start_race; the PreEvent mix of their pre-race menus has no
    // counterpart here.
    applyMix(audio::MixSnapshotId::Race);
    m_resultsTime = 0.0F;
    m_chaseVelocity = glm::vec3(0.0F);
    m_camera.position
        = player().physics->position() - player().physics->forward() * 8.0F + glm::vec3(0.0F, 3.0F, 0.0F);
    logInfo("race {} \"{}\": {} laps, {} racers, {:.0f} m, line {} points{}", event.horizonId, event.name, event.laps,
        m_race->racers().size(), m_race->distance(), m_race->line().points.size(),
        m_race->line().loop ? " (loop)" : "");
}

void Game::endRace(bool toFreeRoam)
{
    if (!m_race) {
        return;
    }
    // Leaving a race goes back through prepare_free_roam, which ends with
    // the FreeRoam snapshot (inferred from the flows; the loading between
    // has no counterpart here).
    if (toFreeRoam) {
        applyMix(audio::MixSnapshotId::FreeRoam);
    }
    m_vehicles.resize(1);
    m_collision->setActiveRoute(-1);
    // Free roam loads route 0 again (missions.xml load_freeroam_route,
    // 0x8298D2D8).
    m_worldRenderer->setEventRoute(render::WorldRenderer::kFreeRoamRoute);
    if (m_crowdRenderer) {
        m_crowdRenderer->setSession(spectators::CrowdSession::freeRoam());
    }
    player().ai.reset();
    player().physics->assists().speedSensitiveSteering = true;
    m_drivingLine.reset();
    m_lineGuide.reset();
    m_race.reset();
}

void Game::updateRace(float dt)
{
    if (!m_race) {
        return;
    }
    std::vector<glm::vec3> positions;
    positions.reserve(m_vehicles.size());
    for (const auto& vehicle : m_vehicles) {
        positions.push_back(vehicle->physics->position());
    }
    m_race->update(dt, positions);

    if (!m_race->started() && !player().ai) {
        // Held on the handbrake on the grid, since the foot brake at a
        // standstill selects reverse; revving is still allowed.
        player().controls.brake = 0.0F;
        player().controls.handbrake = 1.0F;
    }
    if (m_race->phase() == Race::Phase::Finished) {
        m_resultsTime += dt;
        if (!player().ai) {
            player().controls = CarControls{};
            player().controls.brake = 0.4F;
        }
        const InputState& in = input();
        if (m_resultsTime > kResultsSeconds || in.pressed(Key::Enter) || in.pressed(Button::A)) {
            logInfo("race {} finished in {} ({})", m_race->event().horizonId, formatTime(m_race->player()->finishTime),
                ordinal(m_race->player()->finishPlace));
            endRace();
        }
    }
}

float Game::gapAhead(const Vehicle& vehicle) const
{
    const glm::vec3 position = vehicle.physics->position();
    const glm::vec3 forward = vehicle.physics->forward();
    float nearest = 1e9F;
    for (const auto& other : m_vehicles) {
        if (other.get() == &vehicle) {
            continue;
        }
        const glm::vec3 offset = other->physics->position() - position;
        const float ahead = glm::dot(offset, forward);
        if (ahead <= 0.0F || ahead > 80.0F) {
            continue;
        }
        const float lateral = glm::length(offset - forward * ahead);
        if (lateral < 2.5F) {
            nearest = std::min(nearest, ahead);
        }
    }
    return nearest;
}

void Game::updateAiDrivers(float dt)
{
    for (std::size_t i = 1; i < m_vehicles.size(); ++i) {
        Vehicle& vehicle = *m_vehicles[i];
        if (!vehicle.ai) {
            continue;
        }
        float scale = 1.0F;
        if (vehicle.role == Vehicle::Role::Racer) {
            scale = m_race && m_race->started() ? 1.0F : 0.0F;
        }
        // Keep a gap to the car ahead: road users a couple of seconds,
        // racers only enough not to drive through it.
        const float speed = std::max(vehicle.physics->forwardSpeed(), 3.0F);
        const bool racing = vehicle.role == Vehicle::Role::Racer;
        const float wanted = racing ? 4.0F + speed * 0.3F : 7.0F + speed * 1.5F;
        scale *= std::clamp((gapAhead(vehicle) - 5.0F) / wanted, 0.0F, 1.0F);
        vehicle.controls = vehicle.ai->update(*vehicle.physics, dt, scale);
        if (vehicle.ai->needsRecovery() && vehicle.line) {
            // Back onto the line where it got stuck.
            const float index = vehicle.ai->lineIndex();
            const glm::vec3 direction = vehicle.line->directionAt(index);
            placeVehicle(vehicle, vehicle.line->positionAt(index), std::atan2(direction.x, direction.z));
            vehicle.ai->recovered();
        }
    }
}

void Game::loadTraffic()
{
    try {
        m_trafficConfig = TrafficConfig::load(m_vfs);
    } catch (const std::exception& e) {
        logWarning("traffic: {}", e.what());
        return;
    }
    const std::string folder = std::format("media/aiopenworld/{}/ribbon_00", toLower(m_options.track));
    for (const std::string& path : m_vfs.list(folder)) {
        const std::size_t underscore = path.rfind('_');
        if (!path.ends_with(".owt") || underscore == std::string::npos) {
            continue;
        }
        const int route = std::atoi(path.c_str() + underscore + 1);
        try {
            if (std::optional<RacingLine> line = RacingLine::load(m_vfs, m_options.track, route)) {
                m_trafficLines.push_back(std::make_unique<RacingLine>(line->reversed()));
                m_trafficLines.push_back(std::make_unique<RacingLine>(std::move(*line)));
            }
        } catch (const std::exception& e) {
            logWarning("traffic: route {}: {}", route, e.what());
        }
    }
    for (const Row& row : m_database->query("SELECT CarId FROM FreeRoamDrivers")) {
        m_festivalCars.push_back(row.i("CarId"));
    }
    logInfo("traffic: {} lanes, {} traffic cars, {} festival drivers", m_trafficLines.size(),
        m_trafficConfig->find("freeroam") ? m_trafficConfig->find("freeroam")->cars.size() : 0, m_festivalCars.size());
}

bool Game::spawnTraffic(
    Vehicle::Role role, const TrafficConfig::Settings& settings, const TrafficConfig::Density& density)
{
    const glm::vec3 centre = player().physics->position();
    const float spawnIn = density.spawnInDistance.value_or(kTrafficSpawnDistance);
    std::uniform_real_distribution<float> unit(0.0F, 1.0F);
    // Every lane point in the spawn ring, so the pick below lands on a road
    // that is actually there.
    std::vector<std::pair<const RacingLine*, float>> candidates;
    for (const auto& lane : m_trafficLines) {
        for (std::size_t i = 0; i < lane->points.size(); i += 3) {
            const glm::vec3& p = lane->points[i].position;
            const float d = glm::length(glm::vec2(p.x - centre.x, p.z - centre.z));
            if (d <= spawnIn && d >= spawnIn * 0.6F) {
                candidates.emplace_back(lane.get(), static_cast<float>(i));
            }
        }
    }
    if (candidates.empty()) {
        return false;
    }
    std::uniform_int_distribution<std::size_t> pick(0, candidates.size() - 1);
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto [linePtr, index] = candidates[pick(m_random)];
        const RacingLine& line = *linePtr;
        const glm::vec3 point = line.positionAt(index);
        const float distance = glm::length(glm::vec2(point.x - centre.x, point.z - centre.z));
        // Most cars appear driving away, so they are not met head on the
        // moment they appear.
        const glm::vec3 direction = line.directionAt(index);
        const bool away = glm::dot(direction, point - centre) > 0.0F;
        if (away != (unit(m_random) < density.spawnAwayChance)) {
            continue;
        }
        // Out of view where possible, so cars do not pop into sight.
        const glm::vec3 look = glm::normalize(m_camera.target - m_camera.position);
        const glm::vec3 toPoint = glm::normalize(point - m_camera.position);
        if (glm::dot(look, toPoint) > 0.5F && attempt < 15) {
            continue;
        }
        const bool crowded = std::any_of(m_vehicles.begin(), m_vehicles.end(), [&](const auto& v) {
            return glm::length(v->physics->position() - point) < 25.0F;
        });
        if (crowded || !m_collision->groundHeight(point.x, point.z, point.y + 20.0F)) {
            continue;
        }

        int carId = 0;
        if (role == Vehicle::Role::Festival) {
            if (m_festivalCars.empty()) {
                return false;
            }
            carId = m_festivalCars[std::uniform_int_distribution<std::size_t>(0, m_festivalCars.size() - 1)(m_random)];
        } else {
            std::vector<int> choices;
            for (const TrafficConfig::Car& car : settings.cars) {
                const auto active = std::count_if(m_vehicles.begin(), m_vehicles.end(), [&](const auto& v) {
                    return v->role == Vehicle::Role::Traffic && v->spec->id == car.carId;
                });
                if (settings.allows(density, car.carId) && (car.maxActive == 0 || active < car.maxActive)) {
                    choices.push_back(car.carId);
                }
            }
            if (choices.empty()) {
                return false;
            }
            carId = choices[std::uniform_int_distribution<std::size_t>(0, choices.size() - 1)(m_random)];
        }
        auto spawn = std::make_shared<TrafficSpawn>();
        spawn->role = role;
        spawn->carId = carId;
        spawn->colourSequence = std::uniform_int_distribution<int>(1, 6)(m_random);
        spawn->line = &line;
        spawn->lineIndex = index;
        // Keep right of the line, as the roads are driven in Colorado.
        spawn->lane = role == Vehicle::Role::Traffic ? 2.0F : 1.2F;
        spawn->skill = role == Vehicle::Role::Traffic ? 40 : 65;
        spawn->speed = density.speedMph > 0.0F ? density.speedMph * kMphToMs : kFestivalDriverSpeed;
        const std::optional<Row> media
            = m_database->queryOne("SELECT MediaName FROM Data_Car WHERE Id = ?", {std::to_string(carId)});
        if (!media) {
            logWarning("traffic: car {} is not in Data_Car", carId);
            return false;
        }
        const std::string mediaName = media->text("MediaName");
        const bool needModel = !m_modelCache.contains(carId);
        audio::AudioDevice* device = m_audio.get();
        m_pendingSpawn = spawn;
        m_jobs->submit(-1000.0F,
            [spawn, mediaName, needModel, device, library = m_audioLibrary, vfs = &m_vfs, db = m_database.get(),
                soundOptions = engineSoundOptions(true)] {
            // Either part may fail on its own; the car then comes without
            // it, as the synchronous path did.
            if (needModel) {
                try {
                    spawn->model = carmodel::CarModel::load(*vfs, db, mediaName);
                } catch (const std::exception& e) {
                    logWarning("car model {}: {}", mediaName, e.what());
                }
            }
            if (device) {
                try {
                    spawn->engine = std::make_unique<audio::EngineSound>(*device, library, mediaName, soundOptions);
                } catch (const std::exception& e) {
                    logWarning("audio: {}: {}", mediaName, e.what());
                }
            }
            spawn->ready = true;
        });
        logDebug("traffic: loading car {} for {:.0f} m out", mediaName, distance);
        return true;
    }
    return false;
}

void Game::finishTrafficSpawn()
{
    if (!m_pendingSpawn || !m_pendingSpawn->ready) {
        return;
    }
    const std::shared_ptr<TrafficSpawn> spawn = std::move(m_pendingSpawn);
    const RacingLine& line = *spawn->line;
    const glm::vec3 point = line.positionAt(spawn->lineIndex);
    const glm::vec3 direction = line.directionAt(spawn->lineIndex);
    const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), direction));
    const glm::vec3 position = point + side * spawn->lane;
    // The spot was free when the car was chosen; loading took a moment.
    const bool crowded = std::any_of(m_vehicles.begin(), m_vehicles.end(),
        [&](const auto& v) { return glm::length(v->physics->position() - position) < 25.0F; });
    if (crowded) {
        return;
    }
    try {
        std::unique_ptr<Vehicle> vehicle = makeVehicle(spawn->carId, spawn->colourSequence, std::move(spawn->model));
        vehicle->role = spawn->role;
        vehicle->line = &line;
        vehicle->physics->assists().speedSensitiveSteering = false;
        vehicle->ai = std::make_unique<AiDriver>(line, *vehicle->spec, spawn->skill, spawn->lane);
        vehicle->ai->setLineIndex(spawn->lineIndex);
        vehicle->ai->setMaxSpeed(spawn->speed);
        placeVehicle(*vehicle, position, std::atan2(direction.x, direction.z));
        // Already moving, as cars on a road are when they come into view.
        vehicle->physics->setState(vehicle->physics->position(), vehicle->physics->orientation(),
            direction * spawn->speed * 0.8F, glm::vec3(0.0F));
        vehicle->engine = std::move(spawn->engine);
        logDebug("traffic: {} {} at {:.0f} m, {:.0f} km/h",
            spawn->role == Vehicle::Role::Traffic ? "car" : "festival driver", vehicle->name,
            glm::length(position - player().physics->position()), spawn->speed * 3.6F);
        m_vehicles.push_back(std::move(vehicle));
    } catch (const std::exception& e) {
        logWarning("traffic: car {}: {}", spawn->carId, e.what());
    }
}

void Game::updateTraffic(float dt)
{
    if (!m_trafficConfig || m_trafficLines.empty() || !m_playerPlaced) {
        return;
    }
    finishTrafficSpawn();
    const TrafficConfig::Settings* settings = m_trafficConfig->find(m_race ? "race" : "freeroam");
    if (!settings) {
        return;
    }
    const auto densityOf = [](const std::map<int, TrafficConfig::Density>& list) {
        const auto it = list.find(kTrafficRoadClass);
        return it == list.end() ? TrafficConfig::Density{} : it->second;
    };
    const TrafficConfig::Density traffic = densityOf(settings->traffic);
    const TrafficConfig::Density festival = densityOf(settings->festival);
    const glm::vec3 centre = player().physics->position();

    // Cars that fell far behind go.
    const auto tooFar = [&](const std::unique_ptr<Vehicle>& v) {
        if (v->role != Vehicle::Role::Traffic && v->role != Vehicle::Role::Festival) {
            return false;
        }
        const TrafficConfig::Density& d = v->role == Vehicle::Role::Traffic ? traffic : festival;
        const float out = d.spawnOutDistance.value_or(kTrafficSpawnDistance + 30.0F);
        const float distance = glm::length(v->physics->position() - centre);
        const bool gone = distance > out || v->physics->position().y < -600.0F;
        if (gone) {
            logDebug("traffic: {} left at {:.0f} m, y {:.0f}", v->name, distance, v->physics->position().y);
        }
        return gone;
    };
    m_vehicles.erase(std::remove_if(m_vehicles.begin() + 1, m_vehicles.end(), tooFar), m_vehicles.end());
    // Models no car uses go (an engine choice); failed loads stay as null so
    // they are not retried, as does the one a pending spawn expects.
    std::erase_if(m_modelCache, [this](const auto& entry) {
        return entry.second.use_count() == 1 && !(m_pendingSpawn && m_pendingSpawn->carId == entry.first);
    });

    m_trafficTimer -= dt;
    if (m_trafficTimer > 0.0F || m_pendingSpawn) {
        return;
    }
    m_trafficTimer = 0.5F;
    // How many cars the density asks for: its middle value per 100 m of
    // the lanes within reach, sampled every few points.
    float laneMetres = 0.0F;
    const float reach = traffic.spawnOutDistance.value_or(kTrafficSpawnDistance + 30.0F);
    for (const auto& line : m_trafficLines) {
        const std::size_t step = 5;
        const float spacing = line->length / static_cast<float>(std::max<std::size_t>(line->points.size(), 1));
        for (std::size_t i = 0; i < line->points.size(); i += step) {
            const glm::vec3& p = line->points[i].position;
            if (glm::length(glm::vec2(p.x - centre.x, p.z - centre.z)) < reach) {
                laneMetres += spacing * static_cast<float>(step);
            }
        }
    }
    const auto target = [&](const TrafficConfig::Density& d, int initial, int cap) {
        const float perHundred = 0.5F * (d.min + d.max);
        return std::clamp(static_cast<int>(std::lround(perHundred * laneMetres / 100.0F)), laneMetres > 0.0F ? initial : 0, cap);
    };
    const auto count = [&](Vehicle::Role role) {
        return static_cast<int>(std::count_if(
            m_vehicles.begin(), m_vehicles.end(), [&](const auto& v) { return v->role == role; }));
    };
    if (count(Vehicle::Role::Traffic) < target(traffic, settings->initialTrafficCars, kMaxTrafficCars)) {
        spawnTraffic(Vehicle::Role::Traffic, *settings, traffic);
    } else if (count(Vehicle::Role::Festival) < target(festival, settings->initialFestivalCars, kMaxFestivalCars)) {
        spawnTraffic(Vehicle::Role::Festival, *settings, festival);
    }
}

void Game::loadActivities()
{
    try {
        m_activities = ActivityCatalogue::load(m_vfs, m_options.track);
    } catch (const std::exception& e) {
        logWarning("activities: {}", e.what());
        return;
    }
    m_activityProps = std::make_unique<ActivityProps>(m_world->index);
    // A career event starts from its activation zone: the TriggerZone of
    // the career_event_activations.xml activity whose object is the
    // event's HorizonEventID. An event without one is not offered in free
    // roam, as the game has nowhere to activate it.
    for (std::size_t i = 0; i < m_events.size(); ++i) {
        const auto zone = m_activities->zoneFor(m_events[i].horizonId);
        const GameObject* object = m_objects->find(m_events[i].horizonId);
        if (!zone || object == nullptr || zone->first->type != "ActivityCareerEventActivation") {
            continue;
        }
        EventStart start{i, object->position, zone->second->radius, zone->second->maxSpeed, std::nullopt,
            m_activationFlares.add(zone->second->object, object->position)};
        if (zone->second->isActivation) {
            start.props = m_activityProps->addGroup(ActivityProps::zoneNames(zone->second->object, *m_objects));
        }
        m_eventStarts.push_back(start);
    }
    logInfo("races: {} of {} can be started from free roam", m_eventStarts.size(), m_events.size());
    // The zones' flares, and the other SimpleEmitters gameplay switches, are
    // placed by the track's ParticleEmitters.xml.
    if (m_particles) {
        try {
            m_particles->placeSimpleEmitters(m_vfs, particles::ParticleEmitterSet::load(m_vfs, m_options.track));
            logInfo("particles: {} gameplay emitters", m_particles->simpleEmitters().size());
        } catch (const std::exception& e) {
            logWarning("particle emitters: {}", e.what());
        }
    }
    m_speedTraps = SpeedTraps::build(*m_activities, *m_objects);
    m_outposts = Outposts::build(*m_activities, *m_objects);
    // The texts are the Activities table's, and the speed traps' names are
    // List_SpeedCameras'.
    ui::UiStrings strings(m_vfs, "en");
    m_activityTexts.enterNow = strings.get("Activities", "IDS_EnterNow");
    for (const Outpost& outpost : m_outposts->all()) {
        m_activityTexts.outpostTitles.push_back(strings.get("Activities", outpost.promptTitle));
        m_activityTexts.outpostNames.push_back(strings.get("Activities", outpost.prompt));
        m_activityTexts.outpostMapNames.push_back(strings.get("Activities", outpost.mapDescription));
    }
    for (const SpeedTrap& trap : m_speedTraps->traps()) {
        m_activityTexts.speedTrapNames.push_back(strings.get("List_SpeedCameras", trap.nameStringId()));
    }
    logInfo("activities: {} speed traps, {} outposts", m_speedTraps->traps().size(), m_outposts->all().size());
}

void Game::updateEventStarts()
{
    m_nearbyStart.reset();
    if (m_activityProps) {
        // Free-roam activation zones are off during a race.
        for (const EventStart& start : m_eventStarts) {
            if (start.props) {
                m_activityProps->setShown(*start.props, !m_race);
            }
        }
    }
    for (const EventStart& start : m_eventStarts) {
        m_activationFlares.setEnabled(start.flare, !m_race);
    }
    if (m_particles) {
        std::optional<glm::vec3> car;
        if (m_playerPlaced) {
            car = player().physics->position();
        }
        m_activationFlares.update(car, *m_particles);
    }
    if (m_race || !m_playerPlaced) {
        return;
    }
    // A zone offers its event while the car is inside its radius (a sphere,
    // inferred from the equal extents of the zones the executable builds)
    // below its maxMPH.
    const CarPhysics& car = *player().physics;
    const glm::vec3 position = car.position();
    for (std::size_t i = 0; i < m_eventStarts.size(); ++i) {
        const EventStart& start = m_eventStarts[i];
        if (glm::length(start.position - position) < start.radius
            && (!start.maxSpeed || car.speed() < *start.maxSpeed)) {
            m_nearbyStart = i;
            break;
        }
    }
    const InputState& in = input();
    // Starting an event is the layout's Activate action, which the default
    // layout puts on the shift-down button, X.
    if (!menuOpen() && m_nearbyStart && (in.pressed(Key::Enter) || in.pressed(Button::X))) {
        startRace(m_events[m_eventStarts[*m_nearbyStart].event]);
    }
}

void Game::updateActivities(const glm::vec3& from)
{
    m_nearbyOutpost.reset();
    if (m_race || !m_playerPlaced) {
        return;
    }
    const CarPhysics& car = *player().physics;
    if (m_speedTraps) {
        if (const auto pass = m_speedTraps->update(from, car.position(), car.velocity())) {
            logInfo("speed trap {}: {:.1f} mph", m_speedTraps->traps()[pass->trap].name, pass->speed * 2.2369F);
            // 0x8296CF94: the shutter sound, then the SpeedCam snapshot (id
            // 25) through the call CPauseGame applies Paused with.
            if (m_gameplaySounds) {
                m_gameplaySounds->speedTrapFlash();
            }
            applyMix(audio::MixSnapshotId::SpeedCam);
            // 0x8296f728: only a pass faster than the best saves.
            if (pass->speed > pass->previousBest) {
                requestProfileSave("Activity Speed Camera: New PB");
            }
        }
    }
    if (m_outposts) {
        if (const auto found = m_outposts->updateDiscovery(car.position())) {
            logInfo("outpost {} discovered", m_outposts->all()[*found].name);
            requestProfileSave("Discovered GasStation");
        }
        m_nearbyOutpost = m_outposts->zoneAt(car.position(), car.speed());
    }
}

// The profile.
//
// The game saves its whole profile (the bags and the game state, see
// profile::GameState) on two kinds of request. The state flows' save_profile
// state (CSaveProfile) runs where the flows put it: after a car change,
// after a mission and in the event unlock map flow, none of which this game
// has yet. Micro saves are asked for from code through 0x824ad988 with a
// reason, and every caller was found: a speed trap's new personal best
// (0x8296f76c), an outpost's discovery (0x82972f24), a smashed discount
// sign (0x8298aa9c), and others for systems not here (average speed zones,
// barn finds, missions, challenges, popularity spikes). Road discovery asks
// for none; the roads driven go with the next save. 0x824ad880 turns a
// request down unless a profile is signed in, the session is of type 17, 18
// or 19 and the checks at 0x826b9450 and 0x826b8488 pass; what those
// sessions and checks are was not traced, so every request here saves. The
// game writes on a job while play goes on (0x825659e8 queues it, and a
// request during a write saves again after it); the file here is small
// enough to write at once.
//
// Leaving the game also saves, once: QUIT in the pause menu, closing the
// window, or the game object going away without either. The console game
// has no such moment (its QUIT goes back to the splash screen, and the
// console is switched off, not the game closed); the player chose it so the
// roads driven since the last save are not lost. It is this engine's rule,
// not the game's.

void Game::loadProfile()
{
    std::optional<profile::ProfileSchema> schema;
    try {
        schema = profile::ProfileSchema::load(m_vfs);
    } catch (const std::exception& e) {
        logWarning("profile: {}; progress is not saved", e.what());
        return;
    }
    m_profileStore = std::make_unique<profile::ProfileStore>(
        m_options.profileDir.empty() ? profile::ProfileStore::defaultDirectory() : m_options.profileDir);
    profile::ProfileStore::LoadResult loaded = m_profileStore->load(*schema);
    m_profile.emplace(std::move(loaded.profile));
    // The game hands the pad the profile's controller options (0x82569E20);
    // of those, the steering dead zones shape the left stick.
    const auto profileFloat = [this](std::string_view path) -> std::optional<float> {
        const std::optional<profile::PropertyValue> value = m_profile->value(path);
        const float* number = value ? std::get_if<float>(&*value) : nullptr;
        return number ? std::optional<float>(*number) : std::nullopt;
    };
    const std::optional<float> steerInside = profileFloat("ControllerAdvancedOptions/SteeringAxisDeadzoneInside");
    const std::optional<float> steerOutside = profileFloat("ControllerAdvancedOptions/SteeringAxisDeadzoneOutside");
    if (steerInside && steerOutside) {
        m_window.setLeftStickDeadZone(controllerSteeringDeadZone(*steerInside, *steerOutside));
    }
    const profile::GameState& state = m_profile->state;
    if (m_navigation && state.roads) {
        m_navigation->coverage().restoreVisited(state.roads->visited);
    }
    if (m_speedTraps) {
        for (const auto& [name, camera] : state.speedCameras) {
            m_speedTraps->restore(name, camera.discovered, camera.personalBest);
        }
    }
    if (m_outposts) {
        for (const auto& [name, station] : state.gasStations) {
            if (station.discovered) {
                m_outposts->restoreDiscovered(name);
            }
        }
    }
    if (m_flyers) {
        std::vector<std::string> collected;
        for (const auto& [name, flyer] : state.flyers) {
            if (flyer.collected) {
                collected.push_back(name);
            }
        }
        m_flyers->restore(collected);
    }
    logInfo("profile: {} from {}", loaded.status == profile::ProfileStore::LoadStatus::Loaded ? "loaded" : "new",
        m_profileStore->file().string());
}

void Game::requestProfileSave(std::string_view reason)
{
    if (m_profile) {
        logInfo("profile: save requested ({})", reason);
        m_profileSaveRequested = true;
    }
}

void Game::saveProfileOnExit()
{
    if (m_exitSaved || !m_profile) {
        return;
    }
    m_exitSaved = true;
    logInfo("profile: saving on exit");
    saveProfile();
}

void Game::saveProfile()
{
    m_profileSaveRequested = false;
    if (!m_profile || !m_profileStore) {
        return;
    }
    profile::GameState& state = m_profile->state;
    if (m_navigation) {
        state.roads = profile::RoadDiscoveryState{m_navigation->coverage().visitedNodes()};
    }
    if (m_speedTraps) {
        for (const SpeedTrap& trap : m_speedTraps->traps()) {
            // The activity's own discovered flag (+40) is kept as loaded.
            profile::SpeedCameraState& camera = state.speedCameras[trap.name];
            camera.discovered = trap.discovered;
            camera.personalBest = trap.best.value_or(0.0F);
        }
    }
    if (m_outposts) {
        for (const Outpost& outpost : m_outposts->all()) {
            state.gasStations[outpost.name].discovered = outpost.discovered;
        }
    }
    if (m_flyers) {
        for (const std::string& id : m_flyers->ids()) {
            state.flyers[id].collected = m_flyers->isCollected(id);
        }
        // Main/UpgradeDiscount follows the smashed count (0x8298d4f8).
        m_profile->set("Main/UpgradeDiscount", std::uint64_t{m_flyers->upgradeDiscount()});
    }
    if (m_profileStore->save(*m_profile)) {
        ++m_profileSaves;
    }
}

void Game::updateCamera(float dt)
{
    const InputState& in = input();
    const CameraSettings* ini = m_cameraSettings ? &*m_cameraSettings : nullptr;
    const auto setting = [ini](std::string_view key, float fallback) { return ini ? ini->get(key, fallback) : fallback; };
    if (m_cameraMode == CameraMode::Free || !m_playerPlaced) {
        const float look = 0.0025F;
        if (m_window.mouseCaptured()) {
            m_freeYaw += in.mouseDx * look;
            m_freePitch = std::clamp(m_freePitch - in.mouseDy * look, -1.5F, 1.5F);
        }
        m_freeYaw += in.axis(Axis::RightX) * dt * setting("FreeCam\\YawSpeed", 3.0F);
        m_freePitch = std::clamp(
            m_freePitch + in.axis(Axis::RightY) * dt * setting("FreeCam\\PitchSpeed", 1.5F), -1.5F, 1.5F);
        const glm::vec3 forward(std::sin(m_freeYaw) * std::cos(m_freePitch), std::sin(m_freePitch),
            std::cos(m_freeYaw) * std::cos(m_freePitch));
        const glm::vec3 right = glm::normalize(glm::vec3(std::cos(m_freeYaw), 0.0F, -std::sin(m_freeYaw)));
        glm::vec3 move(0.0F);
        if (m_cameraMode == CameraMode::Free) {
            const float boost = (in.held(Key::Shift) ? 4.0F : 1.0F) * (1.0F + in.axis(Axis::RightTrigger) * 4.0F);
            const float forwardSpeed = setting("FreeCam\\MoveForwardBackwardSpeed", 75.0F) * boost;
            const float sideSpeed = setting("FreeCam\\MoveLeftRightSpeed", 40.0F) * boost;
            const float upSpeed = setting("FreeCam\\MoveUpDownSpeed", 20.0F) * boost;
            const float ahead = (in.held(Key::W) ? 1.0F : 0.0F) - (in.held(Key::S) ? 1.0F : 0.0F) + in.axis(Axis::LeftY);
            const float across = (in.held(Key::D) ? 1.0F : 0.0F) - (in.held(Key::A) ? 1.0F : 0.0F) + in.axis(Axis::LeftX);
            const float rise = (in.held(Key::E) ? 1.0F : 0.0F) - (in.held(Key::Q) ? 1.0F : 0.0F);
            move = forward * ahead * forwardSpeed + right * across * sideSpeed + glm::vec3(0.0F, rise * upSpeed, 0.0F);
        }
        m_camera.position += move * dt;
        m_camera.target = m_camera.position + forward;
        m_camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
        m_camera.fov = 1.05F;
        m_camera.nearPlane = render::Camera{}.nearPlane;
        m_followMode.reset();
        return;
    }

    const CarPhysics& car = *player().physics;
    const CarSpec& spec = *player().spec;
    const glm::mat4 transform = car.transform();
    const glm::vec3 carPosition = car.position();

    // Looking around: the right stick, or the mouse while its right button
    // is held, turns the view; letting go eases it back. Clicking the stick
    // (or holding V) looks behind. This is this engine's own control; the
    // game's look controller (0x829028D0) is not reproduced.
    const float previousLookYaw = m_lookYaw;
    const bool lookBack = in.held(Button::RightStick) || in.held(Key::V);
    const float stickYaw = in.axis(Axis::RightX) * kPi * 0.75F;
    if (in.mouseRight) {
        m_window.setMouseCaptured(true);
        m_lookYaw = std::clamp(m_lookYaw + in.mouseDx * 0.004F, -kPi, kPi);
    } else {
        if (m_window.mouseCaptured() && m_cameraMode != CameraMode::Free) {
            m_window.setMouseCaptured(false);
        }
        const float ease = std::min(1.0F, dt * 8.0F);
        m_lookYaw += ((lookBack ? kPi : stickYaw) - m_lookYaw) * ease;
    }

    // The car's box as the cameras see it (the body's slot 220 in
    // CCarDynamics: centre, then half extents). Only its top, rear and
    // front are used: centre.y + half.y, half.z - centre.z and
    // centre.z + half.z.
    const float boxTop = spec.boundsMax[1];
    const float boxRear = -spec.boundsMin[2];
    const float boxFront = spec.boundsMax[2];
    // Height of the ground under a wheel in car space: the hub less the
    // tyre radius (0x8290D52C for the chase cameras, 0x82901A40 for the
    // bumper camera). The hub is taken from its rest point and compression
    // rather than its world position, which is stale until the first
    // physics step after a placement.
    const auto groundUnderWheel = [&](int wheel) {
        const auto index = static_cast<std::size_t>(wheel);
        return car.wheelRestPosition(wheel).y + car.wheels()[index].compression - car.wheelRadius(wheel);
    };

    // The hood and bumper cameras share CCamHood/CCamBumperHigh::Update
    // (0x8290CD80): the eye is a point fixed in car space and the view
    // follows the car's axes, turned by the look angle.
    if (m_cameraMode == CameraMode::Hood || m_cameraMode == CameraMode::Bumper) {
        const bool hood = m_cameraMode == CameraMode::Hood;
        // Hood (0x82901AB8): (0, 0.45, 0.6) plus the car's offsets, plus a
        // vector of the car's physics body (CCarDynamics slot 1128, filled
        // from the body descriptor's bytes 100-111 at 0x82DEAE60) whose
        // source was not traced; it is left out.
        // Bumper (0x82901990): RestHeightAboveGround above the ground under
        // front wheel 0, 0.6096 m back from the front of the box, plus the
        // Z offset.
        const glm::vec3 eye = hood
            ? glm::vec3(0.0F, 0.45F + m_carCameras.hoodHeight, 0.6F + m_carCameras.hoodZ)
            : glm::vec3(0.0F, groundUnderWheel(CarPhysics::FrontLeft) + m_carCameras.bumperRestHeight,
                  boxFront - 0.6096F + m_carCameras.bumperZ);
        m_camera.position = glm::vec3(transform * glm::vec4(eye, 1.0F));
        // Hood (0x82901B68): the car's forward axis turned about its right
        // axis by minus the pitch offset, then about its up axis by the look
        // angle. Bumper (0x82901810): forward and right mixed by the look
        // angle, no pitch.
        const glm::vec3 right = car.right();
        const glm::vec3 up = car.up();
        glm::vec3 look = car.forward();
        if (hood) {
            look = glm::vec3(glm::rotate(glm::mat4(1.0F), -m_carCameras.hoodPitch, right) * glm::vec4(look, 0.0F));
        }
        look = right * std::sin(m_lookYaw) + look * std::cos(m_lookYaw);
        m_camera.target = m_camera.position + look;
        m_camera.up = up;
        m_camera.fov = glm::radians(hood ? m_carCameras.hoodFovDegrees : m_carCameras.bumperFovDegrees);
        m_camera.nearPlane = hood ? m_carCameras.hoodNearClip : m_carCameras.bumperNearClip;
        m_followMode.reset();
        return;
    }

    const FollowCamera& cam = m_cameraMode == CameraMode::Far ? m_carCameras.far : m_carCameras.near;
    // CCamFollow::Init (0x8290D358) runs when the camera becomes active;
    // here also when something else has moved the camera (a spawn or the
    // start of a race placing it behind the car).
    const bool reset = m_followMode != m_cameraMode || m_camera.position != m_followEye;
    if (reset) {
        m_followMode = m_cameraMode;
        m_followHeightOffset = cam.heightOffset;
        m_followTargetYOffset = cam.targetYOffset;
        if (!cam.overridden) {
            // Bring the rest height above the ground within the ini's limits
            // by moving the height and the target together; the ground is
            // read under rear wheel 2.
            const float rest = -groundUnderWheel(CarPhysics::RearLeft) + boxTop + cam.baseHeight + cam.heightOffset;
            const float shift
                = std::min(cam.heightMaxAboveGround - rest, 0.0F) + std::max(cam.heightMinAboveGround - rest, 0.0F);
            m_followHeightOffset += shift;
            m_followTargetYOffset += shift;
        }
        m_followAirTime = 0.0F;
    }

    int wheelsOnGround = 0;
    float springScale = 0.0F;
    const collision::SurfaceTable* surfaces = m_collision ? m_collision->surfaces() : nullptr;
    for (const CarPhysics::WheelState& wheel : car.wheels()) {
        if (!wheel.onGround) {
            continue;
        }
        ++wheelsOnGround;
        // Render geometry, which stands in where the game has collision
        // meshes of its own, has no surface type; it counts as 1, the
        // value of the paved types.
        const collision::SurfaceType* type = surfaces && wheel.ground.surface >= 0
            ? surfaces->find(static_cast<std::uint8_t>(wheel.ground.surface))
            : nullptr;
        springScale += type ? type->chaseCamSpringMultiplier : 1.0F;
    }

    // Heading (0x82902128): the angle of the car's backward axis measured
    // from +x towards +z, followed by a spring whose damping works against
    // the camera's own rate plus YRotCarDampScale of the car's yaw rate. The
    // yaw rate turns this angle the other way, hence the sum. The spring is
    // scaled by the mean surface multiplier of the wheels on the ground.
    const glm::vec3 forward = car.forward();
    const float heading = std::atan2(-forward.z, -forward.x);
    const float step = std::abs(dt);
    if (reset) {
        m_chaseYawSpeed = 0.0F;
        m_chaseYaw = heading;
    } else {
        float springK = cam.yawSpringK;
        float springD = cam.yawSpringD;
        if (wheelsOnGround > 0) {
            const float scale = springScale / static_cast<float>(wheelsOnGround);
            springK *= scale;
            springD *= scale;
        }
        const float error = std::remainder(heading - m_chaseYaw, 2.0F * kPi);
        const float yawRate = car.angularVelocity().y * (dt >= 0.0F ? 1.0F : -1.0F);
        m_chaseYawSpeed += step * (springK * error - springD * (yawRate * cam.yawCarDampScale + m_chaseYawSpeed));
        m_chaseYawSpeed = std::clamp(m_chaseYawSpeed, -cam.yawMaxSpeed, cam.yawMaxSpeed);
        m_chaseYaw = std::remainder(m_chaseYaw + step * m_chaseYawSpeed, 2.0F * kPi);
    }

    // Distance (0x828FE320): from the car's origin to the rear of its box,
    // plus the class constant and the radius offset.
    const float radius = boxRear + cam.baseRadius + cam.radiusOffset;
    const float angle = m_chaseYaw - m_lookYaw;
    const float previousAngle = m_chaseYaw - previousLookYaw;

    // Where the plane of the car rises or falls along the camera's
    // direction, the camera rises or falls with it (0x828FDC50): the slope
    // of the car's up axis crossed with the horizontal at right angles to
    // that direction, times the distance, within twice the distance either
    // way. Behind a car going downhill the camera climbs; uphill it drops.
    const glm::vec3 up = car.up();
    const auto slopeRise = [&](float a) {
        const glm::vec3 along = glm::cross(up, glm::vec3(-std::sin(a), 0.0F, std::cos(a)));
        const float horizontal = std::sqrt(along.x * along.x + along.z * along.z);
        const float rise = horizontal > 0.0F ? radius * along.y / horizontal : 0.0F;
        return std::clamp(rise, -2.0F * radius, 2.0F * radius);
    };

    // Height (0x8290B220): a spring towards the top of the box plus the
    // class constant, and the slope rise while three or more wheels have
    // touched the ground within the last 0.2 s and the car is upright.
    // Changes of the look angle move the spring's position with the rise so
    // they do not set it swinging.
    m_followAirTime = wheelsOnGround >= 3 ? 0.0F : m_followAirTime + step;
    float wanted = boxTop + cam.baseHeight;
    if (m_followAirTime < 0.2F && up.y > 0.7F) {
        const float rise = slopeRise(angle);
        m_followHeight += rise - slopeRise(previousAngle);
        wanted += rise;
    }
    if (reset) {
        m_chaseHeightSpeed = 0.0F;
        m_followHeight = wanted;
    } else {
        const float springK = wheelsOnGround == 0 ? cam.heightSpringKInAir : cam.heightSpringK;
        m_chaseHeightSpeed += step * (springK * (wanted - m_followHeight) - cam.heightSpringD * m_chaseHeightSpeed);
        m_followHeight += step * m_chaseHeightSpeed;
    }

    glm::vec3 eye = carPosition
        + glm::vec3(radius * std::cos(angle), m_followHeightOffset + m_followHeight, radius * std::sin(angle));
    const glm::vec3 target = carPosition + glm::vec3(0.0F, boxTop + m_followTargetYOffset, 0.0F);

    // Collision (CPhysicsWorld slot 136, 0x82E0E278): from the target
    // towards the eye, the eye stops at the first surface hit, no nearer
    // than 0.1 m. The game sweeps a 0.2 m sphere; WorldCollision offers a
    // ray only, so the eye can come to rest on the surface itself.
    const glm::vec3 toEye = eye - target;
    const float reach = glm::length(toEye);
    if (m_collision && reach > 0.0F) {
        const glm::vec3 direction = toEye / reach;
        GroundHit hit;
        if (m_collision->raycast(target, direction, reach, hit)) {
            eye = target + direction * std::clamp(hit.distance, 0.1F, reach);
        }
    }

    m_camera.position = eye;
    m_camera.target = target;
    m_camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
    m_camera.fov = glm::radians(cam.fovDegrees);
    m_camera.nearPlane = cam.nearClip;
    m_followEye = eye;
}

void Game::drawAnimatedObjects(const render::FrameParams& frame)
{
    if (!m_animatedObjects || !m_animatedRenderer) {
        return;
    }
    // The objects advance by the presentation clock's step, which is 0
    // while the game is paused (see animated::AnimatedObjects).
    const float step = std::max(frame.time - m_animatedClock, 0.0F);
    m_animatedClock = frame.time;
    animated::AnimatedView view;
    view.camera = frame.cameraPosition;
    view.viewProjection = frame.projection * frame.view;
    view.switchOnLights = frame.lighting.switchOnLights;
    view.route = m_worldRenderer->eventRoute();
    m_animatedObjects->update(step, view);
    m_animatedRenderer->setShadowMask(m_shadowPass->mask());
    m_animatedRenderer->draw(frame);
}

render::FrameParams Game::frameParams() const
{
    render::FrameParams frame;
    const float aspect = static_cast<float>(std::max(m_present.width(), 1))
        / static_cast<float>(std::max(m_present.height(), 1));
    frame.view = m_camera.view();
    frame.projection = m_camera.projection(aspect);
    frame.cameraPosition = m_camera.position;
    frame.lighting = m_timeOfDay->lighting();
    frame.time = m_time;
    return frame;
}

void Game::drawShadows(const render::FrameParams& frame)
{
    const int width = m_present.width();
    const int height = m_present.height();
    if (m_shadowPass->resizes(width, height)) {
        m_worldRenderer->setShadowMask(0);
        render::CarRenderer::setShadowMask(0);
    }
    // Where the light comes from when the track does not override it was
    // not traced (see world::TrackSettings), so such a track gets none.
    const std::optional<glm::vec3> direction
        = m_world && m_world->settings ? m_world->settings->shadowLightDirection : std::nullopt;
    const float intensity = frame.lighting.shadowIntensity;
    if (!direction || !render::ShadowPass::active(intensity)) {
        m_shadowPass->clearMask(width, height);
    } else {
        render::ShadowCamera camera;
        camera.position = m_camera.position;
        camera.forward = glm::normalize(m_camera.target - m_camera.position);
        camera.right = glm::normalize(glm::cross(m_camera.up, camera.forward));
        camera.up = glm::cross(camera.forward, camera.right);
        camera.nearPlane = m_camera.nearPlane;
        camera.tanHalfHeight = std::tan(m_camera.fov * 0.5F);
        camera.tanHalfWidth
            = camera.tanHalfHeight * static_cast<float>(std::max(width, 1)) / static_cast<float>(std::max(height, 1));
        m_shadowCascades.update(camera, *direction);
        for (std::size_t i = 0; i < render::ShadowCascades::kLevels; ++i) {
            const render::ShadowCascades::Level& level = m_shadowCascades.levels()[i];
            if (level.dirty) {
                m_shadowPass->beginLevel(i);
                m_worldRenderer->drawShadowCasters(level.frustum.viewProjection, camera.position);
                m_shadowCascades.markDrawn(i);
            }
        }
        for (std::size_t i = 0; i < render::ShadowCascades::kCascades; ++i) {
            m_shadowPass->beginCascade(i);
            for (const auto& vehicle : m_vehicles) {
                if (vehicle->model && m_playerPlaced) {
                    vehicle->model->drawDepth(
                        m_shadowCascades.cascades()[i].frustum.viewProjection, carPose(*vehicle), false);
                }
            }
        }
        m_shadowPass->endShadowMaps();
        const glm::mat4 viewProjection = frame.projection * frame.view;
        const glm::mat4 flipped = render::ShadowPass::flippedViewProjection(viewProjection);
        m_shadowPass->beginDepth(width, height);
        // The world only: the cars cover their own pixels of the mask with
        // their SMask technique in the game, which is not drawn here (where
        // the game draws it was not traced), and their lighting takes the
        // sun from their own terms rather than the mask.
        m_worldRenderer->drawDepthOnly(
            flipped, camera.position, m_shadowCascades.levels().back().frustum.viewProjection);
        m_shadowPass->endDepth();
        m_shadowPass->buildMask(m_shadowCascades, camera, viewProjection, intensity);
    }
    m_worldRenderer->setShadowMask(m_shadowPass->mask());
    render::CarRenderer::setShadowMask(m_shadowPass->mask());
}

void Game::loadEnvironmentCubes()
{
    try {
        m_envMapFrequencyScale
            = RenderScenario::stacked(m_vfs, RenderScenario::kInGame).envMapFrequencyScale.value_or(0.0F);
    } catch (const std::exception& e) {
        logWarning("render scenario: {}; the environment cube is not updated", e.what());
    }
    // The presentation tries the track's numbered cube first
    // (staticCarCubemap%d.xpr, numbered by a field of the presentation not
    // traced; the disc has none), then the track's, then the shared one.
    for (const std::string& path : {std::format("media/tracks/{}/staticCarCubemap.xpr", m_options.track),
             std::string("media/tracks/staticCarCubemap.xpr")}) {
        if (const std::optional<Bytes> file = m_vfs.read(path)) {
            try {
                // The car shaders square what they fetch, so the texels are
                // taken as stored.
                m_staticCarCube = render::CubeMap::fromCube(world::readXprCube(*file), false);
            } catch (const std::exception& e) {
                logWarning("{}: {}", path, e.what());
            }
            break;
        }
    }
    if (m_world && m_world->settings && m_world->settings->dynamicCubemap) {
        m_environmentCube = std::make_unique<render::EnvironmentCube>();
    }
    render::CarRenderer::setEnvironmentCubes(
        m_environmentCube ? m_environmentCube->texture() : 0, m_staticCarCube ? m_staticCarCube->id() : 0);
}

void Game::drawEnvironmentCube(const render::FrameParams& frame)
{
    if (!m_environmentCube || !m_playerPlaced || !m_world || !m_world->settings) {
        return;
    }
    const world::TrackSettings& settings = *m_world->settings;
    const float reach = settings.cubemapFarDistance;
    // Where the driver can pick layers in the vertex shader the world goes
    // into every face in one pass, sending each batch once; the sky is drawn
    // face by face either way.
    const bool layered = m_worldRenderer->cubeFacesLayered();
    const render::EnvironmentCube::DrawFaces world
        = [&](const render::FrameParams& centred, std::span<const glm::mat4> transforms, std::span<const int> layers) {
        m_worldRenderer->drawCubeFaces(centred, transforms, layers, reach);
    };
    m_environmentCube->update(render::envcube::centre(player().physics->transform()), m_envMapFrequencyScale,
        settings.cubeClearColour(frame.lighting.sunDirection), frame, layered ? &world : nullptr,
        [&](const render::FrameParams& face) {
        if (!layered) {
            m_worldRenderer->drawCubeFace(face, reach);
        }
        m_frame->cubeSky(face);
    });
}

render::CarPose Game::carPose(const Vehicle& vehicle) const
{
    const CarPhysics& car = *vehicle.physics;
    render::CarPose pose;
    pose.body = car.transform();
    const glm::mat4 toCar = glm::inverse(pose.body);
    for (std::size_t w = 0; w < CarPhysics::kWheels; ++w) {
        const CarPhysics::WheelState& wheel = car.wheels()[w];
        pose.hubs[w] = glm::vec3(toCar * glm::vec4(wheel.hubWorld, 1.0F));
        pose.steer[w] = wheel.steerAngle;
        pose.spin[w] = wheel.spinAngle;
        pose.spinRate[w] = wheel.spin;
    }
    pose.brakeLights = vehicle.controls.brake > 0.1F ? 1.0F : 0.0F;
    pose.headLights = m_timeOfDay->lights();
    pose.paint = vehicle.paint;
    pose.metallic = vehicle.metallic;
    // The camera's focus car takes the static cube while the dynamic one is
    // not updated; every other car the dynamic one (see CarPose).
    const bool cubeUpdated = m_environmentCube && render::envcube::facesPerFrame(m_envMapFrequencyScale) > 0;
    pose.useStaticCubeMap = &vehicle == &player() && !cubeUpdated;
    return pose;
}

void Game::drawVehicles(const render::FrameParams& frame)
{
    if (!m_playerPlaced) {
        return;
    }
    for (const auto& vehicle : m_vehicles) {
        // A car whose model failed to load is not drawn.
        if (vehicle->model) {
            vehicle->model->draw(frame, carPose(*vehicle), false);
        }
    }
}

void Game::drawDrivingLine(const glm::mat4& viewProjection)
{
    m_drivingLineDrawn.clear();
    const Race::Racer* racer = m_race ? m_race->player() : nullptr;
    // The pause flow hides the line while paused (disable_racing_line in
    // pause.xml). Hiding it once the player has finished is inferred.
    if (!racer || racer->finished || !m_drivingLine || !m_lineGuide || !m_drivingLineRenderer
        || (m_pause && m_pause->isOpen())) {
        return;
    }
    m_drivingLineDrawn = m_drivingLine->build(m_race->line(), racer->lineIndex, player().physics->speed(),
        [this](float index) { return m_lineGuide->speedLimitAt(index); });
    m_drivingLineRenderer->draw(m_drivingLineDrawn, viewProjection);
}

void Game::drawVehicleGlass(const render::FrameParams& frame)
{
    if (!m_playerPlaced) {
        return;
    }
    for (const auto& vehicle : m_vehicles) {
        if (vehicle->model) {
            vehicle->model->draw(frame, carPose(*vehicle), true);
        }
    }
}

ui::HudState Game::hudState(float dt) const
{
    ui::HudState state;
    state.dt = dt;
    state.metric = m_options.metric;
    state.coverage = m_navigation ? &m_navigation->coverage() : nullptr;
    if (!m_playerPlaced) {
        return state;
    }
    const CarPhysics& car = *player().physics;
    const CarSpec& spec = *player().spec;
    state.speedKmh = std::abs(car.forwardSpeed()) * kMsToKmh;
    state.speedMph = state.speedKmh / 1.609344F;
    state.gear = car.gear();
    state.rpm = car.engineRpm();
    state.redlineRpm = spec.redlineRpm;
    state.maxRpm = std::max(spec.torqueCurveMaxRpm, spec.redlineRpm);
    // The lights show an assist at work, as the game's HUD does, not its
    // setting.
    const AssistActivity& assists = player().physics->assistActivity();
    state.absOn = assists.abs;
    state.stmOn = assists.stabilityControl;
    state.tcsOn = assists.tractionControl;
    const glm::vec3 position = car.position();
    const glm::vec3 forward = car.forward();
    state.playerMapPosition = {position.x, position.z};
    state.playerHeading = std::atan2(forward.x, forward.z);
    state.speedMetresPerSecond = car.speed();

    if (m_race && m_race->player()) {
        const Race::Racer& own = *m_race->player();
        state.inRace = true;
        state.position = m_race->place(own);
        state.racers = static_cast<int>(m_race->racers().size());
        state.lap = m_race->lap(own);
        state.laps = m_race->line().loop ? m_race->event().laps : 1;
        if (m_race->phase() == Race::Phase::Countdown) {
            state.countdownElapsed = Race::kCountdownSeconds - m_race->countdown();
        } else {
            state.raceTime = own.finished ? own.finishTime : m_race->elapsed();
            // The scene shows GO! once the countdown passes three seconds;
            // keep it past that point for the first moment of the race.
            if (m_race->elapsed() < 0.5F) {
                state.countdownElapsed = Race::kCountdownSeconds + m_race->elapsed();
            }
        }
        for (const Race::Racer* racer : m_race->standings()) {
            if (racer->player) {
                state.playerStanding = static_cast<int>(state.standings.size());
            }
            state.standings.push_back(m_vehicles[racer->vehicle]->name);
        }
        // The course ahead as the minimap's route, and the metres left to
        // the finish under it. Which map entity type the game gives a race's
        // course was not traced; "route" is what the reference screenshots
        // show (the profile's green line with the finish flag at its end).
        const RacingLine& line = m_race->line();
        if (!own.finished && line.points.size() > 1) {
            ui::HudMapLine course{"type", "route", {}, {}};
            course.points.push_back(state.playerMapPosition);
            const auto count = static_cast<std::ptrdiff_t>(line.points.size());
            const auto next = static_cast<std::ptrdiff_t>(std::ceil(own.lineIndex));
            const std::ptrdiff_t end = line.loop ? next + count : count;
            for (std::ptrdiff_t i = next; i < end; ++i) {
                const glm::vec3& at = line.points[static_cast<std::size_t>(i % count)].position;
                course.points.emplace_back(at.x, at.z);
            }
            state.mapLines.push_back(std::move(course));
            const float left = m_race->distance() - own.progress;
            if (left > 0.0F) {
                state.destinationDistance = left;
            }
        }
    }
    // The sat-nav route: the route entity, which the minimap draws from the
    // car's place along it (its reveal is whole outside the map), and the
    // distance left under the minimap (0x8266cf20 reads the planner's +624
    // while it has a goal; a race's own distance is kept in races).
    if (m_navigation && !m_navigation->routeNodes().empty()) {
        if (!state.destinationDistance && m_navigation->routeDistance() > 0.0F) {
            state.destinationDistance = m_navigation->routeDistance();
        }
        ui::HudMapLine route{"type", "route", m_navigation->route(), {}};
        route.ratios = ui::routeRatios(route.points.size(), m_navigation->routeProgress(), 1.0F);
        state.mapLines.push_back(std::move(route));
    }
    for (const EventStart& start : m_eventStarts) {
        const std::string_view type = ui::careerEventActivityType(m_events[start.event].careerEventStyle);
        if (!type.empty()) {
            state.mapMarkers.push_back(
                {"activity_type", std::string(type), {start.position.x, start.position.z}, 0.0F});
        }
    }
    // The game's map entities for other cars are CMapEntityVehicleAI ("ai")
    // and CMapEntityVehicleFestivalTraffic ("traffic"); festival drivers
    // counting as traffic is inferred from the class name.
    for (std::size_t i = 1; i < m_vehicles.size(); ++i) {
        const Vehicle& vehicle = *m_vehicles[i];
        const glm::vec3 at = vehicle.physics->position();
        const glm::vec3 ahead = vehicle.physics->forward();
        state.mapMarkers.push_back({"vehicle_type", vehicle.role == Vehicle::Role::Racer ? "ai" : "traffic",
            {at.x, at.z}, std::atan2(ahead.x, ahead.z)});
    }
    // The activation prompt: a zone's name, its title when it has one, and
    // its call to action, IDS_EnterNow unless the zone names another.
    if (m_nearbyStart) {
        state.prompt = m_events[m_eventStarts[*m_nearbyStart].event].name;
        state.promptDetail = m_activityTexts.enterNow;
    } else if (m_nearbyOutpost) {
        state.promptTitle = m_activityTexts.outpostTitles[*m_nearbyOutpost];
        state.prompt = m_activityTexts.outpostNames[*m_nearbyOutpost];
        state.promptDetail = m_activityTexts.enterNow;
        state.promptIcon = std::string(Outposts::kPromptIcon);
    }
    for (const ui::MapMarker& marker : activityMapMarkers()) {
        state.mapMarkers.push_back({marker.key, marker.value, marker.position, marker.heading});
    }
    if (m_speedTraps && m_speedTraps->lastPass()) {
        const SpeedTrapPass& pass = *m_speedTraps->lastPass();
        state.speedCamera = ui::HudSpeedCamera{m_speedTraps->passCount(), false, pass.speed, pass.previousBest};
    }
    return state;
}

std::vector<ui::MapMarker> Game::activityMapMarkers(std::vector<std::string>* names) const
{
    std::vector<ui::MapMarker> markers;
    // A speed trap's "speed_camera_discovered" entity shows once it is
    // discovered (0x82958398), at the middle of its gate.
    if (m_speedTraps) {
        for (std::size_t i = 0; i < m_speedTraps->traps().size(); ++i) {
            const SpeedTrap& trap = m_speedTraps->traps()[i];
            if (trap.discovered) {
                const glm::vec3 at = trap.centre();
                markers.push_back({"activity_type", "speed_camera_discovered", {at.x, at.z}, 0.0F});
                if (names) {
                    names->push_back(m_activityTexts.speedTrapNames[i]);
                }
            }
        }
    }
    // An outpost's "gas_station" entity appears with its discovery
    // (0x82972da0), at its zone's object.
    if (m_outposts) {
        for (std::size_t i = 0; i < m_outposts->all().size(); ++i) {
            const Outpost& outpost = m_outposts->all()[i];
            if (outpost.discovered) {
                markers.push_back({"activity_type", "gas_station", {outpost.position.x, outpost.position.z}, 0.0F});
                if (names) {
                    names->push_back(m_activityTexts.outpostMapNames[i]);
                }
            }
        }
    }
    return markers;
}

float Game::audioCategoryVolume(audio::MixCategory category) const
{
    return m_audio && m_snapshots ? m_audio->mixer().busVolume(m_categoryBuses[static_cast<std::size_t>(category)])
                                  : 0.0F;
}

float Game::playerEngineGain() const
{
    return player().engine ? player().engine->engineGain() : 0.0F;
}

std::vector<std::string> Game::uiSoundsPlayed() const
{
    return m_uiSounds ? m_uiSounds->played() : std::vector<std::string>();
}

bool Game::uiSoundPlaying(std::string_view name) const
{
    return m_uiSounds && m_uiSounds->isPlaying(name);
}

bool Game::menuOpen() const
{
    return (m_pause && m_pause->isOpen()) || (m_map && m_map->isOpen());
}

ui::MenuInput Game::readMenuInput()
{
    const InputState& in = input();
    ui::MenuInput menu = m_menuInput.read(in);
    menu.up = menu.up || in.pressed(Key::Up);
    menu.down = menu.down || in.pressed(Key::Down);
    menu.left = menu.left || in.pressed(Key::Left);
    menu.right = menu.right || in.pressed(Key::Right);
    menu.accept = menu.accept || in.pressed(Key::Enter);
    menu.back = menu.back || in.pressed(Key::Escape);
    menu.select = menu.select || in.pressed(Key::M);
    if (m_map && m_map->isOpen()) {
        const auto axis = [&](Key positive, Key negative) {
            return (in.held(positive) ? 1.0F : 0.0F) - (in.held(negative) ? 1.0F : 0.0F);
        };
        const glm::vec2 keys(axis(Key::D, Key::A), axis(Key::W, Key::S));
        if (glm::length(keys) > glm::length(menu.leftStick)) {
            menu.leftStick = keys;
        }
        const float zoom = axis(Key::E, Key::Q);
        if (std::abs(zoom) > std::abs(menu.rightStick.y)) {
            menu.rightStick.y = zoom;
        }
    }
    return menu;
}

ui::WorldMapState Game::worldMapState() const
{
    ui::WorldMapState state;
    state.coverage = m_navigation ? &m_navigation->coverage() : nullptr;
    if (m_navigation) {
        state.route = m_navigation->route();
        state.routeStart = m_navigation->routeProgress();
        state.routeLength = m_navigation->routeLength();
    }
    state.metric = m_options.metric;
    if (m_profile) {
        // The player bar reads these as stored (0x82597a08, 0x82996948,
        // the career progression object's Main/XP).
        const auto integer = [&](std::string_view path) -> std::int64_t {
            const std::optional<profile::PropertyValue> value = m_profile->value(path);
            if (!value) {
                return 0;
            }
            if (const auto* unsignedValue = std::get_if<std::uint64_t>(&*value)) {
                return static_cast<std::int64_t>(*unsignedValue);
            }
            if (const auto* signedValue = std::get_if<std::int64_t>(&*value)) {
                return *signedValue;
            }
            return 0;
        };
        ui::PlayerBarValues bar;
        bar.credits = integer("Main/Credits");
        bar.wristbandLevel = integer("Main/WristbandLevel");
        bar.xp = integer("Main/XP");
        if (!m_vehicles.empty() && player().spec) {
            bar.carId = player().spec->id;
        }
        state.playerBar = bar;
    }
    if (m_playerPlaced) {
        const glm::vec3 at = player().physics->position();
        const glm::vec3 ahead = player().physics->forward();
        state.playerPosition = {at.x, at.z};
        state.playerHeading = std::atan2(ahead.x, ahead.z);
    }
    for (const EventStart& start : m_eventStarts) {
        const RaceEvent& event = m_events[start.event];
        const std::string_view type = ui::careerEventActivityType(event.careerEventStyle);
        if (!type.empty()) {
            state.markers.push_back(
                {{"activity_type", std::string(type), {start.position.x, start.position.z}, 0.0F}, event.name});
        }
    }
    std::vector<std::string> names;
    const std::vector<ui::MapMarker> activities = activityMapMarkers(&names);
    for (std::size_t i = 0; i < activities.size(); ++i) {
        state.markers.push_back({activities[i], names[i]});
    }
    return state;
}

bool Game::updateMenus(const ui::MenuInput& input, float dt)
{
    constexpr std::string_view kMapAmbience = "Map_AmbientBackgroundLoop";
    if (m_map && m_map->isOpen()) {
        // CMapScreen's update (0x827B3D28) starts the map's ambience loop
        // the first time it finds the map shown. It skips the start when a
        // flag and a positive value on the object at 0x833D5BAC (+119, +120)
        // say so; what they stand for was not identified.
        if (!m_mapAmbienceStarted) {
            m_mapAmbienceStarted = true;
            playUiSound(kMapAmbience);
        }
        if (m_uiSounds) {
            // Every frame 0x82C57CC0 sets RadioIsPlaying on the UI events
            // that play, from byte +141 of the radio's state; that the byte
            // is whether the radio is on (not RADIO OFF) is inferred. The
            // loop's only sound lies in the lower half of the parameter, so
            // it is heard with the radio off.
            const bool radioOn = m_radio && !m_radio->nowPlaying().off;
            m_uiSounds->setParameter(kMapAmbience, "RadioIsPlaying", radioOn ? 1.0F : 0.0F);
        }
        m_map->setState(worldMapState());
        const ui::WorldMapAction action = m_map->update(input, dt);
        if (m_navigation && action == ui::WorldMapAction::SetWaypoint) {
            // SET ROUTE (0x828af208): the planner takes the waypoint and
            // searches the route to it.
            m_navigation->setDestination(m_map->waypoint());
            if (!m_navigation->destination()) {
                m_map->clearWaypoint();
            }
            m_map->setState(worldMapState());
        } else if (m_navigation && action == ui::WorldMapAction::ClearWaypoint) {
            m_navigation->setDestination(std::nullopt);
            m_map->setState(worldMapState());
        }
        if (action == ui::WorldMapAction::Back) {
            // B (input action 96) plays Cancel as the map goes (CMapScreen's
            // input handler, 0x827C9300); leaving the screen stops its loop
            // (0x8278D988).
            playUiSound("Cancel");
            m_map->close();
            if (m_uiSounds) {
                m_uiSounds->stop(kMapAmbience);
            }
            m_mapAmbienceStarted = false;
            if (m_mapFromPause && m_pause) {
                m_pause->reopen();
                // pause_screen.enterBack: the pause screen's entry
                // (CShowPauseScreen, 0x82A1D5B8) plays Pause again.
                playUiSound("Pause");
            }
        }
        return true;
    }
    if (!m_pause || !m_pause->isOpen()) {
        return true;
    }
    if (!m_pause->confirming()) {
        if (input.back) {
            // CPauseMenu's input handler (0x827DADC8) plays Cancel for B
            // before resuming.
            playUiSound("Cancel");
        } else if (input.accept && !m_pause->entries().empty()) {
            // Choosing an entry plays Accept in the menu list (CMenuCtrl,
            // 0x828B80A8) and again in the pause menu's handler (0x827EBEE0)
            // in the same frame; UI/Nav/Accept allows one playback.
            playUiSound("Accept");
        }
    }
    const ui::PauseAction action = m_pause->update(input, dt);
    if (!m_pause->isOpen()) {
        // Leaving pause_screen, for the game, the map or a retirement: the
        // pause screen's exit (CShowPauseScreen, 0x82A1D850).
        playUiSound("UnPause");
    }
    switch (action) {
    case ui::PauseAction::OpenMap:
        if (m_map) {
            m_mapFromPause = true;
            m_mapAmbienceStarted = false;
            m_map->open(worldMapState());
        }
        break;
    case ui::PauseAction::Restart:
        if (m_race) {
            // Unpaused before the race starts over under its own mix.
            setPaused(false);
            const RaceEvent event = m_race->event();
            endRace(false);
            startRace(event);
        }
        break;
    case ui::PauseAction::RetireFromEvent:
        setPaused(false);
        endRace();
        break;
    case ui::PauseAction::Quit:
        // QUIT leaves the game for the desktop.
        saveProfileOnExit();
        return false;
    default:
        break;
    }
    return true;
}

void Game::drawHud(float dt)
{
    const auto h = static_cast<float>(m_present.height());
    m_fps = m_fps * 0.95F + (dt > 0.0F ? 1.0F / dt : 0.0F) * 0.05F;

    // The map and the pause screen replace the HUD while they are open:
    // the HUD (CCShowHudScreen, screen 94, 0x82A181B8), the pause screen
    // (CShowPauseScreen, screen 205, 0x82A1D420) and the map are all shown
    // into layer 1 (the default the screen behaviours' base, 0x82A020D0,
    // sets), which holds one screen (0x82A002C0), and the pause flow shows
    // the HUD again on its way out (show_hud_screen before unpause_game).
    const bool mapOpen = m_map && m_map->isOpen();
    const bool hudScreen = !mapOpen && !(m_pause && m_pause->isOpen());
    m_hudDrawn = m_hud && hudScreen;
    if (m_hud && hudScreen) {
        ui::HudState state = hudState(dt);
        if (m_radioPanel) {
            state.radio = ui::HudRadioRequest{
                static_cast<int>(m_radioPanel->station), m_radioPanel->title, m_radioPanel->artist};
            m_radioPanel.reset();
        }
        m_hud->draw(state, m_present.width(), m_present.height());
    }

    m_overlayVertices += m_overlay->queuedVertices();
    if (m_showDebug) {
        const auto& stats = m_worldRenderer->stats();
        const glm::vec3 p = m_playerPlaced ? player().physics->position() : m_camera.position;
        const int hours = static_cast<int>(m_timeOfDay->seconds() / 3600.0F);
        const int minutes = static_cast<int>(m_timeOfDay->seconds() / 60.0F) % 60;
        const std::vector<std::string> lines = {
            std::format("{:.0f} fps   {:02d}:{:02d}", m_fps, hours, minutes),
            std::format("X {:.1f}  Y {:.1f}  Z {:.1f}", p.x, p.y, p.z),
            std::format("tiles {}  triangles {}k  batches {}", stats.tilesDrawn, stats.triangles / 1000, stats.batches),
            std::format(
                "textures {} ({} MB)  jobs {}", stats.texturesLoaded, stats.textureBytes >> 20, stats.pendingJobs),
            std::format("collision triangles {}k, tree trunks {}", m_collision->triangleCount() / 1000,
                m_collision->trunkCount()),
        };
        const float lineHeight = m_overlay->lineHeight(0.7F);
        const float top = m_race ? h - lineHeight * static_cast<float>(lines.size()) - 40.0F : 8.0F;
        m_overlay->rect(8.0F, top, 520.0F, lineHeight * static_cast<float>(lines.size()) + 16.0F, kHudPanel);
        float y = top + 8.0F;
        for (const std::string& line : lines) {
            m_overlay->text(18.0F, y, line, kHudText, 0.7F);
            y += lineHeight;
        }
    }
}

bool Game::saveScreenshot(const std::filesystem::path& path)
{
    // The 16:9 picture, without the window's bars.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_present.framebuffer());
    const int w = m_present.width();
    const int h = m_present.height();
    xds::Image image;
    image.width = static_cast<std::uint32_t>(w);
    image.height = static_cast<std::uint32_t>(h);
    image.rgba.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
    // OpenGL's rows run bottom-up.
    const std::size_t row = static_cast<std::size_t>(w) * 4;
    for (int y = 0; y < h / 2; ++y) {
        std::swap_ranges(image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y) * row),
            image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(y + 1) * row),
            image.rgba.begin() + static_cast<std::ptrdiff_t>(static_cast<std::size_t>(h - 1 - y) * row));
    }
    const std::vector<std::uint8_t> png = xds::writePng(image);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    if (!out) {
        logError("cannot write {}", path.string());
        return false;
    }
    logInfo("screenshot: {}", path.string());
    return true;
}

bool Game::frame(const InputState& controls, float dt)
{
    using Part = render::FrameStats::Part;
    {
        m_input = &controls;
        m_hudDrawn = false;
        m_overlayVertices = 0;
        m_frameStats->beginFrame();
        int physicsSteps = 0;
        const InputState& in = input();

        const ui::MenuInput menuInput = readMenuInput();
        if (menuOpen()) {
            if (!updateMenus(menuInput, dt)) {
                return false;
            }
        } else if (in.pressed(Key::Escape) && m_window.mouseCaptured()) {
            m_window.setMouseCaptured(false);
        } else if (m_pause && inControl() && (menuInput.start || in.pressed(Key::Escape))) {
            // Start pauses (CWaitForPause onPause) and Back opens the map
            // (onMap), as the game's default controller layout binds them.
            ui::PauseMenuContext context;
            context.freeRoam = !m_race;
            m_pause->open(context);
            // The pause screen's entry (CShowPauseScreen, 0x82A1D5B8).
            playUiSound("Pause");
        } else if (m_map && inControl() && menuInput.select) {
            m_mapFromPause = false;
            m_mapAmbienceStarted = false;
            m_map->open(worldMapState());
            // CWaitForPause plays Pause itself before it takes onMap
            // (0x82A47E98).
            playUiSound("Pause");
        } else if (!m_pause && in.pressed(Key::Escape)) {
            // Without the menu scenes Escape still leaves a race or quits.
            if (m_race) {
                endRace();
            } else {
                return false;
            }
        }
        if (in.pressed(Key::F11)) {
            m_window.toggleFullscreen();
        }
        if (in.pressed(Key::Tab) || in.pressed(Key::F3)) {
            m_showDebug = !m_showDebug;
        }
        // RB changes the view (SwitchCamera in the default layout).
        if (!menuOpen() && (in.pressed(Key::C) || in.pressed(Button::RightShoulder))) {
            m_cameraMode = m_cameraMode == CameraMode::Chase ? CameraMode::Far
                : m_cameraMode == CameraMode::Far          ? CameraMode::Bumper
                : m_cameraMode == CameraMode::Bumper       ? CameraMode::Hood
                                                           : CameraMode::Chase;
        }
        if (in.pressed(Key::F)) {
            if (m_cameraMode == CameraMode::Free) {
                m_cameraMode = CameraMode::Chase;
                m_window.setMouseCaptured(false);
            } else {
                m_cameraMode = CameraMode::Free;
                const glm::vec3 look = glm::normalize(m_camera.target - m_camera.position);
                m_freeYaw = std::atan2(look.x, look.z);
                m_freePitch = std::asin(std::clamp(look.y, -1.0F, 1.0F));
                m_window.setMouseCaptured(true);
            }
        }
        if (in.pressed(Key::R) && inControl()) {
            if (m_race && m_race->player()) {
                // Back onto the line where the player left it.
                const float index = m_race->player()->lineIndex;
                const glm::vec3 direction = m_race->line().directionAt(index);
                placeVehicle(player(), m_race->line().positionAt(index), std::atan2(direction.x, direction.z));
            } else {
                player().physics->recover(*m_collision);
            }
        }
        // After the menus, so the frame that opens the pause menu is already
        // paused, as the car's simulation is. The pause stops the world's
        // clocks with it: the time of day, and the clock the world's
        // animation runs on (the game updates its presentation's clock, the
        // track effects' TimeGain, with 0 while paused; see
        // render::TrackFrameConstants::timeGain).
        setPaused(menuOpen());
        if (!m_paused) {
            m_timeOfDay->advance(dt);
            m_time += dt;
        }
        // The zone blend follows the player's car, and the fog template it
        // blends multiplies the time of day's fog curves. Inferred: the
        // blend's clock stops while paused, as the time of day's does.
        const glm::vec3 carPosition = player().physics->position();
        m_postProcessing->update(
            m_playerPlaced ? std::optional<glm::vec2>(glm::vec2(carPosition.x, carPosition.z)) : std::nullopt,
            m_paused ? 0.0F : dt, m_timeOfDay->curves(), m_timeOfDay->minutes());
        m_timeOfDay->setFog(m_postProcessing->fog());
        if (inControl()) {
            updateEventStarts();
        }

        const glm::vec3 focus = m_playerPlaced ? player().physics->position() : m_spawnPosition;
        m_frameStats->mark(Part::Simulation);
        m_collision->update(focus);
        m_frameStats->mark(Part::Streaming);
        if (!m_playerPlaced && m_collision->readyAt(m_spawnPosition)) {
            // Wait for the ground itself, but not forever: a spawn point off
            // the map has none.
            m_spawnWait += dt;
            if (m_collision->groundHeight(m_spawnPosition.x, m_spawnPosition.z, m_spawnPosition.y + 50.0F)
                || m_spawnWait > 10.0F) {
                spawnPlayer();
            }
        }
        updateLoading(dt);
        // The pause menu and the map stop the game (pause_game in the pause
        // flow); the world keeps streaming and drawing behind them.
        if (m_playerPlaced && !menuOpen()) {
            if (inControl()) {
                const float playerScale = m_race && !m_race->started() ? 0.0F : 1.0F;
                player().controls
                    = player().ai ? player().ai->update(*player().physics, dt, playerScale) : readPlayerControls(dt);
                updateRace(dt);
                if (m_drivingLine) {
                    m_drivingLine->update(dt);
                }
            } else {
                // Until the fade is over the car stands on its parking brake,
                // which prepare_free_roam releases only after fade_in
                // (disable_car_parking_brake).
                player().controls = CarControls{};
                player().controls.handbrake = 1.0F;
            }
            // Traffic is enabled at the end of loading and drives through
            // the fade.
            if (m_loadPhase == LoadPhase::Fading || m_loadPhase == LoadPhase::Playing) {
                updateAiDrivers(dt);
                updateTraffic(dt);
            }
            m_frameStats->mark(Part::Simulation);
            const glm::vec3 before = player().physics->position();
            physicsSteps = stepPhysics(dt);
            updateActivities(before);
            if (m_navigation) {
                m_navigation->update(player().physics->position(), player().physics->forward());
                // The planner drops the waypoint when the car arrives or no
                // route reaches it; the map's goes with it.
                if (m_map && m_map->waypoint() && !m_navigation->destination()) {
                    m_map->clearWaypoint();
                }
            }
            if (m_profileSaveRequested) {
                saveProfile();
            }
            m_frameStats->mark(Part::Physics);
            updateDebris(dt);
            updateParticles(dt);
            updateAudio(dt);
            if (m_race && m_race->player() && secondTicked(m_time, dt)) {
                const Race::Racer* own = m_race->player();
                const CarPhysics& car = *player().physics;
                const auto& wheels = car.wheels();
                logDebug("race: place {}/{}, progress {:.0f}/{:.0f} m, speed {:.1f}, target {:.1f}, gear {}, rpm {:.0f}, "
                         "throttle {:.2f}/{:.2f}, brake {:.2f}, slip {:.2f} {:.2f} {:.2f} {:.2f}, ground {}{}{}{}",
                    m_race->place(*own), m_race->racers().size(), own->progress, m_race->distance(), car.forwardSpeed(),
                    player().ai ? player().ai->targetSpeed() : 0.0F, car.gear(), car.engineRpm(),
                    player().controls.throttle, car.throttleApplied(), player().controls.brake, wheels[0].slipRatio,
                    wheels[1].slipRatio, wheels[2].slipRatio, wheels[3].slipRatio, wheels[0].onGround,
                    wheels[1].onGround, wheels[2].onGround, wheels[3].onGround);
            }
            if (player().physics->position().y < -600.0F) {
                // Fell through a hole in the world.
                spawnPlayer();
            }
        }
        // The radio plays on through the pause, under the Paused mix's
        // GameMusic volume of a quarter.
        if (m_playerPlaced) {
            updateRadio(dt);
        }
        // The cameras are part of the world the pause stops: the chase
        // camera steps on the world's signed time step (its heading spring,
        // 0x82902128, takes |dt| and flips with the sign for rewinds), not
        // on real time.
        if (!m_paused) {
            updateCamera(dt);
        }
        // The mix fades on real time, through the pause too.
        updateMix(dt);
        m_frameStats->mark(Part::Simulation);
        if (m_activityProps) {
            m_activityProps->update(m_camera.position, *m_worldRenderer);
        }
        m_worldRenderer->update(m_camera.position);
        if (m_animatedRenderer) {
            m_animatedRenderer->update();
        }
        if (m_clouds) {
            // The clouds drift on the frame's real time (inferred; see
            // render::CloudMotion).
            m_cloudParameters = m_timeOfDay->clouds();
            m_cloudMotion.advance(m_cloudParameters, dt);
        }
        m_frameStats->mark(Part::Streaming);

        // The whole frame draws into a 16:9 picture that present() fits in
        // the window, as the console's output always was 16:9.
        m_present.resize(m_window.width(), m_window.height());
        m_frame->setOutputFramebuffer(m_present.framebuffer());
        // The map is its own screen: the game swaps to the map scene and
        // stops drawing the world, so nothing of it shows through.
        const bool mapOpen = m_map && m_map->isOpen();
        // Behind the loading screen the world waits for its effects, which
        // it would otherwise link one by one as it draws.
        const bool worldHidden = (m_loadPhase == LoadPhase::Streaming || m_loadPhase == LoadPhase::Starting)
            && m_worldRenderer->programsPending() > 0;
        m_worldDrawn = !mapOpen && !worldHidden;
        if (worldHidden) {
            m_present.bind();
            glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        } else if (mapOpen) {
            m_present.bind();
            glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            m_map->draw(m_present.width(), m_present.height());
        } else {
            const render::FrameParams frame = frameParams();
            drawShadows(frame);
            m_frameStats->mark(Part::DrawShadows);
            drawEnvironmentCube(frame);
            m_frameStats->mark(Part::DrawEnvironmentCube);
            m_frame->begin(m_present.width(), m_present.height());
            m_worldRenderer->draw(frame);
            m_frameStats->mark(Part::DrawWorld);
            if (m_crowdRenderer) {
                m_crowdRenderer->setShadowMask(m_shadowPass->mask());
                m_crowdRenderer->draw(frame, m_world->settings ? &*m_world->settings : nullptr);
            }
            m_frameStats->mark(Part::DrawCrowd);
            drawAnimatedObjects(frame);
            m_frameStats->mark(Part::DrawAnimated);
            drawVehicles(frame);
            m_frameStats->mark(Part::DrawCars);
            m_frame->sky(frame);
            m_frameStats->mark(Part::DrawSky);
            // The game draws the track's blending pass after the sky, so
            // what blends where no geometry lies behind blends over the sky
            // (see WorldRenderer::drawTranslucent()).
            m_worldRenderer->drawTranslucent();
            m_frameStats->mark(Part::DrawWorld);
            drawVehicleGlass(frame);
            m_frameStats->mark(Part::DrawGlass);
            drawDrivingLine(frame.projection * frame.view);
            m_frameStats->mark(Part::DrawDrivingLine);
            drawParticles(frame);
            m_frameStats->mark(Part::DrawParticles);
            m_overlayVertices = m_overlay->queuedVertices();
            m_overlay->flush(frame.projection * frame.view, m_present.width(), m_present.height());
            m_frame->finish(m_postProcessing->frame(
                static_cast<float>(m_present.width()) / static_cast<float>(m_present.height()), m_resetExposure));
            m_resetExposure = false;
            m_frameStats->mark(Part::Tonemap);
            if (const float fade = fadeAlpha(); fade > 0.0F && m_fadeIn) {
                m_overlay->rect(0.0F, 0.0F, static_cast<float>(m_present.width()),
                    static_cast<float>(m_present.height()), glm::vec4(m_fadeIn->colour, fade));
                m_overlay->flush(glm::mat4(1.0F), m_present.width(), m_present.height());
            }
            // The HUD shows once the fade is over (show_hud_screen).
            if (inControl()) {
                drawHud(dt);
            }
            m_overlay->flush(glm::mat4(1.0F), m_present.width(), m_present.height());
            m_frameStats->mark(Part::DrawHud);
        }
        if (m_pause && m_pause->isOpen()) {
            m_pause->draw(m_present.width(), m_present.height());
        }
        if (m_loading && m_loading->isShown()) {
            m_loading->update(dt);
            m_loading->draw(m_present.width(), m_present.height());
        }
        m_frameStats->mark(Part::DrawMenus);

        ++m_frameCount;
        m_present.present();
        m_frameStats->endGpuFrame();
        m_lastPhysicsSteps = physicsSteps;
    }
    return true;
}

int Game::run()
{
    auto last = std::chrono::steady_clock::now();
    while (m_window.pumpEvents()) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1F);
        last = now;
        if (!frame(m_window.input(), dt)) {
            break;
        }
        m_window.swapBuffers();
        m_frameStats->mark(render::FrameStats::Part::Present);
        m_frameStats->endFrame(m_lastPhysicsSteps, m_vehicles.size());
    }
    // The window closed, or QUIT (which saved already).
    saveProfileOnExit();
    return 0;
}

} // namespace openhorizon
