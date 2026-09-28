#include "Game.h"

#include "base/Log.h"
#include "render/Gl.h"

#include <glm/gtc/matrix_transform.hpp>
#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <fstream>

namespace fh {

namespace {

constexpr float kPi = 3.14159265F;
constexpr float kMsToKmh = 3.6F;
/// Keyboard steering eases towards full lock and back, as a stick would.
constexpr float kKeySteerRate = 2.5F;
constexpr float kKeyReturnRate = 4.0F;
/// How long the results stay up after the player finishes.
constexpr float kResultsSeconds = 8.0F;
const glm::vec4 kHudText{0.96F, 0.96F, 0.94F, 1.0F};
const glm::vec4 kHudDim{0.96F, 0.96F, 0.94F, 0.55F};
const glm::vec4 kHudPanel{0.02F, 0.02F, 0.03F, 0.6F};
/// The HUD's one accent: the rev band near the redline, the selected event
/// and the player's row in the standings.
const glm::vec4 kHudAccent{1.0F, 0.42F, 0.1F, 1.0F};
const glm::vec4 kPlayerColour{0.95F, 0.3F, 0.15F, 1.0F};
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

float approach(float value, float target, float rate)
{
    return value < target ? std::min(value + rate, target) : std::max(value - rate, target);
}

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

bool secondTicked(float time, float dt)
{
    return static_cast<int>(time) != static_cast<int>(time - dt);
}

} // namespace

Game::Game(Window& window, const GameOptions& options)
    : m_window(window)
    , m_options(options)
{
    m_vfs.mountDisc(options.gameDir);
    m_database = std::make_unique<Database>(m_vfs.readOrThrow("media/db/gamedb.slt"));
    m_strings = std::make_unique<Localisation>(m_vfs, "en");
    m_jobs = std::make_unique<JobPool>();
    m_frame = std::make_unique<render::FrameRenderer>();
    m_overlay = std::make_unique<render::Overlay>("sans-serif:bold", 26);

    try {
        m_audio = std::make_unique<audio::AudioDevice>();
        m_audioLibrary = std::make_shared<audio::AudioLibrary>(m_vfs);
    } catch (const std::exception& e) {
        logWarning("audio: no sound ({})", e.what());
        m_audio.reset();
    }

    const int carId = carIdByMediaName(*m_database, options.car);
    if (carId < 0) {
        throw DataError("no car named {} in the game database", options.car);
    }
    m_vehicles.push_back(makeVehicle(carId, kPlayerColour));
    player().role = Vehicle::Role::Player;
    try {
        m_cameraSettings = CameraSettings::load(m_vfs);
        m_carCameras = CarCameras::load(*m_cameraSettings, *m_database, carId);
    } catch (const std::exception& e) {
        logWarning("camera: {}", e.what());
    }
    attachEngineSound(player(), false);
    player().physics->assists().automaticGearbox = !options.manualGearbox;
    logInfo("car: {} ({:.0f} kg, {:.0f} N·m)", player().name, player().spec->massKg,
        player().spec->reference.peakTorqueNm);
    loadWorld();
}

Game::~Game()
{
    // Both wait out their running jobs on the pool, which must outlive them;
    // the world the jobs read is a member destroyed after this body.
    m_worldRenderer.reset();
    m_collision.reset();
    m_jobs.reset();
}

std::unique_ptr<Game::Vehicle> Game::makeVehicle(int carId, const glm::vec4& colour, int colourSequence) const
{
    auto vehicle = std::make_unique<Vehicle>();
    vehicle->spec = std::make_unique<CarSpec>(loadCarSpec(*m_database, carId, &m_vfs));
    vehicle->physics = std::make_unique<CarPhysics>(*vehicle->spec);
    vehicle->colour = colour;
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
            const carmodel::CarModel model = carmodel::CarModel::load(m_vfs, m_database.get(), spec.mediaName);
            vehicle->model = std::make_shared<render::CarRenderer>(model);
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

    // Spheres along the body stand in for the car's collision shape: a low
    // row at bumper height and a row near the roof, so both walls and
    // rollovers are caught.
    const glm::vec3 lo(spec.boundsMin[0], spec.boundsMin[1], spec.boundsMin[2]);
    const glm::vec3 hi(spec.boundsMax[0], spec.boundsMax[1], spec.boundsMax[2]);
    const float halfWidth = (hi.x - lo.x) * 0.5F;
    const float lowRadius = std::min(halfWidth, 0.45F);
    const float lowY = std::max(lo.y, 0.0F) + std::max(spec.front.rideHeight, 0.1F) + lowRadius;
    const float roofRadius = std::min(halfWidth * 0.8F, 0.4F);
    const float roofY = hi.y - roofRadius;
    constexpr int kSteps = 4;
    for (int i = 0; i < kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps - 1);
        const float z = (lo.z + lowRadius) + t * ((hi.z - lowRadius) - (lo.z + lowRadius));
        const float x = std::max(0.0F, halfWidth - lowRadius);
        vehicle->spheres.push_back({{-x, lowY, z}, lowRadius});
        vehicle->spheres.push_back({{x, lowY, z}, lowRadius});
    }
    for (float z : {lo.z * 0.35F, hi.z * 0.1F}) {
        vehicle->spheres.push_back({{0.0F, roofY, z}, roofRadius});
    }
    return vehicle;
}

void Game::drawLoading(const std::string& message)
{
    m_window.pumpEvents();
    const int w = m_window.width();
    const int h = m_window.height();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glClearColor(0.04F, 0.04F, 0.05F, 1.0F);
    glClear(GL_COLOR_BUFFER_BIT);
    const float scale = 1.2F;
    const float width = m_overlay->textWidth(message, scale);
    m_overlay->text((static_cast<float>(w) - width) * 0.5F, static_cast<float>(h) * 0.5F, message, kHudText, scale);
    m_overlay->flush(glm::mat4(1.0F), w, h);
    m_window.swapBuffers();
}

void Game::loadWorld()
{
    drawLoading(std::format("Loading {}", m_options.track));
    const auto start = std::chrono::steady_clock::now();
    m_world = world::TrackWorld::load(m_vfs, m_options.track);
    if (!m_world) {
        throw DataError("track {} could not be loaded", m_options.track);
    }
    for (const std::string& warning : m_world->warnings) {
        logWarning("world: {}", warning);
    }
    logInfo("world: {} chunks indexed in {:.1f} s", m_world->index.chunks().size(),
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    m_worldRenderer = std::make_unique<render::WorldRenderer>(*m_world, *m_jobs);
    m_collision = std::make_unique<physics::WorldCollision>(*m_world, *m_jobs, m_vfs, m_options.track);
    try {
        m_smashables = Smashables::load(m_vfs, m_options.track, m_world->index);
        m_collision->setExcludedChunks(m_smashables->chunkMask());
    } catch (const std::exception& e) {
        logWarning("smashables: {}", e.what());
    }
    m_hiddenChunks.assign(m_world->index.chunks().size(), false);

    m_objects = GameObjects::load(m_vfs, m_options.track);
    m_timeOfDay.emplace(TimeOfDayCurves::load(m_vfs, m_options.track));
    if (m_options.startTime) {
        m_timeOfDay->setSeconds(*m_options.startTime);
    }
    const PostEffectSettings post = PostEffectSettings::load(m_vfs, m_options.track);
    m_tone.shoulderStrength = post.shoulderStrength;
    m_tone.linearStrength = post.linearStrength;
    m_tone.linearAngle = post.linearAngle;
    m_tone.toeStrength = post.toeStrength;
    m_tone.toeNumerator = post.toeNumerator;
    m_tone.toeDenominator = post.toeDenominator;
    // The game's white point of 1 assumes its own exposure scheme, which is
    // not decoded yet; with the automatic exposure here a white point of 1
    // clips every highlight, so the curve's usual value stands in.
    m_tone.white = std::max(post.white, 6.0F);
    m_tone.vignettePower = post.vignettePower;

    m_events = loadRaceEvents(*m_database, *m_strings, m_options.track);
    loadTraffic();
    for (std::size_t i = 0; i < m_events.size(); ++i) {
        if (const GameObject* object = m_objects->find(m_events[i].horizonId)) {
            m_eventStarts.push_back({i, object->position});
        }
    }
    logInfo("races: {} of {} can be started from free roam", m_eventStarts.size(), m_events.size());

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

void Game::spawnPlayer()
{
    placeVehicle(player(), m_spawnPosition, m_spawnHeading);
    m_playerPlaced = true;
    m_chaseVelocity = glm::vec3(0.0F);
    logInfo("car placed at height {:.1f}", player().physics->position().y);
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

CarControls Game::readPlayerControls(float dt)
{
    const InputState& in = m_window.input();
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
    const bool returning = keySteer == 0.0F || keySteer * m_keyboardSteer < 0.0F;
    m_keyboardSteer = approach(m_keyboardSteer, keySteer, (returning ? kKeyReturnRate : kKeySteerRate) * dt);

    controls.throttle = std::max(keyThrottle, in.axis(Axis::RightTrigger));
    controls.brake = std::max(keyBrake, in.axis(Axis::LeftTrigger));
    const float stick = in.axis(Axis::LeftX);
    controls.steer = std::abs(stick) > std::abs(m_keyboardSteer) ? stick : m_keyboardSteer;
    controls.handbrake = (in.held(Key::Space) || in.held(Button::A)) ? 1.0F : 0.0F;
    controls.shiftUp = in.pressed(Key::E) || in.pressed(Button::B);
    controls.shiftDown = in.pressed(Key::Q) || in.pressed(Button::X);
    if (m_options.demoDrive) {
        controls.throttle = 1.0F;
        controls.steer = 0.15F * std::sin(m_time * 0.4F);
    }
    return controls;
}

void Game::stepPhysics(float dt)
{
    constexpr float kStep = 1.0F / CarPhysics::kStepRate;
    m_physicsAccumulator = std::min(m_physicsAccumulator + dt, 0.1F);
    std::vector<CarControls> controls;
    controls.reserve(m_vehicles.size());
    for (const auto& vehicle : m_vehicles) {
        controls.push_back(vehicle->controls);
    }
    while (m_physicsAccumulator >= kStep) {
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
    }
}

void Game::collideWithWorld(Vehicle& vehicle)
{
    const glm::mat4 transform = vehicle.physics->transform();
    for (const BodySphere& sphere : vehicle.spheres) {
        const glm::vec3 centre = glm::vec3(transform * glm::vec4(sphere.centre, 1.0F));
        m_contacts.clear();
        m_collision->sphereContacts(centre, sphere.radius, m_contacts);
        // The deepest few are enough; a sphere against a wall touches many
        // coplanar triangles.
        const std::size_t count = std::min<std::size_t>(m_contacts.size(), 3);
        for (std::size_t i = 0; i < count; ++i) {
            const physics::SphereContact& contact = m_contacts[i];
            const float impulse
                = vehicle.physics->resolveContact(contact.point, contact.normal, contact.depth, 0.2F, 0.4F);
            if (impulse > vehicle.physics->mass() * 3.0F) {
                logDebug("impact {:.0f} N·s at {:.1f} {:.1f} {:.1f}, surface {}, normal {:.2f} {:.2f} {:.2f}", impulse,
                    contact.point.x, contact.point.y, contact.point.z, contact.surface, contact.normal.x,
                    contact.normal.y, contact.normal.z);
            }
        }
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

void Game::attachEngineSound(Vehicle& vehicle, bool spatial) const
{
    if (!m_audio) {
        return;
    }
    audio::EngineSoundOptions options;
    options.spatial = spatial;
    // Opponents' gear changes and skids would crowd the mix; the player's
    // own carry the feedback.
    options.transmission = !spatial;
    try {
        vehicle.engine
            = std::make_unique<audio::EngineSound>(*m_audio, m_audioLibrary, vehicle.spec->mediaName, options);
    } catch (const std::exception& e) {
        logWarning("audio: {}: {}", vehicle.spec->mediaName, e.what());
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
        float slip = 0.0F;
        for (const CarPhysics::WheelState& wheel : car.wheels()) {
            if (wheel.onGround) {
                slip = std::max(slip, wheel.slipMagnitude);
            }
        }
        vehicle->engine->setTyreSlip(slip);
        vehicle->engine->setTransform(car.position(), glm::mat3_cast(car.orientation()));
        vehicle->engine->update(car.engineRpm(), car.throttleApplied(), car.speed(), dt);
    }
}

void Game::collideVehicles()
{
    for (std::size_t a = 0; a < m_vehicles.size(); ++a) {
        for (std::size_t b = a + 1; b < m_vehicles.size(); ++b) {
            CarPhysics& carA = *m_vehicles[a]->physics;
            CarPhysics& carB = *m_vehicles[b]->physics;
            if (glm::length(carA.position() - carB.position()) > 8.0F) {
                continue;
            }
            const glm::mat4 ta = carA.transform();
            const glm::mat4 tb = carB.transform();
            for (const BodySphere& sa : m_vehicles[a]->spheres) {
                const glm::vec3 ca = glm::vec3(ta * glm::vec4(sa.centre, 1.0F));
                for (const BodySphere& sb : m_vehicles[b]->spheres) {
                    const glm::vec3 cb = glm::vec3(tb * glm::vec4(sb.centre, 1.0F));
                    const glm::vec3 offset = ca - cb;
                    const float distance = glm::length(offset);
                    const float reach = sa.radius + sb.radius;
                    if (distance >= reach || distance < 1e-4F) {
                        continue;
                    }
                    const glm::vec3 normal = offset / distance;
                    const glm::vec3 point = cb + normal * sb.radius;
                    const float approachSpeed
                        = glm::dot(carA.worldPointVelocity(point) - carB.worldPointVelocity(point), normal);
                    if (approachSpeed < 0.0F) {
                        const float response
                            = carA.impulseResponse(point, normal) + carB.impulseResponse(point, normal);
                        const float impulse = -1.1F * approachSpeed / response;
                        carA.applyImpulse(normal * impulse, point);
                        carB.applyImpulse(-normal * impulse, point);
                    }
                    const float depth = reach - distance;
                    carA.translate(normal * (depth * 0.3F));
                    carB.translate(-normal * (depth * 0.3F));
                }
            }
        }
    }
}

void Game::startRace(const RaceEvent& event)
{
    endRace();
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
    // The race's own barriers close off the side roads of its route.
    m_collision->setActiveRoute(event.routeId);
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
        // Opponents are told apart by hue, all within the HUD's cool range
        // so none is confused with the player's orange.
        const float hue = static_cast<float>(opponent) / 7.0F;
        const glm::vec4 colour(0.35F + 0.3F * std::sin(hue * 6.28F), 0.6F + 0.3F * std::cos(hue * 6.28F), 0.95F, 1.0F);
        try {
            const std::size_t o = static_cast<std::size_t>(opponent);
            std::unique_ptr<Vehicle> ai
                = makeVehicle(carId, colour, o < event.opponentColours.size() ? event.opponentColours[o] : 1);
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
    m_lineGuide = std::make_unique<AiDriver>(m_race->line(), *player().spec, 100, 0.0F);
    if (m_options.autopilot) {
        player().ai = std::make_unique<AiDriver>(m_race->line(), *player().spec, 90, 0.0F);
    }
    m_timeOfDay->setSeconds(event.timeOfDay);
    m_resultsTime = 0.0F;
    m_chaseVelocity = glm::vec3(0.0F);
    m_camera.position
        = player().physics->position() - player().physics->forward() * 8.0F + glm::vec3(0.0F, 3.0F, 0.0F);
    logInfo("race {} \"{}\": {} laps, {} racers, {:.0f} m, line {} points{}", event.horizonId, event.name, event.laps,
        m_race->racers().size(), m_race->distance(), m_race->line().points.size(),
        m_race->line().loop ? " (loop)" : "");
}

void Game::endRace()
{
    if (!m_race) {
        return;
    }
    m_vehicles.resize(1);
    m_collision->setActiveRoute(-1);
    player().ai.reset();
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
        const InputState& in = m_window.input();
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
        try {
            const int colour = std::uniform_int_distribution<int>(1, 6)(m_random);
            std::unique_ptr<Vehicle> vehicle = makeVehicle(carId, {0.7F, 0.7F, 0.7F, 1.0F}, colour);
            vehicle->role = role;
            vehicle->line = &line;
            vehicle->physics->assists().speedSensitiveSteering = false;
            // Keep right of the line, as the roads are driven in Colorado.
            const float lane = role == Vehicle::Role::Traffic ? 2.0F : 1.2F;
            vehicle->ai = std::make_unique<AiDriver>(line, *vehicle->spec, role == Vehicle::Role::Traffic ? 40 : 65, lane);
            vehicle->ai->setLineIndex(index);
            float speed = density.speedMph * kMphToMs;
            if (speed <= 0.0F) {
                speed = kFestivalDriverSpeed;
            }
            vehicle->ai->setMaxSpeed(speed);
            const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), direction));
            placeVehicle(*vehicle, point + side * lane, std::atan2(direction.x, direction.z));
            // Already moving, as cars on a road are when they come into view.
            vehicle->physics->setState(vehicle->physics->position(), vehicle->physics->orientation(),
                direction * speed * 0.8F, glm::vec3(0.0F));
            attachEngineSound(*vehicle, true);
            logDebug("traffic: {} {} at {:.0f} m, {:.0f} km/h", role == Vehicle::Role::Traffic ? "car" : "festival driver",
                vehicle->name, distance, speed * 3.6F);
            m_vehicles.push_back(std::move(vehicle));
            return true;
        } catch (const std::exception& e) {
            logWarning("traffic: car {}: {}", carId, e.what());
            return false;
        }
    }
    return false;
}

void Game::updateTraffic(float dt)
{
    if (!m_trafficConfig || m_trafficLines.empty() || !m_playerPlaced) {
        return;
    }
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

    m_trafficTimer -= dt;
    if (m_trafficTimer > 0.0F) {
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

void Game::updateEventStarts()
{
    m_nearbyStart.reset();
    if (m_race || !m_playerPlaced) {
        return;
    }
    // As in the game, a race is offered while the car sits in its start
    // circle; it starts on the confirm button.
    constexpr float kStartRadius = 12.0F;
    const CarPhysics& car = *player().physics;
    const glm::vec3 position = car.position();
    for (std::size_t i = 0; i < m_eventStarts.size(); ++i) {
        const glm::vec3 offset = m_eventStarts[i].position - position;
        if (glm::length(glm::vec2(offset.x, offset.z)) < kStartRadius && std::abs(offset.y) < 6.0F) {
            m_nearbyStart = i;
            break;
        }
    }
    const InputState& in = m_window.input();
    if (m_nearbyStart && car.speed() < 4.0F && (in.pressed(Key::Enter) || in.pressed(Button::A))) {
        startRace(m_events[m_eventStarts[*m_nearbyStart].event]);
    }
}

void Game::drawEventStarts(const glm::mat4& viewProjection)
{
    m_labels.clear();
    if (m_race || !m_playerPlaced) {
        return;
    }
    constexpr float kBeaconDistance = 1500.0F;
    constexpr float kLabelDistance = 250.0F;
    const glm::vec3 eye = m_camera.position;
    const float width = static_cast<float>(m_window.width());
    const float height = static_cast<float>(m_window.height());
    for (std::size_t i = 0; i < m_eventStarts.size(); ++i) {
        const glm::vec3 base = m_eventStarts[i].position;
        const float distance = glm::length(base - eye);
        if (distance > kBeaconDistance) {
            continue;
        }
        // A tall beam marks the start from afar and a ring on the ground
        // marks where to stop.
        const bool here = m_nearbyStart == i;
        const glm::vec4 colour = here ? glm::vec4(1.0F, 0.55F, 0.2F, 1.0F) : glm::vec4(1.0F, 0.8F, 0.35F, 0.9F);
        for (float dx : {-0.4F, 0.0F, 0.4F}) {
            m_overlay->line(base + glm::vec3(dx, 0.0F, 0.0F), base + glm::vec3(dx, 60.0F, 0.0F), colour);
        }
        m_overlay->circle(base + glm::vec3(0.0F, 0.2F, 0.0F), {1.0F, 0.0F, 0.0F}, {0.0F, 0.0F, 1.0F}, 12.0F, colour, 48);
        if (distance > kLabelDistance) {
            continue;
        }
        const glm::vec4 clip = viewProjection * glm::vec4(base + glm::vec3(0.0F, 4.0F, 0.0F), 1.0F);
        if (clip.w <= 0.1F) {
            continue;
        }
        const glm::vec2 ndc = glm::vec2(clip) / clip.w;
        if (std::abs(ndc.x) > 1.1F || std::abs(ndc.y) > 1.1F) {
            continue;
        }
        const RaceEvent& event = m_events[m_eventStarts[i].event];
        m_labels.push_back({{(ndc.x * 0.5F + 0.5F) * width, (0.5F - ndc.y * 0.5F) * height}, event.name,
            std::clamp(1.0F - (distance - 150.0F) / 100.0F, 0.0F, 1.0F)});
    }
}

void Game::updateCamera(float dt)
{
    const InputState& in = m_window.input();
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
        return;
    }

    const CarPhysics& car = *player().physics;
    const CarSpec& spec = *player().spec;
    const glm::mat4 transform = car.transform();
    const glm::vec3 carPosition = car.position();
    const glm::vec3 forward = car.forward();

    // Looking around: the right stick, or the mouse while its right button
    // is held, turns the view; letting go eases it back. Clicking the stick
    // (or holding V) looks behind.
    const bool lookBack = in.held(Button::RightStick) || in.held(Key::V);
    float stickYaw = in.axis(Axis::RightX) * kPi * 0.75F;
    float stickPitch = in.axis(Axis::RightY) * 0.5F;
    if (in.mouseRight) {
        m_window.setMouseCaptured(true);
        m_lookYaw = std::clamp(m_lookYaw + in.mouseDx * 0.004F, -kPi, kPi);
        m_lookPitch = std::clamp(m_lookPitch - in.mouseDy * 0.004F, -0.6F, 0.6F);
    } else {
        if (m_window.mouseCaptured() && m_cameraMode != CameraMode::Free) {
            m_window.setMouseCaptured(false);
        }
        const float ease = std::min(1.0F, dt * 8.0F);
        m_lookYaw += ((lookBack ? kPi : stickYaw) - m_lookYaw) * ease;
        m_lookPitch += ((lookBack ? 0.0F : stickPitch) - m_lookPitch) * ease;
    }
    const float boundsCentreY = 0.5F * (spec.boundsMin[1] + spec.boundsMax[1]);
    if (m_cameraMode == CameraMode::Hood || m_cameraMode == CameraMode::Bumper) {
        const bool hood = m_cameraMode == CameraMode::Hood;
        // Hood: the car's CameraOverrides height above the middle of its
        // body (the base is inferred), a quarter of the way along the bonnet.
        // Bumper: the ini's rest height above the ground at the front bumper.
        const glm::vec3 eye = hood
            ? glm::vec3(0.0F, boundsCentreY + m_carCameras.hoodHeight,
                  spec.boundsMax[2] * 0.25F + m_carCameras.hoodZOffset)
            : glm::vec3(0.0F, m_carCameras.bumperHeight, spec.boundsMax[2] + m_carCameras.bumperZOffset);
        m_camera.position = glm::vec3(transform * glm::vec4(eye, 1.0F));
        const float pitch = glm::radians(hood ? m_carCameras.hoodPitchDegrees : 0.0F) - m_lookPitch;
        const glm::vec3 local(std::sin(m_lookYaw) * std::cos(pitch), -std::sin(pitch), std::cos(m_lookYaw) * std::cos(pitch));
        const glm::vec3 look = glm::vec3(transform * glm::vec4(local, 0.0F));
        m_camera.target = m_camera.position + look * 10.0F;
        m_camera.up = car.up();
        m_camera.fov = glm::radians(hood ? m_carCameras.hoodFovDegrees : m_carCameras.bumperFovDegrees);
        return;
    }

    const FollowCamera& cam = m_cameraMode == CameraMode::Far ? m_carCameras.far : m_carCameras.near;
    // The yaw spring brings the camera round to where the car is heading
    // (its velocity when it slides forward, else where it points), no
    // faster than the ini's limit.
    float targetYaw = std::atan2(forward.x, forward.z);
    const glm::vec3 velocity = car.velocity();
    if (glm::length(glm::vec2(velocity.x, velocity.z)) > 3.0F && glm::dot(velocity, forward) > 0.0F) {
        targetYaw = std::atan2(velocity.x, velocity.z);
    }
    float error = targetYaw - m_chaseYaw;
    error = std::remainder(error, 2.0F * kPi);
    m_chaseYawSpeed += (cam.yawSpringK * error - cam.yawSpringD * m_chaseYawSpeed) * dt;
    m_chaseYawSpeed = std::clamp(m_chaseYawSpeed, -cam.yawMaxSpeed, cam.yawMaxSpeed);
    // Well behind after a reset or a spin: snap rather than swing through.
    if (std::abs(error) > 2.5F) {
        m_chaseYaw = targetYaw;
        m_chaseYawSpeed = 0.0F;
    }
    m_chaseYaw = std::remainder(m_chaseYaw + m_chaseYawSpeed * dt, 2.0F * kPi);

    // The ini gives the height above the ground but not the distance
    // behind, which the game derives from the car; a car length and a bit
    // (more for the far camera) matches its framing.
    const float length = spec.boundsMax[2] - spec.boundsMin[2];
    const float radius = length * (m_cameraMode == CameraMode::Far ? 1.55F : 1.2F) + cam.radiusOffset;
    const float orbit = m_chaseYaw + m_lookYaw;
    const glm::vec3 behind(std::sin(orbit), 0.0F, std::cos(orbit));
    glm::vec3 eye = carPosition - behind * radius;
    const std::optional<float> ground = m_collision->groundHeight(eye.x, eye.z, carPosition.y + 10.0F);
    const float wantedY = std::max(ground.value_or(carPosition.y), carPosition.y) + cam.height + cam.heightOffset;
    float y = m_camera.position.y;
    if (std::abs(y - wantedY) > 10.0F) {
        y = wantedY;
        m_chaseHeightSpeed = 0.0F;
    }
    m_chaseHeightSpeed += (cam.heightSpringK * (wantedY - y) - cam.heightSpringD * m_chaseHeightSpeed) * dt;
    eye.y = y + m_chaseHeightSpeed * dt + m_lookPitch * radius * 0.5F;
    m_camera.position = eye;
    m_camera.target = carPosition + glm::vec3(0.0F, spec.boundsMax[1] * 0.7F + cam.targetYOffset, 0.0F);
    m_camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
    m_camera.fov = glm::radians(cam.fovDegrees);
}

render::FrameParams Game::frameParams() const
{
    render::FrameParams frame;
    const float aspect = static_cast<float>(std::max(m_window.width(), 1))
        / static_cast<float>(std::max(m_window.height(), 1));
    frame.view = m_camera.view();
    frame.projection = m_camera.projection(aspect);
    frame.cameraPosition = m_camera.position;
    const TimeOfDay& tod = *m_timeOfDay;
    frame.sunDirection = tod.sunDirection();
    // The curves are display-referred colours; the renderer's sun needs to
    // outshine the sky several times over for sunlit and shaded surfaces to
    // separate as they do in the game.
    frame.sunColour = tod.sunColour() * 4.0F;
    frame.skyColour = tod.skyTopColour();
    frame.horizonColour = tod.skyBottomColour() * 0.5F;
    frame.groundColour = tod.skyBottomColour() * 0.08F;
    frame.fogColour = glm::mix(tod.skyBottomColour() * 0.5F, tod.hazeColour() * 2.0F, 0.5F);
    frame.fogDensity = 0.00004F + 0.00012F * tod.hazeIntensity();
    frame.time = m_time;
    return frame;
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
    }
    pose.brakeLights = vehicle.controls.brake > 0.1F ? 1.0F : 0.0F;
    pose.headLights = m_timeOfDay->lights();
    pose.paint = vehicle.paint;
    pose.metallic = vehicle.metallic;
    return pose;
}

void Game::drawVehicles(const render::FrameParams& frame)
{
    if (!m_playerPlaced) {
        return;
    }
    for (const auto& vehicle : m_vehicles) {
        if (vehicle->model) {
            vehicle->model->draw(frame, carPose(*vehicle), false);
            continue;
        }
        // Outline of the body and wheels for a car whose model is missing.
        const CarPhysics& car = *vehicle->physics;
        const CarSpec& spec = *vehicle->spec;
        const glm::mat4 transform = car.transform();
        const glm::vec3 lo(spec.boundsMin[0], spec.boundsMin[1], spec.boundsMin[2]);
        const glm::vec3 hi(spec.boundsMax[0], spec.boundsMax[1], spec.boundsMax[2]);
        const glm::vec3 lift(0.0F, std::max(spec.front.rideHeight, 0.1F), 0.0F);
        const glm::vec3 centre = (lo + hi) * 0.5F + lift * 0.5F;
        const glm::vec3 half = (hi - lo) * 0.5F - glm::vec3(0.0F, lift.y * 0.5F, 0.0F);
        m_overlay->box(transform * glm::translate(glm::mat4(1.0F), centre), half, vehicle->colour);
        const glm::vec3 up = car.up();
        for (int w = 0; w < CarPhysics::kWheels; ++w) {
            const CarPhysics::WheelState& wheel = car.wheels()[static_cast<std::size_t>(w)];
            const glm::quat steer
                = car.orientation() * glm::angleAxis(wheel.steerAngle, glm::vec3(0.0F, 1.0F, 0.0F));
            const glm::vec3 forward = steer * glm::vec3(0.0F, 0.0F, 1.0F);
            const glm::vec4 colour(0.9F, 0.9F, 0.9F, 1.0F);
            const float radius = car.wheelRadius(w);
            m_overlay->circle(wheel.hubWorld, forward, up, radius, colour);
            const glm::vec3 spoke = forward * std::cos(wheel.spinAngle) - up * std::sin(wheel.spinAngle);
            m_overlay->line(wheel.hubWorld, wheel.hubWorld + spoke * radius, colour);
        }
    }
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

void Game::drawDrivingLine()
{
    if (!m_race || !m_lineGuide) {
        return;
    }
    const Race::Racer* racer = m_race->player();
    if (!racer) {
        return;
    }
    // The line ahead, lifted off the road: green where the player's speed
    // can be carried, amber where it needs easing, red where it needs the
    // brakes, as the game's driving line assist shows it.
    const RacingLine& line = m_race->line();
    const float speed = std::abs(player().physics->forwardSpeed());
    const float start = racer->lineIndex;
    const float end = line.advance(start, 180.0F);
    glm::vec3 previousLeft(0.0F);
    glm::vec3 previousRight(0.0F);
    bool first = true;
    for (float index = start + 1.0F; index < end; index += 1.0F) {
        if (!line.loop && index >= static_cast<float>(line.points.size()) - 1.0F) {
            break;
        }
        const glm::vec3 p = line.positionAt(index) + glm::vec3(0.0F, 0.15F, 0.0F);
        const glm::vec3 d = line.directionAt(index);
        const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), d)) * 0.35F;
        const float excess = speed - m_lineGuide->speedLimitAt(index);
        glm::vec4 colour(0.2F, 0.9F, 0.3F, 0.9F);
        if (excess > 3.0F) {
            colour = glm::vec4(0.95F, 0.15F, 0.1F, 0.9F);
        } else if (excess > -3.0F) {
            colour = glm::vec4(1.0F, 0.75F, 0.1F, 0.9F);
        }
        const glm::vec3 left = p - side;
        const glm::vec3 right = p + side;
        if (!first) {
            m_overlay->line(previousLeft, left, colour);
            m_overlay->line(previousRight, right, colour);
            m_overlay->line(left, right, colour);
        }
        previousLeft = left;
        previousRight = right;
        first = false;
    }
    // The finish gate, as two posts and a bar.
    if (const auto& finish = m_race->route().finish) {
        const glm::vec3 side = glm::normalize(glm::cross(glm::vec3(0.0F, 1.0F, 0.0F), finish->facing))
            * std::max(finish->width * 0.5F, 6.0F);
        const glm::vec3 base = finish->position;
        const glm::vec3 up(0.0F, 5.0F, 0.0F);
        const glm::vec4 colour(1.0F, 1.0F, 1.0F, 1.0F);
        m_overlay->line(base - side, base - side + up, colour);
        m_overlay->line(base + side, base + side + up, colour);
        m_overlay->line(base - side + up, base + side + up, colour);
    }
}

void Game::drawRaceHud()
{
    if (!m_race) {
        return;
    }
    const Race::Racer* own = m_race->player();
    if (!own) {
        return;
    }
    const auto w = static_cast<float>(m_window.width());
    const auto h = static_cast<float>(m_window.height());
    const int racers = static_cast<int>(m_race->racers().size());

    // Top left: place, lap and time.
    const std::string place = std::to_string(m_race->place(*own));
    m_overlay->rect(24.0F, 24.0F, 300.0F, 132.0F, kHudPanel);
    m_overlay->text(40.0F, 30.0F, place, kHudText, 2.2F);
    m_overlay->text(48.0F + m_overlay->textWidth(place, 2.2F), 64.0F, std::format("/ {}", racers), kHudDim, 0.9F);
    if (m_race->line().loop) {
        m_overlay->text(
            170.0F, 40.0F, std::format("Lap {}/{}", m_race->lap(*own), m_race->event().laps), kHudText, 0.9F);
    }
    m_overlay->text(170.0F, 76.0F, formatTime(m_race->elapsed()), kHudText, 0.9F);
    const float remaining = std::max(m_race->distance() - own->progress, 0.0F);
    m_overlay->text(40.0F, 118.0F, std::format("{:.1f} km to go", remaining / 1000.0F), kHudDim, 0.75F);

    // Countdown in the middle.
    const float scale = 4.0F;
    if (m_race->phase() == Race::Phase::Countdown) {
        const std::string text = std::to_string(std::max(static_cast<int>(std::ceil(m_race->countdown())), 1));
        m_overlay->text((w - m_overlay->textWidth(text, scale)) * 0.5F, h * 0.3F, text, kHudText, scale);
    } else if (m_race->elapsed() < 1.0F && m_race->phase() == Race::Phase::Racing) {
        m_overlay->text((w - m_overlay->textWidth("GO", scale)) * 0.5F, h * 0.3F, "GO", kHudAccent, scale);
    }

    // Standings during the race, results once the player has finished.
    const std::vector<const Race::Racer*> order = m_race->standings();
    const bool results = m_race->phase() == Race::Phase::Finished;
    const float rowHeight = m_overlay->lineHeight(0.7F) + 2.0F;
    const float panelW = results ? 560.0F : 300.0F;
    const float x0 = results ? (w - panelW) * 0.5F : 24.0F;
    const float y0 = results ? h * 0.25F : 170.0F;
    const float titleHeight = results ? m_overlay->lineHeight(1.0F) + 12.0F : 0.0F;
    m_overlay->rect(x0, y0, panelW, titleHeight + rowHeight * static_cast<float>(order.size()) + 16.0F, kHudPanel);
    float y = y0 + 8.0F;
    if (results) {
        m_overlay->text(x0 + 16.0F, y, std::format("{} place", ordinal(own->finishPlace)), kHudAccent, 1.0F);
        y += titleHeight;
    }
    for (std::size_t i = 0; i < order.size(); ++i) {
        const Race::Racer& racer = *order[i];
        const Vehicle& vehicle = *m_vehicles[racer.vehicle];
        std::string line = std::format("{}  {}", i + 1, vehicle.name);
        if (results && racer.finished) {
            line += std::format("   {}", formatTime(racer.finishTime));
        }
        m_overlay->text(x0 + 16.0F, y, line, racer.player ? kHudAccent : kHudText, 0.7F);
        y += rowHeight;
    }
}

void Game::drawHud(float dt)
{
    const auto w = static_cast<float>(m_window.width());
    const auto h = static_cast<float>(m_window.height());
    m_fps = m_fps * 0.95F + (dt > 0.0F ? 1.0F / dt : 0.0F) * 0.05F;

    if (m_playerPlaced) {
        const CarPhysics& car = *player().physics;
        const float speed = std::abs(car.forwardSpeed()) * kMsToKmh;
        const int gear = car.gear();
        const std::string gearText = gear < 0 ? "R" : gear == 0 ? "N" : std::to_string(gear);
        const float rpm = car.engineRpm();
        const float redline = player().spec->redlineRpm;

        const float panelW = 300.0F;
        const float panelH = 128.0F;
        const float x0 = w - panelW - 32.0F;
        const float y0 = h - panelH - 32.0F;
        m_overlay->rect(x0, y0, panelW, panelH, kHudPanel);
        const std::string speedText = std::format("{:.0f}", speed);
        const float big = 2.4F;
        m_overlay->text(x0 + 20.0F, y0 + 10.0F, speedText, kHudText, big);
        m_overlay->text(x0 + 24.0F + m_overlay->textWidth(speedText, big), y0 + 46.0F, "km/h", kHudDim, 0.8F);
        const float gearWidth = m_overlay->textWidth(gearText, big);
        m_overlay->text(x0 + panelW - gearWidth - 20.0F, y0 + 10.0F, gearText, kHudText, big);

        // Rev bar: the last tenth before the redline takes the accent.
        const float barX = x0 + 20.0F;
        const float barY = y0 + panelH - 34.0F;
        const float barW = panelW - 40.0F;
        const float fraction = std::clamp(rpm / (redline * 1.05F), 0.0F, 1.0F);
        m_overlay->rect(barX, barY, barW, 12.0F, {1.0F, 1.0F, 1.0F, 0.12F});
        m_overlay->rect(barX, barY, barW * fraction, 12.0F, rpm > redline * 0.9F ? kHudAccent : kHudText);
        m_overlay->text(barX, barY - 26.0F, std::format("{:.0f} rpm", rpm), kHudDim, 0.7F);
    }
    drawRaceHud();

    for (const ScreenLabel& label : m_labels) {
        const float labelWidth = m_overlay->textWidth(label.text, 0.75F);
        const glm::vec2 at = label.position - glm::vec2(labelWidth * 0.5F, m_overlay->lineHeight(0.75F));
        m_overlay->rect(at.x - 10.0F, at.y - 4.0F, labelWidth + 20.0F, m_overlay->lineHeight(0.75F) + 8.0F,
            {kHudPanel.r, kHudPanel.g, kHudPanel.b, kHudPanel.a * label.alpha});
        m_overlay->text(at.x, at.y, label.text, {kHudText.r, kHudText.g, kHudText.b, label.alpha}, 0.75F);
    }
    if (m_nearbyStart) {
        const RaceEvent& event = m_events[m_eventStarts[*m_nearbyStart].event];
        const std::string title = event.name;
        const std::string detail = std::format("{}  ·  {} lap{}  ·  {:.1f} km", event.type, event.laps,
            event.laps == 1 ? "" : "s", event.lengthMetres / 1000.0F);
        const std::string action = player().physics->speed() < 4.0F ? "Enter or A to start" : "Stop to start the race";
        const float panelW = std::max({m_overlay->textWidth(title, 1.0F), m_overlay->textWidth(detail, 0.7F),
                                 m_overlay->textWidth(action, 0.7F)})
            + 48.0F;
        const float x0 = (w - panelW) * 0.5F;
        const float y0 = h * 0.62F;
        m_overlay->rect(x0, y0, panelW, 118.0F, kHudPanel);
        m_overlay->text(x0 + 24.0F, y0 + 12.0F, title, kHudText, 1.0F);
        m_overlay->text(x0 + 24.0F, y0 + 52.0F, detail, kHudDim, 0.7F);
        m_overlay->text(x0 + 24.0F, y0 + 82.0F, action, kHudAccent, 0.7F);
    }

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
    if (!m_playerPlaced) {
        const std::string message = "Loading the area around the start";
        const float width = m_overlay->textWidth(message, 1.0F);
        m_overlay->rect((w - width) * 0.5F - 20.0F, h * 0.5F - 12.0F, width + 40.0F, m_overlay->lineHeight() + 24.0F,
            kHudPanel);
        m_overlay->text((w - width) * 0.5F, h * 0.5F, message, kHudText);
    }
}

bool Game::saveScreenshot(const std::filesystem::path& path)
{
    const int w = m_window.width();
    const int h = m_window.height();
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

int Game::run()
{
    auto last = std::chrono::steady_clock::now();
    while (m_window.pumpEvents()) {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.1F);
        last = now;
        m_time += dt;
        const InputState& in = m_window.input();
        m_timeOfDay->advance(dt);

        if (in.pressed(Key::Escape)) {
            if (m_window.mouseCaptured()) {
                m_window.setMouseCaptured(false);
            } else if (m_race) {
                endRace();
            } else {
                break;
            }
        }
        if (in.pressed(Key::F11)) {
            m_window.toggleFullscreen();
        }
        if (in.pressed(Key::Tab) || in.pressed(Key::F3)) {
            m_showDebug = !m_showDebug;
        }
        // Back changes the view, as on the 360.
        if (in.pressed(Key::C) || in.pressed(Button::Back)) {
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
        if (in.pressed(Key::R) && m_playerPlaced) {
            if (m_race && m_race->player()) {
                // Back onto the line where the player left it.
                const float index = m_race->player()->lineIndex;
                const glm::vec3 direction = m_race->line().directionAt(index);
                placeVehicle(player(), m_race->line().positionAt(index), std::atan2(direction.x, direction.z));
            } else {
                player().physics->recover(*m_collision);
            }
        }
        updateEventStarts();

        const glm::vec3 focus = m_playerPlaced ? player().physics->position() : m_spawnPosition;
        m_collision->update(focus);
        if (!m_playerPlaced && m_collision->readyAt(m_spawnPosition)) {
            // Wait for the ground itself, but not forever: a spawn point off
            // the map has none.
            m_spawnWait += dt;
            if (m_collision->groundHeight(m_spawnPosition.x, m_spawnPosition.z, m_spawnPosition.y + 50.0F)
                || m_spawnWait > 10.0F) {
                spawnPlayer();
            }
        }
        if (m_playerPlaced) {
            const float playerScale = m_race && !m_race->started() ? 0.0F : 1.0F;
            player().controls = player().ai ? player().ai->update(*player().physics, dt, playerScale)
                                            : readPlayerControls(dt);
            updateRace(dt);
            updateAiDrivers(dt);
            updateTraffic(dt);
            stepPhysics(dt);
            updateDebris(dt);
            updateAudio(dt);
            if (m_options.demoDrive && secondTicked(m_time, dt)) {
                const CarPhysics& car = *player().physics;
                const GroundHit& ground = car.wheels()[0].ground;
                logInfo("demo: v {:.1f} m/s, rpm {:.0f}, gear {}, at {:.0f} {:.0f}, surface {} (grip {:.2f}, drag {:.3f})",
                    car.forwardSpeed(), car.engineRpm(), car.gear(), car.position().x, car.position().z,
                    ground.surface, ground.grip, ground.drag);
            }
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
        updateCamera(dt);
        m_worldRenderer->update(m_camera.position);

        const render::FrameParams frame = frameParams();
        m_frame->begin(m_window.width(), m_window.height());
        m_worldRenderer->draw(frame);
        drawVehicles(frame);
        m_frame->sky(frame);
        drawVehicleGlass(frame);
        drawEventStarts(frame.projection * frame.view);
        drawDrivingLine();
        m_overlay->flush(frame.projection * frame.view, m_window.width(), m_window.height());
        m_frame->finish(m_tone);
        drawHud(dt);
        m_overlay->flush(glm::mat4(1.0F), m_window.width(), m_window.height());

        ++m_frameCount;
        if (m_options.frames && m_frameCount >= *m_options.frames) {
            if (!m_options.screenshot.empty()) {
                saveScreenshot(m_options.screenshot);
            }
            m_window.swapBuffers();
            break;
        }
        m_window.swapBuffers();
    }
    return 0;
}

} // namespace fh
