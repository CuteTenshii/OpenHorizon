// Inspects and renders the game's pause menu and full-screen map without
// the game: dumps a scene's contracts and event handlers, and draws the
// pause menu or the map for sample states into a PNG through an offscreen
// OpenGL context.

#include "base/Error.h"
#include "data/Database.h"
#include "data/StringTable.h"
#include "platform/Window.h"
#include "render/Gl.h"
#include "ui/AnarkFormats.h"
#include "ui/LoadingScreen.h"
#include "ui/MenuCommon.h"
#include "ui/PauseMenu.h"
#include "ui/Presentation.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"

#include <glm/gtc/constants.hpp>
#include <glm/trigonometric.hpp>
#include <pugixml.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace fh::ui;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1menu DISC contracts SCENE [--names FILE]\n"
        "       fh1menu DISC pause OUT.png [--race] [--steps KEYS] [--size WxH] [--time S]\n"
        "       fh1menu DISC loading OUT.png [--group NAME] [--seed N] [--size WxH] [--time S]\n"
        "       fh1menu DISC map OUT.png [--track NAME] [--at X,Z] [--heading DEG] [--move DX,DY --move-time S]\n"
        "                                [--zoom Z --zoom-time S] [--steps KEYS] [--size WxH] [--time S]\n"
        "  DISC       the extracted disc (its root or its media folder)\n"
        "  SCENE      a scene under media/ui/scenes/ui4 without extension, such as 925_pause_menu\n"
        "  contracts  lists the scene's behaviour elements (contracts) with their parameters and the\n"
        "             event handlers of each logic owner; names are shown as hashes unless FILE, a list of\n"
        "             candidate names one per line (such as the executable's strings), contains them\n"
        "  pause      draws the pause menu of a free-roam session (a race with --race) after the menu inputs\n"
        "             in KEYS, one per frame: u d (up, down), a (A), b (B); then runs --time seconds\n"
        "             more; set SDL_VIDEO_DRIVER=offscreen to render without a window\n"
        "  loading    draws the loading screen with a tip of the LoadingDefs.xml group NAME (default\n"
        "             freeroam), after --time seconds (default 3)\n"
        "  map        draws the full-screen map opened with the player at X,Z facing DEG (0 north, 90 east) and\n"
        "             the track's race starts, after holding the left stick at DX,DY for --move-time seconds,\n"
        "             the right stick at Z (positive zooms in) for --zoom-time seconds, and the KEYS\n");
    return 2;
}

std::string scenePath(std::string_view path)
{
    if (path.find('/') == std::string_view::npos && path.find('\\') == std::string_view::npos) {
        return "media/ui/scenes/ui4/" + std::string(path);
    }
    return std::string(path);
}

/// Anark hashes back to names, from a list of candidates.
class Names {
public:
    void load(const std::string& path)
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
                line.pop_back();
            }
            if (!line.empty()) {
                m_names.emplace(anarkHash(line), line);
            }
        }
    }
    std::string name(std::uint32_t hash) const
    {
        const auto it = m_names.find(hash & 0x07FFFFFFU);
        return it != m_names.end() ? it->second : std::format("#{:07x}", hash & 0x07FFFFFFU);
    }

private:
    std::unordered_map<std::uint32_t, std::string> m_names;
};

std::string pathOf(const Presentation& scene, int element)
{
    std::vector<std::string> names;
    for (int e = element; e >= 0; e = scene.parent(e)) {
        names.push_back(scene.name(e));
    }
    std::string path;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        path += path.empty() ? *it : "/" + *it;
    }
    return path;
}

int dumpContracts(const fh::Vfs& vfs, const std::string& path, const Names& names)
{
    const Presentation scene(SceneFiles::load(vfs, path));
    const BgfFile& bgf = scene.files().bgf;
    for (std::size_t i = 0; i < bgf.elements.size(); ++i) {
        const BgfElement& element = bgf.elements[i];
        if (element.kind != ElementKind::Behavior) {
            continue;
        }
        std::printf("[%zu] behaviour %s under %s\n", i, names.name(element.nameHash).c_str(),
            pathOf(scene, element.parent).c_str());
        for (const Attribute& attribute : element.attributes) {
            const bool text = attribute.type == static_cast<std::uint8_t>(AttributeType::String)
                || attribute.type == static_cast<std::uint8_t>(AttributeType::ContractString)
                || attribute.type == static_cast<std::uint8_t>(AttributeType::ContractPath);
            if (text && attribute.raw < bgf.strings.size()) {
                std::printf("    %s (type %u) = \"%s\" (string %u)\n", names.name(attribute.hash).c_str(),
                    attribute.type, bgf.strings[attribute.raw].c_str(), attribute.raw);
            } else {
                std::printf("    %s (type %u) = %g (0x%08x)\n", names.name(attribute.hash).c_str(), attribute.type,
                    static_cast<double>(attribute.asFloat()), attribute.raw);
            }
        }
    }
    for (const LogicOwner& owner : bgf.logic) {
        std::printf("logic of [%u] %s\n", owner.element, pathOf(scene, static_cast<int>(owner.element)).c_str());
        for (const LogicHandler& handler : owner.handlers) {
            std::printf("    on %s:\n", names.name(handler.event).c_str());
            for (const LogicAction& action : handler.actions) {
                std::string what;
                if (action.command == command::GoToSlideName) {
                    what = "go to slide " + names.name(action.key);
                } else if (action.command == command::SetProperty) {
                    what = std::format("set {} = 0x{:08x}", names.name(action.key), action.value);
                } else if (action.command == command::GoToTime) {
                    what = std::format("go to time {}", action.key);
                } else if (action.command == command::Play) {
                    what = "play";
                } else {
                    what = "command " + names.name(action.command);
                }
                std::printf("        %s: %s\n", pathOf(scene, static_cast<int>(action.target)).c_str(), what.c_str());
            }
        }
    }
    return 0;
}

/// An RGBA8 framebuffer to draw into and read back.
class OffscreenTarget {
public:
    OffscreenTarget(int width, int height)
        : m_width(width)
        , m_height(height)
    {
        glCreateTextures(GL_TEXTURE_2D, 1, &m_colour);
        glTextureStorage2D(m_colour, 1, GL_RGBA8, width, height);
        glCreateFramebuffers(1, &m_framebuffer);
        glNamedFramebufferTexture(m_framebuffer, GL_COLOR_ATTACHMENT0, m_colour, 0);
        if (glCheckNamedFramebufferStatus(m_framebuffer, GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            throw std::runtime_error("offscreen framebuffer is incomplete");
        }
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glViewport(0, 0, width, height);
    }
    ~OffscreenTarget()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteTextures(1, &m_colour);
    }
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    /// A flat grey backdrop standing in for the paused game's picture.
    void clear() const
    {
        const float grey[4] = {0.25F, 0.27F, 0.28F, 1.0F};
        glClearNamedFramebufferfv(m_framebuffer, GL_COLOR, 0, grey);
    }

    xds::Image read() const
    {
        xds::Image image;
        image.width = static_cast<std::uint32_t>(m_width);
        image.height = static_cast<std::uint32_t>(m_height);
        image.rgba.resize(static_cast<std::size_t>(m_width) * static_cast<std::size_t>(m_height) * 4);
        glFinish();
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, m_width, m_height, GL_RGBA, GL_UNSIGNED_BYTE, image.rgba.data());
        const std::size_t row = static_cast<std::size_t>(m_width) * 4;
        std::vector<std::uint8_t> swap(row);
        for (int y = 0; y < m_height / 2; ++y) {
            std::uint8_t* a = image.rgba.data() + static_cast<std::size_t>(y) * row;
            std::uint8_t* b = image.rgba.data() + static_cast<std::size_t>(m_height - 1 - y) * row;
            std::memcpy(swap.data(), a, row);
            std::memcpy(a, b, row);
            std::memcpy(b, swap.data(), row);
        }
        for (std::size_t i = 3; i < image.rgba.size(); i += 4) {
            image.rgba[i] = 255;
        }
        return image;
    }

private:
    int m_width;
    int m_height;
    GLuint m_colour = 0;
    GLuint m_framebuffer = 0;
};

fh::WindowOptions hiddenWindow()
{
    fh::WindowOptions options;
    options.title = "fh1menu";
    options.width = 64;
    options.height = 64;
    options.hidden = true;
    options.vsync = false;
    options.glDebug = std::getenv("FH1_GL_DEBUG") != nullptr;
    return options;
}

bool writePng(const std::string& path, const xds::Image& image)
{
    const std::vector<std::uint8_t> png = xds::writePng(image);
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(out);
}

/// The menu input for one character of a --steps string.
MenuInput stepInput(char step)
{
    MenuInput input;
    switch (step) {
    case 'u':
        input.up = true;
        break;
    case 'd':
        input.down = true;
        break;
    case 'l':
        input.left = true;
        break;
    case 'r':
        input.right = true;
        break;
    case 'a':
        input.accept = true;
        break;
    case 'b':
        input.back = true;
        break;
    case 'x':
        input.x = true;
        break;
    case 'y':
        input.y = true;
        break;
    default:
        break;
    }
    return input;
}

const char* actionName(PauseAction action)
{
    switch (action) {
    case PauseAction::None:
        return "none";
    case PauseAction::Resume:
        return "resume";
    case PauseAction::Restart:
        return "restart";
    case PauseAction::RetireFromEvent:
        return "retire from event";
    case PauseAction::Quit:
        return "quit";
    case PauseAction::OpenMap:
        return "open map";
    case PauseAction::OpenMultiplayer:
        return "open multiplayer";
    case PauseAction::OpenPhotoMode:
        return "open photo mode";
    case PauseAction::OpenMessageCentre:
        return "open message centre";
    case PauseAction::OpenMyProfile:
        return "open my profile";
    case PauseAction::OpenSponsorChallenges:
        return "open sponsor challenges";
    case PauseAction::OpenCoopChallenges:
        return "open co-op challenges";
    case PauseAction::OpenHeadTracking:
        return "open head tracking";
    case PauseAction::OpenCarChallenges:
        return "open car challenges";
    case PauseAction::OpenHorizonRally:
        return "open horizon rally";
    case PauseAction::Finish:
        return "finish";
    case PauseAction::EndFreeRoam:
        return "end free roam";
    case PauseAction::QuitMission:
        return "quit mission";
    case PauseAction::RestartMission:
        return "restart mission";
    }
    return "?";
}

struct RenderOptions {
    bool race = false;
    std::string steps;
    float time = 1.0F;
    int width = 1280;
    int height = 720;
};

int renderPause(const fh::Vfs& vfs, const std::string& out, const RenderOptions& options)
{
    fh::Window window(hiddenWindow());
    OffscreenTarget target(options.width, options.height);
    PauseMenu menu(vfs, "en");
    PauseMenuContext context;
    context.freeRoam = !options.race;
    menu.open(context);
    for (const PauseMenuEntry& entry : menu.entries()) {
        std::printf("entry %d: %s\n", static_cast<int>(entry.item), entry.text.c_str());
    }
    constexpr float kStep = 1.0F / 60.0F;
    const int settle = std::max(1, static_cast<int>(options.time / kStep));
    const auto frame = [&](const MenuInput& input) {
        const PauseAction action = menu.update(input, kStep);
        if (action != PauseAction::None) {
            std::printf("action: %s\n", actionName(action));
        }
        target.clear();
        menu.draw(options.width, options.height);
    };
    for (const char step : options.steps) {
        frame(stepInput(step));
        for (int i = 0; i < 20; ++i) {
            frame(MenuInput{});
        }
    }
    for (int i = 0; i < settle; ++i) {
        frame(MenuInput{});
    }
    std::printf("focus %d%s\n", menu.focus(), menu.confirming() ? ", confirming" : "");
    return writePng(out, target.read()) ? 0 : 1;
}

bool parseSize(const std::string& text, int& width, int& height)
{
    const std::size_t x = text.find('x');
    if (x == std::string::npos) {
        return false;
    }
    width = std::stoi(text.substr(0, x));
    height = std::stoi(text.substr(x + 1));
    return width > 0 && height > 0;
}

/// The race starts of `track` as the game places them: each event of
/// gamedb's Events (other than free roam) at the gameplay object its
/// HorizonEventID names in Ribbon_00/GameObjs.xml, tagged with its career
/// style's activity_type and named from the Events string table.
std::vector<WorldMapMarker> raceStartMarkers(const fh::Vfs& vfs, const std::string& track)
{
    const std::string objectsPath = std::format("media/tracks/{}/Ribbon_00/GameObjs.xml", track);
    const fh::Bytes objects = vfs.readOrThrow(objectsPath);
    pugi::xml_document document;
    if (!document.load_buffer(objects.data(), objects.size())) {
        throw fh::DataError("{}: not XML", objectsPath);
    }
    std::unordered_map<std::string, glm::vec2> positions;
    for (const pugi::xml_node object : document.child("GameObjs").children()) {
        const pugi::xml_node pos = object.child("Pos");
        positions.emplace(object.attribute("GameplayID").as_string(),
            glm::vec2(pos.attribute("x").as_float(), pos.attribute("z").as_float()));
    }
    const fh::Database database(vfs.readOrThrow("media/db/gamedb.slt"));
    fh::Localisation strings(vfs, "en");
    std::vector<WorldMapMarker> markers;
    for (const fh::Row& row : database.query("SELECT HorizonEventID, CareerEventStyle, Name FROM Events")) {
        std::string id = row.text("HorizonEventID");
        while (!id.empty() && id.back() == ' ') {
            id.pop_back();
        }
        const std::string_view type = careerEventActivityType(row.i("CareerEventStyle"));
        const auto it = positions.find(id);
        if (type.empty() || it == positions.end()) {
            continue;
        }
        WorldMapMarker marker;
        marker.marker = {"activity_type", std::string(type), it->second, 0.0F};
        marker.name = strings.resolve("Events", row.text("Name"));
        markers.push_back(std::move(marker));
    }
    return markers;
}

struct MapOptions {
    std::string track = "colorado";
    glm::vec2 at{-1500.0F, 800.0F};
    float headingDegrees = 35.0F;
    /// Left stick held for the first seconds, then the right stick.
    glm::vec2 move{0.0F};
    float moveSeconds = 0.0F;
    float zoom = 0.0F;
    float zoomSeconds = 0.0F;
    std::string steps;
    float time = 1.0F;
    int width = 1280;
    int height = 720;
};

const char* mapActionName(WorldMapAction action)
{
    switch (action) {
    case WorldMapAction::None:
        return "none";
    case WorldMapAction::Back:
        return "back";
    case WorldMapAction::SetWaypoint:
        return "set waypoint";
    case WorldMapAction::ClearWaypoint:
        return "clear waypoint";
    }
    return "?";
}

int renderMap(const fh::Vfs& vfs, const std::string& out, const MapOptions& options)
{
    fh::Window window(hiddenWindow());
    OffscreenTarget target(options.width, options.height);
    WorldMap map(vfs, options.track, "en");
    WorldMapState state;
    state.playerPosition = options.at;
    state.playerHeading = glm::radians(options.headingDegrees);
    state.markers = raceStartMarkers(vfs, options.track);
    std::printf("%zu race starts\n", state.markers.size());
    map.open(state);
    constexpr float kStep = 1.0F / 60.0F;
    const auto frame = [&](const MenuInput& input) {
        const WorldMapAction action = map.update(input, kStep);
        if (action != WorldMapAction::None) {
            std::printf("action: %s\n", mapActionName(action));
        }
        target.clear();
        map.draw(options.width, options.height);
    };
    for (int i = 0; i < static_cast<int>(options.moveSeconds / kStep); ++i) {
        MenuInput input;
        input.leftStick = options.move;
        frame(input);
    }
    for (int i = 0; i < static_cast<int>(options.zoomSeconds / kStep); ++i) {
        MenuInput input;
        input.rightStick.y = options.zoom;
        frame(input);
    }
    for (const char step : options.steps) {
        frame(stepInput(step));
        for (int i = 0; i < 20; ++i) {
            frame(MenuInput{});
        }
    }
    for (int i = 0; i < std::max(1, static_cast<int>(options.time / kStep)); ++i) {
        frame(MenuInput{});
    }
    const WorldMapCamera& camera = map.camera();
    std::printf("cursor %.0f, %.0f  camera %.0f, %.0f  distance %.0f  zoom %.3f\n", camera.cursor().x,
        camera.cursor().y, camera.position().x, camera.position().y, camera.distance(), camera.zoom());
    if (const WorldMapMarker* hovered = map.hovered()) {
        std::printf("hovering %s (%s)\n", hovered->name.c_str(), hovered->marker.value.c_str());
    }
    if (const std::optional<glm::vec2> waypoint = map.waypoint()) {
        std::printf("waypoint %.0f, %.0f\n", waypoint->x, waypoint->y);
    }
    return writePng(out, target.read()) ? 0 : 1;
}

glm::vec2 parsePair(const std::string& text)
{
    const std::size_t comma = text.find(',');
    if (comma == std::string::npos) {
        throw std::runtime_error(std::format("{}: expected X,Y", text));
    }
    return {std::stof(text.substr(0, comma)), std::stof(text.substr(comma + 1))};
}

int renderLoading(const fh::Vfs& vfs, const std::string& out, const std::string& group, std::uint32_t seed,
    float seconds, int width, int height)
{
    fh::Window window(hiddenWindow());
    OffscreenTarget target(width, height);
    LoadingScreen screen(vfs, "en", seed);
    screen.show(group);
    constexpr float kStep = 1.0F / 60.0F;
    for (int i = 0; i < std::max(1, static_cast<int>(seconds / kStep)); ++i) {
        screen.update(kStep);
        target.clear();
        screen.draw(width, height);
    }
    if (const LoadingTip* tip = screen.tip()) {
        std::printf("tip: %s: %s\n", tip->title.c_str(), tip->message.c_str());
    }
    std::printf("background: %s\n", screen.background().c_str());
    return writePng(out, target.read()) ? 0 : 1;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        return usage();
    }
    try {
        fh::Vfs vfs;
        vfs.mountDisc(argv[1]);
        const std::string_view command = argv[2];
        const std::vector<std::string_view> args(argv + 3, argv + argc);
        if (command == "contracts" && !args.empty()) {
            Names names;
            for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
                if (args[i] != "--names") {
                    return usage();
                }
                names.load(std::string(args[i + 1]));
            }
            return dumpContracts(vfs, scenePath(args[0]), names);
        }
        if (command == "pause" && !args.empty()) {
            RenderOptions options;
            for (std::size_t i = 1; i < args.size(); ++i) {
                const std::string_view arg = args[i];
                const auto value = [&]() -> std::string {
                    if (i + 1 >= args.size()) {
                        throw std::runtime_error(std::format("{} needs a value", arg));
                    }
                    return std::string(args[++i]);
                };
                if (arg == "--race") {
                    options.race = true;
                } else if (arg == "--steps") {
                    options.steps = value();
                } else if (arg == "--time") {
                    options.time = std::stof(value());
                } else if (arg == "--size") {
                    if (!parseSize(value(), options.width, options.height)) {
                        return usage();
                    }
                } else {
                    return usage();
                }
            }
            return renderPause(vfs, std::string(args[0]), options);
        }
        if (command == "loading" && !args.empty()) {
            std::string group = "freeroam";
            std::uint32_t seed = 0;
            float seconds = 3.0F;
            int width = 1280;
            int height = 720;
            for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
                const std::string value(args[i + 1]);
                if (args[i] == "--group") {
                    group = value;
                } else if (args[i] == "--seed") {
                    seed = static_cast<std::uint32_t>(std::stoul(value));
                } else if (args[i] == "--time") {
                    seconds = std::stof(value);
                } else if (args[i] == "--size") {
                    if (!parseSize(value, width, height)) {
                        return usage();
                    }
                } else {
                    return usage();
                }
            }
            return renderLoading(vfs, std::string(args[0]), group, seed, seconds, width, height);
        }
        if (command == "map" && !args.empty()) {
            MapOptions options;
            for (std::size_t i = 1; i < args.size(); ++i) {
                const std::string_view arg = args[i];
                const auto value = [&]() -> std::string {
                    if (i + 1 >= args.size()) {
                        throw std::runtime_error(std::format("{} needs a value", arg));
                    }
                    return std::string(args[++i]);
                };
                if (arg == "--track") {
                    options.track = value();
                } else if (arg == "--at") {
                    options.at = parsePair(value());
                } else if (arg == "--heading") {
                    options.headingDegrees = std::stof(value());
                } else if (arg == "--move") {
                    options.move = parsePair(value());
                } else if (arg == "--move-time") {
                    options.moveSeconds = std::stof(value());
                } else if (arg == "--zoom") {
                    options.zoom = std::stof(value());
                } else if (arg == "--zoom-time") {
                    options.zoomSeconds = std::stof(value());
                } else if (arg == "--steps") {
                    options.steps = value();
                } else if (arg == "--time") {
                    options.time = std::stof(value());
                } else if (arg == "--size") {
                    if (!parseSize(value(), options.width, options.height)) {
                        return usage();
                    }
                } else {
                    return usage();
                }
            }
            return renderMap(vfs, std::string(args[0]), options);
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fh1menu: %s\n", e.what());
        return 1;
    }
}
