// Inspects the game's UI data and renders the HUD without the game: lists
// the vector fonts, dumps a scene's element tree, draws every glyph of a
// font, decodes a UI texture, and renders the HUD for sample states into a
// PNG through an offscreen OpenGL context.

#include "base/Error.h"
#include "base/Log.h"
#include "data/StringTable.h"
#include "platform/Window.h"
#include "render/Gl.h"
#include "ui/AnarkFormats.h"
#include "ui/Hud.h"
#include "ui/Presentation.h"
#include "ui/UiRenderer.h"
#include "ui/VectorFont.h"
#include "vfs/Vfs.h"

#include <glm/gtc/matrix_transform.hpp>
#include <xds/xds.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <format>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace fh::ui;

int usage()
{
    std::fprintf(stderr,
        "usage: fh1ui DISC fonts\n"
        "       fh1ui DISC scene PATH [--attributes]\n"
        "       fh1ui DISC glyphs OUT.png [--font FACE] [--size PX]\n"
        "       fh1ui DISC texture PATH OUT.png\n"
        "       fh1ui DISC string ID [LANGUAGE]\n"
        "       fh1ui DISC render OUT.png [--race] [--sprint] [--countdown S] [--prompt] [--assists]\n"
        "                                 [--size WxH] [--time S] [--rpm R] [--rpm-rate R]\n"
        "                                 [--speed S] [--metric] [--at X,Z] [--heading DEG]\n"
        "                                 [--track NAME] [--radio STATION[,TITLE[,ARTIST]]] [--radio-tuning]\n"
        "                                 [--marker KEY=VALUE@X,Z[,DEG]]... [--backdrop R,G,B]\n"
        "                                 [--line KEY=VALUE@X,Z;X,Z;...]... [--distance METRES]\n"
        "  DISC      the extracted disc (its root or its media folder)\n"
        "  PATH      a scene without extension, such as 947_HUD or\n"
        "            media/cars/<car>/digitalgauge/dash_<car>_p; a texture's game path\n"
        "  string    prints the text of a string id (such as IDS_Best) in every table that has it\n"
        "  render    draws the HUD for a sample free-roam state, or a circuit race with --race\n"
        "            (a point-to-point one with --sprint), --countdown S seconds into the start countdown,\n"
        "            the player at world X,Z facing DEG (0 north, 90 east), a radio panel request on the\n"
        "            first frame (station index, the off station last),\n"
        "            run for --time seconds (with the revs changing by --rpm-rate per second);\n"
        "            set SDL_VIDEO_DRIVER=offscreen to render without a window\n");
    return 2;
}

std::string scenePath(std::string_view path)
{
    if (path.find('/') == std::string_view::npos && path.find('\\') == std::string_view::npos) {
        return "media/ui/scenes/ui4/" + std::string(path);
    }
    return std::string(path);
}

bool writeFile(const std::string& path, const std::vector<std::uint8_t>& data)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

int listFonts(const fh::Vfs& vfs)
{
    FontLibrary library(vfs, "en");
    for (const std::string& target : library.targets()) {
        const std::string file = library.fileFor(target);
        const std::optional<fh::Bytes> data = vfs.read(file);
        if (!data) {
            std::printf("%-4s %-40s missing\n", target.c_str(), file.c_str());
            continue;
        }
        try {
            const VectorFont font = VectorFont::parse(*data, file);
            std::size_t triangles = 0;
            for (const auto& [code, glyph] : font.glyphs()) {
                triangles += glyph.triangles.size() / 3;
            }
            std::printf("%-4s %-40s %4zu glyphs %7zu triangles  line %.3f em  caps %.3f em  space %.3f em\n",
                target.c_str(), file.c_str(), font.glyphs().size(), triangles, static_cast<double>(font.lineHeight()),
                static_cast<double>(font.capHeight()), static_cast<double>(font.spaceAdvance()));
        } catch (const fh::DataError& e) {
            std::printf("%-4s %-40s error: %s\n", target.c_str(), file.c_str(), e.what());
        }
    }
    return 0;
}

const char* kindName(const Presentation& p, int element)
{
    const std::uint32_t id = p.files().bgf.elements[static_cast<std::size_t>(element)].id;
    for (const FbfRecord& record : p.files().fbf.records) {
        if (record.id != id) {
            continue;
        }
        switch (record.kind) {
        case FbfKind::Group:
            return p.kind(element) == ElementKind::Component ? "component" : "group";
        case FbfKind::Text:
            return "text";
        case FbfKind::Camera:
            return "camera";
        case FbfKind::Layer:
            return "layer";
        case FbfKind::Model:
            return "model";
        case FbfKind::Material:
            return "material";
        case FbfKind::Light:
            return "light";
        case FbfKind::Image:
            return "image";
        }
    }
    return p.kind(element) == ElementKind::Behavior ? "behaviour" : "element";
}

int dumpScene(const fh::Vfs& vfs, const std::string& path, bool attributes)
{
    const Presentation p(SceneFiles::load(vfs, path));
    const BgfFile& bgf = p.files().bgf;
    std::printf("%s: %dx%d, %zu elements, %zu slides, %zu animation tracks, %zu meshes\n", path.c_str(), bgf.width,
        bgf.height, bgf.elements.size(), bgf.slides.size(), bgf.tracks.size(), p.files().fbf.meshes.size());
    const std::vector<DrawItem> items = p.evaluate();
    const std::function<void(int, int)> print = [&](int element, int depth) {
        if (p.kind(element) == ElementKind::Behavior) {
            return;
        }
        const glm::mat4 world = p.worldTransform(element);
        std::string line = std::format("{:>{}}[{}] {} \"{}\" at ({:.1f}, {:.1f}, {:.1f})", "", depth * 2, element,
            kindName(p, element), p.name(element), world[3].x, world[3].y, world[3].z);
        if (const auto opacity = p.attribute(element, attr::Opacity); opacity && *opacity < 100.0F) {
            line += std::format(" opacity {:.0f}", *opacity);
        }
        if (!p.present(element)) {
            line += " (not shown)";
        }
        const std::vector<std::string> slides = p.slideNames(element);
        if (slides.size() > 1) {
            line += " slides:";
            for (std::size_t i = 1; i < slides.size(); ++i) {
                line += (slides[i] == p.currentSlide(element) ? " *" : " ") + slides[i];
            }
        }
        const std::string text = p.text(element);
        if (!text.empty()) {
            line += std::format(" text \"{}\"", text);
        }
        for (const FbfRecord& record : p.files().fbf.records) {
            if (record.id == bgf.elements[static_cast<std::size_t>(element)].id) {
                if (record.kind == FbfKind::Image) {
                    line += " " + record.image.path;
                } else if (record.kind == FbfKind::Text) {
                    line += std::format(" font {} size {}", record.text.font, record.text.size);
                } else if (record.kind == FbfKind::Material) {
                    static constexpr std::array<const char*, 5> kBlend{
                        "zero", "one", "srcalpha", "invsrcalpha", "other"};
                    const glm::vec4& d = record.material.diffuse;
                    line += std::format(" diffuse ({:.2f} {:.2f} {:.2f} {:.2f}) blend {}/{}", d.r, d.g, d.b, d.a,
                        kBlend[static_cast<std::size_t>(record.material.sourceBlend)],
                        kBlend[static_cast<std::size_t>(record.material.destinationBlend)]);
                }
                break;
            }
        }
        std::printf("%s\n", line.c_str());
        if (attributes) {
            for (const Attribute& a : bgf.elements[static_cast<std::size_t>(element)].attributes) {
                std::printf("%*s  0x%07x type %u = %g (0x%08x)\n", depth * 2, "", a.hash, a.type,
                    static_cast<double>(a.asFloat()), a.raw);
            }
        }
        for (const int child : p.children(element)) {
            print(child, depth + 1);
        }
    };
    for (std::size_t i = 0; i < p.elementCount(); ++i) {
        if (p.parent(static_cast<int>(i)) < 0) {
            print(static_cast<int>(i), 0);
        }
    }
    return 0;
}

int dumpTexture(const fh::Vfs& vfs, const std::string& path, const std::string& out)
{
    const std::string file = path.ends_with(".xds") ? fh::Vfs::normalize(path) : texturePathForScene(path);
    const fh::Bytes data = vfs.readOrThrow(file);
    const xds::Texture texture = xds::Texture::parse(std::span<const std::uint8_t>(data));
    std::printf("%s: %s\n", file.c_str(), xds::describe(texture.info()).c_str());
    return writeFile(out, xds::writePng(texture.decodeLevel(0))) ? 0 : 1;
}

/// A hidden window for the context, and an RGBA8 framebuffer to draw into.
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
    }
    ~OffscreenTarget()
    {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteTextures(1, &m_colour);
    }
    OffscreenTarget(const OffscreenTarget&) = delete;
    OffscreenTarget& operator=(const OffscreenTarget&) = delete;

    /// A backdrop standing in for the game's picture: a sky-to-road
    /// gradient, so light and dark HUD parts both show.
    void clear() const
    {
        const float top[4] = {0.42F, 0.55F, 0.66F, 1.0F};
        glClearNamedFramebufferfv(m_framebuffer, GL_COLOR, 0, top);
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
    options.title = "fh1ui";
    options.width = 64;
    options.height = 64;
    options.hidden = true;
    options.vsync = false;
    options.glDebug = std::getenv("FH1_GL_DEBUG") != nullptr;
    return options;
}

/// Draws the ground half of the backdrop so the HUD is seen over both a
/// light and a dark picture.
void drawBackdrop(UiRenderer& renderer, int width, int height, const std::optional<glm::vec3>& flat)
{
    static const Mesh quad = [] {
        Mesh mesh;
        mesh.vertices = {{{-0.5F, -0.5F, 0.0F}, {}, {0.0F, 0.0F}}, {{0.5F, -0.5F, 0.0F}, {}, {1.0F, 0.0F}},
            {{0.5F, 0.5F, 0.0F}, {}, {1.0F, 1.0F}}, {{-0.5F, 0.5F, 0.0F}, {}, {0.0F, 1.0F}}};
        mesh.indices = {0, 1, 2, 0, 2, 3};
        return mesh;
    }();
    renderer.begin(width, height);
    if (flat) {
        DrawModel fill;
        fill.mesh = &quad;
        fill.color = glm::vec4(*flat, 1.0F);
        fill.world = glm::scale(glm::mat4(1.0F), glm::vec3(4000.0F, 4000.0F, 1.0F));
        renderer.drawModel(fill);
        renderer.end();
        return;
    }
    DrawModel ground;
    ground.mesh = &quad;
    ground.color = glm::vec4(0.18F, 0.2F, 0.17F, 1.0F);
    ground.world
        = glm::scale(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -180.0F, 0.0F)), glm::vec3(4000.0F, 360.0F, 1.0F));
    renderer.drawModel(ground);
    DrawModel road;
    road.mesh = &quad;
    road.color = glm::vec4(0.3F, 0.3F, 0.31F, 1.0F);
    road.world
        = glm::scale(glm::translate(glm::mat4(1.0F), glm::vec3(0.0F, -250.0F, 0.0F)), glm::vec3(700.0F, 220.0F, 1.0F));
    renderer.drawModel(road);
    renderer.end();
}

/// Appends a code point below U+0800 as UTF-8.
void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out.push_back(static_cast<char>(c));
    } else {
        out.push_back(static_cast<char>(0xC0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
    }
}

int renderGlyphs(const fh::Vfs& vfs, const std::string& out, const std::string& face, float size)
{
    fh::Window window(hiddenWindow());
    FontLibrary fonts(vfs, "en");
    const VectorFont* font = fonts.font(face);
    if (!font) {
        std::fprintf(stderr, "no font for %s\n", face.c_str());
        return 1;
    }
    std::vector<std::string> rows;
    std::string row;
    int column = 0;
    std::size_t missing = 0;
    for (char32_t c = 0x20; c <= 0xFF; ++c) {
        if (c >= 0x7F && c < 0xA0) {
            continue;
        }
        if (!font->glyph(c) && c != U' ' && c != 0xA0) {
            ++missing;
        }
        appendUtf8(row, c);
        row.push_back(' ');
        if (++column == 32) {
            rows.push_back(row);
            row.clear();
            column = 0;
        }
    }
    if (!row.empty()) {
        rows.push_back(row);
    }
    const int width = static_cast<int>(size * 40.0F);
    const int height = static_cast<int>(size * 1.6F * static_cast<float>(rows.size() + 1));
    OffscreenTarget target(width, height);
    target.clear();
    UiRenderer renderer(vfs, fonts);
    renderer.begin(width, height, static_cast<float>(width), static_cast<float>(height));
    float y = static_cast<float>(height) * 0.5F - size * 1.2F;
    for (const std::string& line : rows) {
        DrawText text;
        text.text = line;
        text.font = face;
        text.size = size;
        text.horizontalAlign = 0;
        text.verticalAlign = 0;
        text.color = glm::vec4(1.0F);
        text.world
            = glm::translate(glm::mat4(1.0F), glm::vec3(-static_cast<float>(width) * 0.5F + size * 0.5F, y, 0.0F));
        renderer.drawText(text);
        y -= size * 1.6F;
    }
    renderer.end();
    std::printf("%s: %zu glyphs, %zu of ASCII and Latin-1 missing (drawn as the fallback)\n", face.c_str(),
        font->glyphs().size(), missing);
    return writeFile(out, xds::writePng(target.read())) ? 0 : 1;
}

struct RenderOptions {
    bool race = false;
    bool prompt = false;
    bool sprint = false;
    bool assists = false;
    /// Seconds into the start countdown at the last frame; negative for none.
    float countdown = -1.0F;
    float time = 1.0F;
    float rpm = 5200.0F;
    float rpmRate = 0.0F;
    float speed = 87.0F;
    bool metric = false;
    std::string track = "colorado";
    /// A radio panel request on the first frame.
    std::optional<HudRadioRequest> radio;
    bool radioTuning = false;
    std::vector<HudMapMarker> markers;
    std::vector<HudMapLine> lines;
    std::optional<float> distance;
    /// A flat backdrop colour (display values 0 to 1) instead of the scene.
    std::optional<glm::vec3> backdrop;
    glm::vec2 at{-1500.0F, 800.0F};
    float headingDegrees = 35.0F;
    int width = 1280;
    int height = 720;
};

HudState sampleState(const RenderOptions& options)
{
    HudState state;
    state.metric = options.metric;
    state.speedMph = options.metric ? options.speed / 1.609344F : options.speed;
    state.speedKmh = state.speedMph * 1.609344F;
    state.gear = 4;
    state.rpm = options.rpm;
    state.redlineRpm = 7200.0F;
    state.maxRpm = 8000.0F;
    state.absOn = options.assists;
    state.tcsOn = options.assists;
    state.stmOn = options.assists;
    state.playerMapPosition = options.at;
    state.playerHeading = glm::radians(options.headingDegrees);
    if (options.prompt) {
        state.prompt = "COLORADO SPRINT";
        state.promptDetail = "SIGN ME UP!";
        state.promptIcon = "DEFAULT";
    }
    if (options.race || options.sprint || options.countdown >= 0.0F) {
        state.inRace = true;
        state.position = 3;
        state.racers = 8;
        state.laps = options.sprint ? 1 : 3;
        state.lap = options.sprint ? 1 : 2;
        state.raceTime = 83.456F;
        if (!options.sprint) {
            state.lapTime = 22.249F;
            state.lastLapTime = 61.207F;
            state.bestLapTime = 61.207F;
        }
        state.standings = {"D. RIVAL", "ALEX", "YOU", "SAM", "JORDAN", "CASEY", "RILEY", "MORGAN"};
        state.playerStanding = 2;
        if (options.countdown >= 0.0F) {
            state.speedMph = 0.0F;
            state.speedKmh = 0.0F;
            state.gear = 1;
            state.rpm = 1100.0F;
            state.raceTime = 0.0F;
            state.lap = 1;
            state.lapTime.reset();
            state.lastLapTime.reset();
            state.bestLapTime.reset();
        }
    }
    state.speedMetresPerSecond = state.speedKmh / 3.6F;
    state.radioTuning = options.radioTuning;
    state.mapMarkers = options.markers;
    state.mapLines = options.lines;
    state.destinationDistance = options.distance;
    return state;
}

int renderHud(const fh::Vfs& vfs, const std::string& out, const RenderOptions& options)
{
    fh::Window window(hiddenWindow());
    OffscreenTarget target(options.width, options.height);
    target.clear();
    FontLibrary fonts(vfs, "en");
    UiRenderer backdrop(vfs, fonts);
    Hud hud(vfs, options.track, "en");
    HudState state = sampleState(options);
    // Run the HUD for a while so animations settle as they would in play.
    constexpr float kStep = 1.0F / 60.0F;
    const int frames = std::max(1, static_cast<int>(options.time / kStep));
    double hudSeconds = 0.0;
    for (int frame = 0; frame < frames; ++frame) {
        state.dt = kStep;
        state.radio = frame == 0 ? options.radio : std::nullopt;
        state.rpm += options.rpmRate * kStep;
        if (options.countdown >= 0.0F) {
            state.countdownElapsed = options.countdown - kStep * static_cast<float>(frames - 1 - frame);
        }
        target.clear();
        drawBackdrop(backdrop, options.width, options.height, options.backdrop);
        const auto start = std::chrono::steady_clock::now();
        hud.draw(state, options.width, options.height);
        glFinish();
        hudSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }
    std::printf("%d frames, %.3f ms per HUD frame (CPU and GPU)\n", frames, hudSeconds * 1000.0 / frames);
    return writeFile(out, xds::writePng(target.read())) ? 0 : 1;
}

int findString(const fh::Vfs& vfs, std::string_view id, const std::string& language)
{
    const std::uint16_t key = stringIdHash(id);
    const std::string folder = "media/stringtables/" + language;
    int found = 0;
    for (const std::string& path : vfs.list(folder)) {
        if (!fh::endsWithNoCase(path, ".str")) {
            continue;
        }
        const std::optional<fh::Bytes> data = vfs.read(path);
        if (!data) {
            continue;
        }
        const fh::StringTable table = fh::StringTable::parse(*data, path);
        if (const std::optional<std::string> text = table.get(key)) {
            std::printf("%s: %s\n", path.c_str(), text->c_str());
            ++found;
        }
    }
    return found > 0 ? 0 : 1;
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
        std::vector<std::string_view> args(argv + 3, argv + argc);
        if (command == "fonts") {
            return listFonts(vfs);
        }
        if (command == "scene" && !args.empty()) {
            const bool attributes = args.size() > 1 && args[1] == "--attributes";
            return dumpScene(vfs, scenePath(args[0]), attributes);
        }
        if (command == "string" && !args.empty()) {
            return findString(vfs, args[0], args.size() > 1 ? std::string(args[1]) : std::string("en"));
        }
        if (command == "texture" && args.size() == 2) {
            return dumpTexture(vfs, std::string(args[0]), std::string(args[1]));
        }
        if (command == "glyphs" && !args.empty()) {
            std::string face = "horizon_e";
            float size = 48.0F;
            for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
                if (args[i] == "--font") {
                    face = args[i + 1];
                } else if (args[i] == "--size") {
                    size = std::stof(std::string(args[i + 1]));
                } else {
                    return usage();
                }
            }
            return renderGlyphs(vfs, std::string(args[0]), face, size);
        }
        if (command == "render" && !args.empty()) {
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
                } else if (arg == "--sprint") {
                    options.sprint = true;
                } else if (arg == "--assists") {
                    options.assists = true;
                } else if (arg == "--at") {
                    const std::string at = value();
                    const std::size_t comma = at.find(',');
                    if (comma == std::string::npos) {
                        return usage();
                    }
                    options.at = {std::stof(at.substr(0, comma)), std::stof(at.substr(comma + 1))};
                } else if (arg == "--radio") {
                    // STATION[,TITLE[,ARTIST]]
                    const std::string spec = value();
                    HudRadioRequest request;
                    const std::size_t first = spec.find(',');
                    request.station = std::stoi(spec.substr(0, first));
                    if (first != std::string::npos) {
                        const std::size_t second = spec.find(',', first + 1);
                        request.title = spec.substr(
                            first + 1, second == std::string::npos ? std::string::npos : second - first - 1);
                        if (second != std::string::npos) {
                            request.artist = spec.substr(second + 1);
                        }
                    }
                    options.radio = request;
                } else if (arg == "--marker") {
                    // KEY=VALUE@X,Z[,HEADING_DEG]
                    const std::string spec = value();
                    const std::size_t equals = spec.find('=');
                    const std::size_t at = spec.find('@');
                    const std::size_t comma = spec.find(',', at);
                    if (equals == std::string::npos || at == std::string::npos || comma == std::string::npos) {
                        return usage();
                    }
                    HudMapMarker marker;
                    marker.key = spec.substr(0, equals);
                    marker.value = spec.substr(equals + 1, at - equals - 1);
                    const std::size_t second = spec.find(',', comma + 1);
                    marker.position = {std::stof(spec.substr(at + 1, comma - at - 1)),
                        std::stof(spec.substr(
                            comma + 1, second == std::string::npos ? std::string::npos : second - comma - 1))};
                    if (second != std::string::npos) {
                        marker.heading = glm::radians(std::stof(spec.substr(second + 1)));
                    }
                    options.markers.push_back(marker);
                } else if (arg == "--line") {
                    // KEY=VALUE@X,Z;X,Z;...
                    const std::string spec = value();
                    const std::size_t equals = spec.find('=');
                    const std::size_t at = spec.find('@');
                    if (equals == std::string::npos || at == std::string::npos) {
                        return usage();
                    }
                    HudMapLine line;
                    line.key = spec.substr(0, equals);
                    line.value = spec.substr(equals + 1, at - equals - 1);
                    std::size_t start = at + 1;
                    while (start < spec.size()) {
                        const std::size_t end = spec.find(';', start);
                        const std::string point
                            = spec.substr(start, end == std::string::npos ? std::string::npos : end - start);
                        const std::size_t comma = point.find(',');
                        if (comma == std::string::npos) {
                            return usage();
                        }
                        line.points.emplace_back(std::stof(point.substr(0, comma)), std::stof(point.substr(comma + 1)));
                        start = end == std::string::npos ? spec.size() : end + 1;
                    }
                    options.lines.push_back(line);
                } else if (arg == "--distance") {
                    options.distance = std::stof(value());
                } else if (arg == "--backdrop") {
                    // R,G,B in 0-255
                    const std::string rgb = value();
                    const std::size_t a = rgb.find(',');
                    const std::size_t b = rgb.find(',', a + 1);
                    if (a == std::string::npos || b == std::string::npos) {
                        return usage();
                    }
                    options.backdrop = glm::vec3(std::stof(rgb.substr(0, a)), std::stof(rgb.substr(a + 1, b - a - 1)),
                                           std::stof(rgb.substr(b + 1)))
                        / 255.0F;
                } else if (arg == "--radio-tuning") {
                    options.radioTuning = true;
                } else if (arg == "--track") {
                    options.track = value();
                } else if (arg == "--heading") {
                    options.headingDegrees = std::stof(value());
                } else if (arg == "--prompt") {
                    options.prompt = true;
                } else if (arg == "--countdown") {
                    options.countdown = std::stof(value());
                } else if (arg == "--time") {
                    options.time = std::stof(value());
                } else if (arg == "--rpm") {
                    options.rpm = std::stof(value());
                } else if (arg == "--rpm-rate") {
                    options.rpmRate = std::stof(value());
                } else if (arg == "--speed") {
                    options.speed = std::stof(value());
                } else if (arg == "--metric") {
                    options.metric = true;
                } else if (arg == "--size") {
                    const std::string size = value();
                    const std::size_t x = size.find('x');
                    if (x == std::string::npos) {
                        return usage();
                    }
                    options.width = std::stoi(size.substr(0, x));
                    options.height = std::stoi(size.substr(x + 1));
                } else {
                    return usage();
                }
            }
            return renderHud(vfs, std::string(args[0]), options);
        }
        return usage();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fh1ui: %s\n", e.what());
        return 1;
    }
}
