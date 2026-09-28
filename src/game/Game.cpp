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
const glm::vec4 kHudText{0.96F, 0.96F, 0.94F, 1.0F};
const glm::vec4 kHudDim{0.96F, 0.96F, 0.94F, 0.55F};
const glm::vec4 kHudPanel{0.02F, 0.02F, 0.03F, 0.55F};
/// The one accent the HUD uses, for the rev band near the redline.
const glm::vec4 kHudAccent{1.0F, 0.42F, 0.1F, 1.0F};

float approach(float value, float target, float rate)
{
    return value < target ? std::min(value + rate, target) : std::max(value - rate, target);
}

} // namespace

Game::Game(Window& window, const GameOptions& options)
    : m_window(window)
    , m_options(options)
{
    m_vfs.mountDisc(options.gameDir);
    m_database = std::make_unique<Database>(m_vfs.readOrThrow("media/db/gamedb.slt"));
    m_jobs = std::make_unique<JobPool>();
    m_frame = std::make_unique<render::FrameRenderer>();
    m_overlay = std::make_unique<render::Overlay>("sans-serif:bold", 26);

    const int carId = carIdByMediaName(*m_database, options.car);
    if (carId < 0) {
        throw DataError("no car named {} in the game database", options.car);
    }
    m_spec = loadCarSpec(*m_database, carId, &m_vfs);
    m_car = std::make_unique<CarPhysics>(m_spec);
    m_car->assists().automaticGearbox = !options.manualGearbox;
    logInfo("car: {} ({} kg, {:.0f} N·m)", m_spec.mediaName, m_spec.massKg, m_spec.reference.peakTorqueNm);

    // Spheres along the body stand in for the car's collision shape: a low
    // row at bumper height and a row near the roof, so both walls and
    // rollovers are caught.
    const glm::vec3 lo(m_spec.boundsMin[0], m_spec.boundsMin[1], m_spec.boundsMin[2]);
    const glm::vec3 hi(m_spec.boundsMax[0], m_spec.boundsMax[1], m_spec.boundsMax[2]);
    const float halfWidth = (hi.x - lo.x) * 0.5F;
    const float lowRadius = std::min(halfWidth, 0.45F);
    const float lowY = std::max(lo.y, 0.0F) + std::max(m_spec.front.rideHeight, 0.1F) + lowRadius;
    const float roofRadius = std::min(halfWidth * 0.8F, 0.4F);
    const float roofY = hi.y - roofRadius;
    constexpr int kSteps = 4;
    for (int i = 0; i < kSteps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(kSteps - 1);
        const float z = (lo.z + lowRadius) + t * ((hi.z - lowRadius) - (lo.z + lowRadius));
        const float x = std::max(0.0F, halfWidth - lowRadius);
        m_bodySpheres.push_back({{-x, lowY, z}, lowRadius});
        m_bodySpheres.push_back({{x, lowY, z}, lowRadius});
    }
    for (float z : {lo.z * 0.35F, hi.z * 0.1F}) {
        m_bodySpheres.push_back({{0.0F, roofY, z}, roofRadius});
    }
    loadWorld();
}

Game::~Game()
{
    // Workers may still run jobs that reference the world; stop them first.
    m_worldRenderer.reset();
    m_collision.reset();
    m_jobs.reset();
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
    logInfo("spawn: X {:.1f}, Z {:.1f}, heading {:.0f}°", m_spawnPosition.x, m_spawnPosition.z,
        m_spawnHeading * 180.0F / kPi);
    m_camera.position = m_spawnPosition + glm::vec3(0.0F, 30.0F, 0.0F);
    m_camera.target = m_spawnPosition;
}

void Game::spawnCar()
{
    // Start just above the ground so the suspension settles onto it.
    const std::optional<float> ground
        = m_collision->groundHeight(m_spawnPosition.x, m_spawnPosition.z, m_spawnPosition.y + 50.0F);
    glm::vec3 at = m_spawnPosition;
    if (ground) {
        at.y = *ground + 0.05F;
    }
    m_car->reset(at, m_spawnHeading, *m_collision);
    m_carPlaced = true;
    m_chaseVelocity = glm::vec3(0.0F);
    logInfo("car placed at height {:.1f} ({})", at.y, ground ? "on the ground" : "no ground found below");
}

CarControls Game::readControls(float dt)
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

void Game::stepPhysics(float dt, const CarControls& controls)
{
    constexpr float kStep = 1.0F / CarPhysics::kStepRate;
    m_physicsAccumulator = std::min(m_physicsAccumulator + dt, 0.1F);
    CarControls stepControls = controls;
    while (m_physicsAccumulator >= kStep) {
        m_car->step(stepControls, *m_collision);
        collideBody();
        m_physicsAccumulator -= kStep;
        // A gear request is one press, not one per step.
        stepControls.shiftUp = false;
        stepControls.shiftDown = false;
    }
}

void Game::collideBody()
{
    const glm::mat4 transform = m_car->transform();
    for (const BodySphere& sphere : m_bodySpheres) {
        const glm::vec3 centre = glm::vec3(transform * glm::vec4(sphere.centre, 1.0F));
        m_contacts.clear();
        m_collision->sphereContacts(centre, sphere.radius, m_contacts);
        // The deepest few are enough; a sphere against a wall touches many
        // coplanar triangles.
        const std::size_t count = std::min<std::size_t>(m_contacts.size(), 3);
        for (std::size_t i = 0; i < count; ++i) {
            const physics::SphereContact& contact = m_contacts[i];
            m_car->resolveContact(contact.point, contact.normal, contact.depth, 0.2F, 0.4F);
        }
    }
}

void Game::updateCamera(float dt)
{
    const InputState& in = m_window.input();
    if (m_cameraMode == CameraMode::Free || !m_carPlaced) {
        const float look = 0.0025F;
        if (m_window.mouseCaptured()) {
            m_freeYaw += in.mouseDx * look;
            m_freePitch = std::clamp(m_freePitch - in.mouseDy * look, -1.5F, 1.5F);
        }
        m_freeYaw += in.axis(Axis::RightX) * dt * 2.0F;
        m_freePitch = std::clamp(m_freePitch + in.axis(Axis::RightY) * dt * 1.5F, -1.5F, 1.5F);
        const glm::vec3 forward(
            std::sin(m_freeYaw) * std::cos(m_freePitch), std::sin(m_freePitch), std::cos(m_freeYaw) * std::cos(m_freePitch));
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

    const glm::vec3 carPosition = m_car->position();
    const glm::vec3 forward = m_car->forward();
    const glm::vec3 flatForward = glm::normalize(glm::vec3(forward.x, 0.0F, forward.z) + glm::vec3(0.0F, 0.0F, 1e-4F));
    const float length = m_spec.boundsMax[2] - m_spec.boundsMin[2];
    if (m_cameraMode == CameraMode::Hood) {
        const glm::mat4 transform = m_car->transform();
        const glm::vec3 eye(0.0F, m_spec.boundsMax[1] * 0.92F, m_spec.boundsMax[2] * 0.25F);
        m_camera.position = glm::vec3(transform * glm::vec4(eye, 1.0F));
        m_camera.target = m_camera.position + forward * 10.0F;
        m_camera.up = m_car->up();
        m_camera.fov = 1.15F;
        return;
    }
    const bool far = m_cameraMode == CameraMode::Far;
    const float distance = length * (far ? 1.9F : 1.35F) + 1.0F;
    const float height = m_spec.boundsMax[1] * (far ? 1.35F : 1.05F) + 0.4F;
    // Look where the car is going when sliding, not only where it points.
    glm::vec3 heading = flatForward;
    const glm::vec3 velocity = m_car->velocity();
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
    m_camera.target = carPosition + glm::vec3(0.0F, m_spec.boundsMax[1] * 0.7F, 0.0F) + heading * 1.5F;
    m_camera.up = glm::vec3(0.0F, 1.0F, 0.0F);
    // Widen the view a little with speed.
    m_camera.fov = 1.0F + std::min(m_car->speed() / 90.0F, 1.0F) * 0.15F;
}

render::FrameParams Game::frameParams() const
{
    render::FrameParams frame;
    const float aspect = static_cast<float>(std::max(m_window.width(), 1)) / static_cast<float>(std::max(m_window.height(), 1));
    frame.view = m_camera.view();
    frame.projection = m_camera.projection(aspect);
    frame.cameraPosition = m_camera.position;
    // Late afternoon, the time of day the game's free roam opens at (16:00).
    frame.sunDirection = glm::normalize(glm::vec3(-0.45F, -0.55F, 0.7F));
    frame.sunColour = glm::vec3(3.2F, 2.9F, 2.5F);
    frame.skyColour = glm::vec3(0.55F, 0.72F, 1.0F);
    frame.groundColour = glm::vec3(0.32F, 0.28F, 0.22F);
    frame.fogColour = glm::vec3(0.72F, 0.8F, 0.9F);
    frame.fogDensity = 0.00009F;
    frame.time = m_time;
    return frame;
}

void Game::drawCar()
{
    if (!m_carPlaced) {
        return;
    }
    // Outline of the body and wheels until the car models are decoded.
    const glm::mat4 transform = m_car->transform();
    const glm::vec3 lo(m_spec.boundsMin[0], m_spec.boundsMin[1], m_spec.boundsMin[2]);
    const glm::vec3 hi(m_spec.boundsMax[0], m_spec.boundsMax[1], m_spec.boundsMax[2]);
    const glm::vec3 lift(0.0F, std::max(m_spec.front.rideHeight, 0.1F), 0.0F);
    const glm::vec3 centre = (lo + hi) * 0.5F + lift * 0.5F;
    const glm::vec3 half = (hi - lo) * 0.5F - glm::vec3(0.0F, lift.y * 0.5F, 0.0F);
    m_overlay->box(transform * glm::translate(glm::mat4(1.0F), centre), half, {0.95F, 0.3F, 0.15F, 1.0F});
    const glm::vec3 up = m_car->up();
    for (int w = 0; w < CarPhysics::kWheels; ++w) {
        const CarPhysics::WheelState& wheel = m_car->wheels()[static_cast<std::size_t>(w)];
        const glm::quat steer = m_car->orientation() * glm::angleAxis(wheel.steerAngle, glm::vec3(0.0F, 1.0F, 0.0F));
        const glm::vec3 forward = steer * glm::vec3(0.0F, 0.0F, 1.0F);
        const glm::vec4 colour = wheel.slipMagnitude > 1.0F ? glm::vec4(1.0F, 0.9F, 0.2F, 1.0F)
                                                            : glm::vec4(0.9F, 0.9F, 0.9F, 1.0F);
        const float radius = m_car->wheelRadius(w);
        m_overlay->circle(wheel.hubWorld, forward, up, radius, colour);
        // A spoke shows the wheel turning.
        const glm::vec3 spoke = forward * std::cos(wheel.spinAngle) - up * std::sin(wheel.spinAngle);
        m_overlay->line(wheel.hubWorld, wheel.hubWorld + spoke * radius, colour);
    }
}

void Game::drawHud(float dt)
{
    const auto w = static_cast<float>(m_window.width());
    const auto h = static_cast<float>(m_window.height());
    m_fps = m_fps * 0.95F + (dt > 0.0F ? 1.0F / dt : 0.0F) * 0.05F;

    if (m_carPlaced) {
        const float speed = std::abs(m_car->forwardSpeed()) * kMsToKmh;
        const int gear = m_car->gear();
        const std::string gearText = gear < 0 ? "R" : gear == 0 ? "N" : std::to_string(gear);
        const float rpm = m_car->engineRpm();
        const float redline = m_spec.redlineRpm;

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
        const bool high = rpm > redline * 0.9F;
        m_overlay->rect(barX, barY, barW * fraction, 12.0F, high ? kHudAccent : kHudText);
        m_overlay->text(barX, barY - 26.0F, std::format("{:.0f} rpm", rpm), kHudDim, 0.7F);
    }

    if (m_showDebug) {
        const auto& stats = m_worldRenderer->stats();
        const glm::vec3 p = m_carPlaced ? m_car->position() : m_camera.position;
        const std::vector<std::string> lines = {
            std::format("{:.0f} fps", m_fps),
            std::format("X {:.1f}  Y {:.1f}  Z {:.1f}", p.x, p.y, p.z),
            std::format("tiles {}  triangles {}k  batches {}", stats.tilesDrawn, stats.triangles / 1000, stats.batches),
            std::format("textures {} ({} MB)  jobs {}", stats.texturesLoaded, stats.textureBytes >> 20,
                stats.pendingJobs),
            std::format("collision triangles {}k, tree trunks {}", m_collision->triangleCount() / 1000,
                m_collision->trunkCount()),
        };
        float y = 16.0F;
        m_overlay->rect(8.0F, 8.0F, 520.0F, m_overlay->lineHeight(0.7F) * static_cast<float>(lines.size()) + 16.0F,
            kHudPanel);
        for (const std::string& line : lines) {
            m_overlay->text(18.0F, y, line, kHudText, 0.7F);
            y += m_overlay->lineHeight(0.7F);
        }
    }
    if (!m_carPlaced) {
        const std::string message = "Loading the area around the festival";
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

        if (in.pressed(Key::Escape)) {
            if (m_window.mouseCaptured()) {
                m_window.setMouseCaptured(false);
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
        if (in.pressed(Key::R) && m_carPlaced) {
            m_car->recover(*m_collision);
        }

        const glm::vec3 focus = m_carPlaced ? m_car->position() : m_spawnPosition;
        m_collision->update(focus);
        if (!m_carPlaced && m_collision->readyAt(m_spawnPosition)) {
            // Wait for the ground itself, but not forever: a spawn point off
            // the map has none.
            m_spawnWait += dt;
            if (m_collision->groundHeight(m_spawnPosition.x, m_spawnPosition.z, m_spawnPosition.y + 50.0F)
                || m_spawnWait > 10.0F) {
                spawnCar();
            }
        }
        if (m_carPlaced) {
            const CarControls controls = readControls(dt);
            stepPhysics(dt, controls);
            if (m_options.demoDrive && static_cast<int>(m_time) != static_cast<int>(m_time - dt)) {
                const auto& w = m_car->wheels();
                logInfo("demo: v {:.1f} m/s, rpm {:.0f}, gear {}, y {:.2f}, ground {}{}{}{}, load {:.0f} {:.0f} {:.0f} "
                        "{:.0f}, spin {:.1f} {:.1f}, body contacts {}",
                    m_car->forwardSpeed(), m_car->engineRpm(), m_car->gear(), m_car->position().y, w[0].onGround,
                    w[1].onGround, w[2].onGround, w[3].onGround, w[0].load, w[1].load, w[2].load, w[3].load,
                    w[2].spin, w[3].spin, m_contacts.size());
                logInfo("demo: collision {}k triangles, {} trunks", m_collision->triangleCount() / 1000,
                    m_collision->trunkCount());
            }
            if (m_car->position().y < -600.0F) {
                // Fell through a hole in the world.
                spawnCar();
            }
        }
        updateCamera(dt);
        m_worldRenderer->update(m_camera.position);

        const render::FrameParams frame = frameParams();
        m_frame->begin(m_window.width(), m_window.height());
        m_worldRenderer->draw(frame);
        m_frame->sky(frame);
        drawCar();
        m_overlay->flush(frame.projection * frame.view, m_window.width(), m_window.height());
        m_frame->finish(1.0F);
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
