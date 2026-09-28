#include "Game.h"

#include "base/Log.h"
#include "render/Gl.h"

#include <glm/gtc/matrix_transform.hpp>
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

    const int carId = carIdByMediaName(*m_database, options.car);
    if (carId < 0) {
        throw DataError("no car named {} in the game database", options.car);
    }
    m_vehicles.push_back(makeVehicle(carId, kPlayerColour));
    player().physics->assists().automaticGearbox = !options.manualGearbox;
    logInfo("car: {} ({:.0f} kg, {:.0f} N·m)", player().name, player().spec->massKg,
        player().spec->reference.peakTorqueNm);
    loadWorld();
}

Game::~Game()
{
    // Workers may still run jobs that reference the world; stop them first.
    m_worldRenderer.reset();
    m_collision.reset();
    m_jobs.reset();
}

std::unique_ptr<Game::Vehicle> Game::makeVehicle(int carId, const glm::vec4& colour) const
{
    auto vehicle = std::make_unique<Vehicle>();
    vehicle->spec = std::make_unique<CarSpec>(loadCarSpec(*m_database, carId, &m_vfs));
    vehicle->physics = std::make_unique<CarPhysics>(*vehicle->spec);
    vehicle->colour = colour;
    const CarSpec& spec = *vehicle->spec;
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
    m_collision = std::make_unique<physics::WorldCollision>(*m_world, *m_jobs);

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
    for (std::size_t i = 0; i < m_events.size(); ++i) {
        if (const GameObject* object = m_objects->find(m_events[i].horizonId)) {
            m_eventStarts.push_back({i, object->position});
        }
    }
    logInfo("races: {} of {} can be started from free roam", m_eventStarts.size(), m_events.size());

    if (m_options.spawnAt) {
        m_spawnPosition = glm::vec3(m_options.spawnAt->x, 0.0F, m_options.spawnAt->y);
        m_spawnHeading = m_options.spawnHeading.value_or(0.0F);
    } else if (const GameObject* object = m_objects->find(m_options.spawnObject)) {
        m_spawnPosition = object->position;
        m_spawnHeading = m_options.spawnHeading.value_or(object->heading());
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
    if (m_menuOpen) {
        controls.brake = 1.0F;
        return controls;
    }
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
            vehicle.physics->resolveContact(contact.point, contact.normal, contact.depth, 0.2F, 0.4F);
        }
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
            std::unique_ptr<Vehicle> ai = makeVehicle(carId, colour);
            ai->physics->assists().speedSensitiveSteering = false;
            // Alternate sides of the line, as a grid does, so the field does
            // not drive nose to tail.
            const float lane = (opponent % 2 == 0 ? -1.0F : 1.0F) * 1.2F;
            ai->ai = std::make_unique<AiDriver>(m_race->line(), *ai->spec, event.aiSkill, lane);
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

    // Opponents drive, and come back to the line when hopelessly stuck.
    const float speedScale = m_race->started() ? 1.0F : 0.0F;
    for (std::size_t i = 1; i < m_vehicles.size(); ++i) {
        Vehicle& vehicle = *m_vehicles[i];
        if (!vehicle.ai) {
            continue;
        }
        vehicle.controls = vehicle.ai->update(*vehicle.physics, dt, speedScale);
        if (vehicle.ai->needsRecovery()) {
            const float index = vehicle.ai->lineIndex();
            const glm::vec3 at = m_race->line().positionAt(index);
            const glm::vec3 direction = m_race->line().directionAt(index);
            placeVehicle(vehicle, at, std::atan2(direction.x, direction.z));
            vehicle.ai->recovered();
        }
    }
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

void Game::updateMenu()
{
    const InputState& in = m_window.input();
    if (in.pressed(Key::M) || in.pressed(Button::Start)) {
        m_menuOpen = !m_menuOpen;
    }
    if (!m_menuOpen || m_events.empty()) {
        return;
    }
    const int count = static_cast<int>(m_events.size());
    if (in.pressed(Key::Down) || in.pressed(Key::S) || in.pressed(Button::DpadDown)) {
        m_menuSelection = (m_menuSelection + 1) % count;
    }
    if (in.pressed(Key::Up) || in.pressed(Key::W) || in.pressed(Button::DpadUp)) {
        m_menuSelection = (m_menuSelection + count - 1) % count;
    }
    if (in.pressed(Key::Enter) || in.pressed(Button::A)) {
        m_menuOpen = false;
        startRace(m_events[static_cast<std::size_t>(m_menuSelection)]);
    }
    if (in.pressed(Button::B)) {
        m_menuOpen = false;
    }
}

void Game::updateEventStarts()
{
    m_nearbyStart.reset();
    if (m_race || !m_playerPlaced || m_menuOpen) {
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
    if (m_cameraMode == CameraMode::Free || !m_playerPlaced) {
        const float look = 0.0025F;
        if (m_window.mouseCaptured()) {
            m_freeYaw += in.mouseDx * look;
            m_freePitch = std::clamp(m_freePitch - in.mouseDy * look, -1.5F, 1.5F);
        }
        m_freeYaw += in.axis(Axis::RightX) * dt * 2.0F;
        m_freePitch = std::clamp(m_freePitch + in.axis(Axis::RightY) * dt * 1.5F, -1.5F, 1.5F);
        const glm::vec3 forward(std::sin(m_freeYaw) * std::cos(m_freePitch), std::sin(m_freePitch),
            std::cos(m_freeYaw) * std::cos(m_freePitch));
        const glm::vec3 right = glm::normalize(glm::vec3(std::cos(m_freeYaw), 0.0F, -std::sin(m_freeYaw)));
        float speed = in.held(Key::Shift) ? 250.0F : 40.0F;
        glm::vec3 move(0.0F);
        if (m_cameraMode == CameraMode::Free) {
            move += forward * ((in.held(Key::W) ? 1.0F : 0.0F) - (in.held(Key::S) ? 1.0F : 0.0F));
            move += right * ((in.held(Key::D) ? 1.0F : 0.0F) - (in.held(Key::A) ? 1.0F : 0.0F));
            move.y += (in.held(Key::E) ? 1.0F : 0.0F) - (in.held(Key::Q) ? 1.0F : 0.0F);
            move += forward * in.axis(Axis::LeftY) + right * in.axis(Axis::LeftX);
            speed *= 1.0F + in.axis(Axis::RightTrigger) * 5.0F;
        }
        m_camera.position += move * speed * dt;
        m_camera.target = m_camera.position + forward;
        m_camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
        m_camera.fov = 1.05F;
        return;
    }

    const CarPhysics& car = *player().physics;
    const CarSpec& spec = *player().spec;
    const glm::vec3 carPosition = car.position();
    const glm::vec3 forward = car.forward();
    const glm::vec3 flatForward
        = glm::normalize(glm::vec3(forward.x, 0.0F, forward.z) + glm::vec3(0.0F, 0.0F, 1e-4F));
    const float length = spec.boundsMax[2] - spec.boundsMin[2];
    if (m_cameraMode == CameraMode::Hood) {
        const glm::mat4 transform = car.transform();
        const glm::vec3 eye(0.0F, spec.boundsMax[1] * 0.92F, spec.boundsMax[2] * 0.25F);
        m_camera.position = glm::vec3(transform * glm::vec4(eye, 1.0F));
        m_camera.target = m_camera.position + forward * 10.0F;
        m_camera.up = car.up();
        m_camera.fov = 1.15F;
        return;
    }
    const bool far = m_cameraMode == CameraMode::Far;
    const float distance = length * (far ? 1.9F : 1.35F) + 1.0F;
    const float height = spec.boundsMax[1] * (far ? 1.35F : 1.05F) + 0.4F;
    // Look where the car is going when sliding, not only where it points.
    glm::vec3 heading = flatForward;
    const glm::vec3 velocity = car.velocity();
    const glm::vec3 flatVelocity(velocity.x, 0.0F, velocity.z);
    if (glm::length(flatVelocity) > 3.0F && glm::dot(flatVelocity, flatForward) > 0.0F) {
        heading = glm::normalize(glm::mix(flatForward, glm::normalize(flatVelocity), 0.35F));
    }
    const glm::vec3 wanted = carPosition - heading * distance + glm::vec3(0.0F, height, 0.0F);
    // A critically damped spring behind the car.
    constexpr float kStiffness = 60.0F;
    const float damping = 2.0F * std::sqrt(kStiffness);
    const glm::vec3 offset = m_camera.position - wanted;
    m_chaseVelocity += (-kStiffness * offset - damping * (m_chaseVelocity - velocity)) * dt;
    m_camera.position += m_chaseVelocity * dt;
    if (glm::length(m_camera.position - wanted) > distance * 3.0F) {
        m_camera.position = wanted;
        m_chaseVelocity = velocity;
    }
    // Keep the camera above the ground it passes over.
    if (const std::optional<float> ground
        = m_collision->groundHeight(m_camera.position.x, m_camera.position.z, m_camera.position.y + 5.0F)) {
        m_camera.position.y = std::max(m_camera.position.y, *ground + 0.6F);
    }
    m_camera.target = carPosition + glm::vec3(0.0F, spec.boundsMax[1] * 0.7F, 0.0F) + heading * 1.5F;
    m_camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
    // Widen the view a little with speed.
    m_camera.fov = 1.0F + std::min(car.speed() / 90.0F, 1.0F) * 0.15F;
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

void Game::drawVehicles()
{
    if (!m_playerPlaced) {
        return;
    }
    // Outlines of the bodies and wheels until the car models are decoded.
    for (const auto& vehicle : m_vehicles) {
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
            const glm::vec4 colour = wheel.slipMagnitude > 1.0F ? glm::vec4(1.0F, 0.9F, 0.2F, 1.0F)
                                                                : glm::vec4(0.9F, 0.9F, 0.9F, 1.0F);
            const float radius = car.wheelRadius(w);
            m_overlay->circle(wheel.hubWorld, forward, up, radius, colour);
            // A spoke shows the wheel turning.
            const glm::vec3 spoke = forward * std::cos(wheel.spinAngle) - up * std::sin(wheel.spinAngle);
            m_overlay->line(wheel.hubWorld, wheel.hubWorld + spoke * radius, colour);
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

void Game::drawMenu()
{
    if (!m_menuOpen) {
        return;
    }
    const auto w = static_cast<float>(m_window.width());
    const auto h = static_cast<float>(m_window.height());
    const float rowHeight = m_overlay->lineHeight(0.72F) + 4.0F;
    const int visible = std::max(4, static_cast<int>((h * 0.7F) / rowHeight));
    const int count = static_cast<int>(m_events.size());
    const int first = std::clamp(m_menuSelection - visible / 2, 0, std::max(count - visible, 0));
    const float panelW = std::min(w - 48.0F, 900.0F);
    const float x0 = (w - panelW) * 0.5F;
    const float y0 = h * 0.12F;
    const std::string hint = "Enter to start, M to close";
    m_overlay->rect(x0, y0, panelW, rowHeight * static_cast<float>(std::min(visible, count)) + 72.0F, kHudPanel);
    m_overlay->text(x0 + 20.0F, y0 + 12.0F, "Races", kHudText, 1.0F);
    m_overlay->text(x0 + panelW - 20.0F - m_overlay->textWidth(hint, 0.65F), y0 + 20.0F, hint, kHudDim, 0.65F);
    float y = y0 + 56.0F;
    for (int i = first; i < std::min(first + visible, count); ++i) {
        const RaceEvent& event = m_events[static_cast<std::size_t>(i)];
        const bool selected = i == m_menuSelection;
        if (selected) {
            m_overlay->rect(x0 + 8.0F, y - 2.0F, panelW - 16.0F, rowHeight, {1.0F, 1.0F, 1.0F, 0.08F});
        }
        m_overlay->text(x0 + 20.0F, y, event.name, selected ? kHudAccent : kHudText, 0.72F);
        const std::string detail = std::format("{}  ·  {} lap{}  ·  {:.1f} km", event.type, event.laps,
            event.laps == 1 ? "" : "s", event.lengthMetres / 1000.0F);
        m_overlay->text(x0 + panelW * 0.5F, y, detail, kHudDim, 0.72F);
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
    drawMenu();
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
            if (m_menuOpen) {
                m_menuOpen = false;
            } else if (m_window.mouseCaptured()) {
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
        if (in.pressed(Key::Tab) || in.pressed(Button::Back)) {
            m_showDebug = !m_showDebug;
        }
        if (in.pressed(Key::C) || in.pressed(Button::RightStick) || in.pressed(Button::Y)) {
            m_cameraMode = m_cameraMode == CameraMode::Chase ? CameraMode::Far
                : m_cameraMode == CameraMode::Far          ? CameraMode::Hood
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
        updateMenu();
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
            stepPhysics(dt);
            if (m_options.demoDrive && secondTicked(m_time, dt)) {
                const CarPhysics& car = *player().physics;
                logInfo("demo: v {:.1f} m/s, rpm {:.0f}, gear {}, at {:.0f} {:.0f}", car.forwardSpeed(),
                    car.engineRpm(), car.gear(), car.position().x, car.position().z);
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
        m_frame->sky(frame);
        drawVehicles();
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
