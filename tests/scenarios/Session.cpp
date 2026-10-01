#include "scenarios/Session.h"

#include "support/TestSupport.h"

#include "base/Host.h"
#include "base/Log.h"
#include "car/CarPhysics.h"
#include "platform/Window.h"
#include "profile/Profile.h"
#include "profile/ProfileSchema.h"
#include "profile/ProfileStore.h"
#include "render/FrameRenderer.h"
#include "render/Gl.h"
#include "render/ShadowPass.h"

#include <catch2/catch_test_case_info.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <format>
#include <numbers>
#include <string>
#include <utility>

namespace openhorizon::test {

namespace {

/// Why the window could not be made, or empty when it was.
const std::string& openGlError()
{
    static const std::string error = [] {
        try {
            window();
            return std::string();
        } catch (const std::exception& e) {
            return std::string(e.what());
        }
    }();
    return error;
}

/// The disc mounted once for the whole process.
const Vfs& sharedDisc()
{
    static const std::unique_ptr<Vfs> vfs = [] {
        auto mounted = std::make_unique<Vfs>();
        mounted->mountDisc(pathFromUtf8(openhorizon::test::gameDir()));
        return mounted;
    }();
    return *vfs;
}

/// Keeps the scenarios off the player's own folders and gives each case a
/// log of its own.
class ScenarioFolders : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo& /*info*/) override
    {
        const std::filesystem::path temp = std::filesystem::temp_directory_path();
        // Not the player's shader cache; this folder persists between runs,
        // so later runs also exercise the cached path.
        setEnvironmentVariable(userFolderVariable(UserFolder::Cache), pathToUtf8(temp / "openhorizon-scenarios-cache"));
        // Nor the player's own profile: every Session names its own folder,
        // and this catches any game made without one.
        setEnvironmentVariable(userFolderVariable(UserFolder::Data), pathToUtf8(temp / "openhorizon-scenarios-data"));
    }

    void testCaseStarting(const Catch::TestCaseInfo& info) override
    {
        std::string name = info.name;
        std::replace_if(
            name.begin(), name.end(), [](char c) { return c == '/' || c == '\\' || c == ':' || c == ' '; }, '_');
        openLogFile(std::filesystem::temp_directory_path() / "openhorizon-scenarios-logs" / (name + ".log"));
    }
};

} // namespace

} // namespace openhorizon::test

CATCH_REGISTER_LISTENER(openhorizon::test::ScenarioFolders)

namespace openhorizon::test {

GameScenario::GameScenario()
    : m_gameDir(openhorizon::test::gameDir())
{
    if (!openGlError().empty()) {
        SKIP("no OpenGL context (" << openGlError() << ")");
    }
}

const Vfs& GameScenario::disc() const
{
    return sharedDisc();
}

Window& window()
{
    static Window instance([] {
        WindowOptions options;
        options.width = 640;
        options.height = 360;
        options.hidden = true;
        options.vsync = false;
        return options;
    }());
    return instance;
}

std::filesystem::path freshProfileDir(std::string_view name)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "openhorizon-scenarios-profiles" / name;
    std::filesystem::remove_all(dir);
    return dir;
}

void saveProfile(const std::filesystem::path& dir, const std::function<void(profile::Profile&)>& edit)
{
    profile::Profile profile(profile::ProfileSchema::load(sharedDisc()));
    edit(profile);
    CHECK(profile::ProfileStore(dir).save(profile));
}

Session::Session(GameOptions options)
{
    options.gameDir = pathFromUtf8(openhorizon::test::gameDir());
    if (options.profileDir.empty()) {
        // A new profile for every session, so none sees what another
        // discovered.
        static int sessions = 0;
        options.profileDir = freshProfileDir(std::format("session{}", ++sessions));
    }
    m_game = std::make_unique<Game>(window(), options);
}

Session::~Session() = default;

bool Session::waitForCar(float seconds)
{
    for (float t = 0.0F; t < seconds && !m_game->inControl(); t += kStep) {
        frame({});
    }
    return m_game->inControl();
}

void Session::run(float seconds, const Hold& hold)
{
    for (float t = 0.0F; t < seconds; t += kStep) {
        frame(hold);
    }
}

void Session::press(Button button)
{
    frame({}, button);
    frame({});
}

void Session::pressTimes(Button button, int times)
{
    for (int i = 0; i < times; ++i) {
        press(button);
    }
}

bool Session::pressMayQuit(Button button)
{
    return step({}, button) && step({}, std::nullopt);
}

bool Session::focusPauseItem(ui::PauseItem item)
{
    const ui::PauseMenu* menu = m_game->pauseMenu();
    if (menu == nullptr) {
        return false;
    }
    const auto focused = [menu] {
        const auto focus = static_cast<std::size_t>(menu->focus());
        return focus < menu->entries().size() ? std::optional(menu->entries()[focus].item) : std::nullopt;
    };
    for (std::size_t i = 0; i < menu->entries().size() && focused() != item; ++i) {
        press(Button::DpadDown);
    }
    return focused() == item;
}

void Session::capture(const std::string& name)
{
    const char* dir = std::getenv("OPENHORIZON_CAPTURE_DIR");
    if (dir != nullptr && *dir != '\0') {
        m_game->saveScreenshot(pathFromUtf8(dir) / pathFromUtf8(name + ".png"));
    }
}

void Session::frame(const Hold& hold, std::optional<Button> button)
{
    CHECK(step(hold, button));
}

bool Session::step(const Hold& hold, std::optional<Button> button)
{
    InputState input;
    input.previousButtons = m_previousButtons;
    input.axes[static_cast<std::size_t>(Axis::RightTrigger)] = hold.throttle;
    input.axes[static_cast<std::size_t>(Axis::LeftTrigger)] = hold.brake;
    input.axes[static_cast<std::size_t>(Axis::LeftX)] = hold.steer;
    input.axes[static_cast<std::size_t>(Axis::RightY)] = hold.zoom;
    input.axes[static_cast<std::size_t>(Axis::LeftY)] = hold.cursorY;
    input.axes[static_cast<std::size_t>(Axis::RightX)] = hold.lookX;
    if (button) {
        input.buttons[static_cast<std::size_t>(*button)] = true;
    }
    m_previousButtons = input.buttons;
    return m_game->frame(input, kStep);
}

glm::vec3 SceneReadback::worldAt(int x, int y) const
{
    const glm::vec4 ndc(2.0F * (static_cast<float>(x) + 0.5F) / static_cast<float>(width) - 1.0F,
        2.0F * (static_cast<float>(y) + 0.5F) / static_cast<float>(height) - 1.0F, depth[at(x, y)], 1.0F);
    const glm::vec4 world = inverseViewProjection * ndc;
    return glm::vec3(world) / world.w;
}

SceneReadback readScene(Session& session)
{
    SceneReadback scene;
    // The sun shadow mask is made at the scene's size.
    const render::ShadowPass* pass = session.game().shadowPass();
    const render::FrameRenderer* frame = session.game().frameRenderer();
    if (pass == nullptr || frame == nullptr || pass->width() <= 0) {
        return scene;
    }
    scene.width = pass->width();
    scene.height = pass->height();
    const std::size_t pixels = static_cast<std::size_t>(scene.width) * static_cast<std::size_t>(scene.height);
    scene.depth.resize(pixels);
    glGetTextureImage(frame->sceneDepth(), 0, GL_DEPTH_COMPONENT, GL_FLOAT,
        static_cast<GLsizei>(scene.depth.size() * sizeof(float)), scene.depth.data());
    const render::Camera& camera = session.game().camera();
    scene.inverseViewProjection = glm::inverse(
        camera.projection(static_cast<float>(scene.width) / static_cast<float>(scene.height)) * camera.view());
    return scene;
}

bool played(const std::vector<std::string>& sounds, std::string_view name)
{
    return std::find(sounds.begin(), sounds.end(), name) != sounds.end();
}

std::vector<std::string> playedSince(const Game& game, std::size_t before)
{
    const std::vector<std::string> all = game.uiSoundsPlayed();
    return {all.begin() + static_cast<std::ptrdiff_t>(std::min(before, all.size())), all.end()};
}

std::optional<audio::MixSnapshotId> gameplayMix(const Game& game)
{
    const audio::SnapshotMixer* mix = game.mixSnapshots();
    return mix ? mix->current(audio::MixGroup::Gameplay) : std::nullopt;
}

bool hasMarker(const ui::WorldMapState& state, std::string_view value, std::string_view name)
{
    return std::any_of(state.markers.begin(), state.markers.end(),
        [&](const ui::WorldMapMarker& m) { return m.marker.value == value && (name.empty() || m.name == name); });
}

float headingOf(const glm::vec3& direction)
{
    return std::atan2(direction.x, direction.z);
}

float angleBetween(float a, float b)
{
    return std::abs(std::remainder(a - b, 2.0F * 3.14159265F));
}

float steerTowards(const CarPhysics& car, glm::vec2 target)
{
    const glm::vec3 at = car.position();
    const float wanted = std::atan2(target.x - at.x, target.y - at.z);
    return std::clamp(
        std::remainder(wanted - headingOf(car.forward()), 2.0F * std::numbers::pi_v<float>) * 3.0F, -1.0F, 1.0F);
}

} // namespace openhorizon::test
