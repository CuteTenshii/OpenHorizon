// Tests of the pause menu, the full-screen map and the title screen: the
// text wrapping, the map camera and the title prompt's timing on values
// built here, and, when FH1_GAME_DIR names an extracted disc, the pause
// menu's entries for free roam and races with their texts from the string
// tables, the structure of the scenes the screens bind, the map's tunables
// and cursor area, and the title's movie and music. The loading screen's
// cases draw it in an offscreen OpenGL context and skip without one.

#include "support/GlContext.h"
#include "support/TestSupport.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/Fev.h"
#include "base/Bytes.h"
#include "base/Host.h"
#include "platform/Window.h"
#include "ui/AnarkFormats.h"
#include "ui/EventScreens.h"
#include "ui/LoadingScreen.h"
#include "ui/MenuCommon.h"
#include "ui/Minimap.h"
#include "ui/MovieRenderer.h"
#include "ui/PauseMenu.h"
#include "ui/Presentation.h"
#include "ui/TitleScreen.h"
#include "ui/UiRenderer.h"
#include "ui/UiSounds.h"
#include "ui/VectorFont.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"

#include <glm/glm.hpp>
#include <xds/xds.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using Catch::Matchers::WithinAbs;

using fh::test::gameDir;

namespace {

using namespace fh::ui;

/// The shipped UI/Map values, so the camera tests read like the game.
WorldMapTuning shippedTuning()
{
    WorldMapTuning tuning;
    tuning.distanceMin = 500.0F;
    tuning.distanceMax = 7652.0F;
    tuning.cursorSpeedMin = 900.0F;
    tuning.cursorSpeedMax = 10000.0F;
    tuning.cursorZoomMin = 0.1F;
    tuning.cursorZoomMax = 0.38F;
    tuning.zoomSpeed = 1.2F;
    tuning.zoomDefault = 0.38F;
    tuning.inputAccelerationStart = 1.0F;
    tuning.inputAccelerationTime = 1.0F;
    tuning.inputLinearity = 2.0F;
    tuning.positionSpring = 100.0F;
    tuning.positionDamping = 20.0F;
    tuning.zoomSpring = 300.0F;
    tuning.zoomDamping = 40.0F;
    tuning.deadArea = 0.4F;
    tuning.viewEdgeLeft = -7400.0F;
    tuning.viewEdgeRight = 7700.0F;
    tuning.viewEdgeTop = 4600.0F;
    tuning.viewEdgeBottom = -6900.0F;
    tuning.contextSnapSize = 0.02F;
    return tuning;
}

TEST_CASE("wrap_breaks_at_spaces_within_the_width", "[menu]")
{
    const auto measure = [](const std::string& text) { return static_cast<float>(text.size()); };
    CHECK(wrapText("one two three", 7.0F, measure) == std::string("one two\nthree"));
    CHECK(wrapText("one\ntwo three", 20.0F, measure) == std::string("one\ntwo three"));
    CHECK(wrapText("unbreakable word", 4.0F, measure) == std::string("unbreakable\nword"));
    CHECK(wrapText("", 10.0F, measure).empty());
}

TEST_CASE("map_camera_opens_on_the_player_at_the_default_zoom", "[menu]")
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({-1500.0F, 800.0F});
    CHECK_THAT(camera.zoom(), WithinAbs(0.38, 1e-6));
    CHECK_THAT(camera.distance(), WithinAbs(500.0 + 7152.0 * 0.38, 1e-2));
    CHECK_THAT(camera.cursor().x, WithinAbs(-1500.0, 1e-3));
    CHECK_THAT(camera.position().y, WithinAbs(800.0, 1e-3));
}

TEST_CASE("map_camera_keeps_the_view_inside_the_edges", "[menu]")
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({6000.0F, 3500.0F});
    // At the default zoom the view is 2 x 3218 m tall and 16/9 of that
    // wide, so its centre stays 5721 m inside the right edge and 3218 m
    // below the top.
    const float distance = camera.distance();
    CHECK_THAT(camera.position().x, WithinAbs(7700.0 - distance * 16.0 / 9.0, 1e-2));
    CHECK_THAT(camera.position().y, WithinAbs(4600.0 - distance, 1e-2));
    // The cursor itself stays where it was put, inside the cursor area.
    CHECK_THAT(camera.cursor().x, WithinAbs(6000.0, 1e-3));
}

TEST_CASE("map_cursor_moves_with_the_shaped_stick", "[menu]")
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({0.0F, 0.0F});
    // Half deflection squared (Input_Linearity 2) at the default zoom's
    // speed of 900 + 9100 * 0.38 m/s, for one 60th of a second.
    camera.update({0.5F, 0.0F}, 0.0F, 1.0F / 60.0F);
    CHECK_THAT(camera.cursor().x, WithinAbs(0.25 * (900.0 + 9100.0 * 0.38) / 60.0, 1e-2));
    CHECK_THAT(camera.cursor().y, WithinAbs(0.0, 1e-6));
}

TEST_CASE("map_camera_follows_only_past_the_dead_area", "[menu]")
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({0.0F, 0.0F});
    // A small move stays inside 0.4 times the distance: the camera stays.
    for (int i = 0; i < 30; ++i) {
        camera.update({0.3F, 0.0F}, 0.0F, 1.0F / 60.0F);
    }
    CHECK(camera.cursor().x > 0.0F);
    CHECK(camera.cursor().x < 0.4F * camera.distance());
    CHECK_THAT(camera.position().x, WithinAbs(0.0, 1e-3));
    // Far enough and the camera follows, trailing by the dead area once
    // its spring settles (short of the view-edge clamp, 1979 m east).
    for (int i = 0; i < 20; ++i) {
        camera.update({1.0F, 0.0F}, 0.0F, 1.0F / 60.0F);
    }
    for (int i = 0; i < 600; ++i) {
        camera.update({0.0F, 0.0F}, 0.0F, 1.0F / 60.0F);
    }
    CHECK_THAT(camera.position().x, WithinAbs(camera.cursor().x - 0.4 * camera.distance(), 1.0));
}

TEST_CASE("map_zoom_stays_within_the_cursor_zoom_limits", "[menu]")
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({0.0F, 0.0F});
    for (int i = 0; i < 300; ++i) {
        camera.update({0.0F, 0.0F}, 1.0F, 1.0F / 60.0F);
    }
    CHECK_THAT(camera.zoom(), WithinAbs(0.1, 1e-6));
    CHECK_THAT(camera.distance(), WithinAbs(500.0 + 7152.0 * 0.1, 1.0));
    for (int i = 0; i < 300; ++i) {
        camera.update({0.0F, 0.0F}, -1.0F, 1.0F / 60.0F);
    }
    CHECK_THAT(camera.zoom(), WithinAbs(0.38, 1e-6));
}

bool fires(const TitleFlow::Step& step, std::string_view event)
{
    return std::find(step.events.begin(), step.events.end(), event) != step.events.end();
}

TEST_CASE("movie_definitions_select_streams_by_number", "[menu]")
{
    const MovieStreams title
        = MovieStreams::parse("<xml>\n<StreamDefinition>\n<Video id=\"2\" />\n</StreamDefinition>\n</xml>", "t");
    CHECK(title.video == 2);
    CHECK(title.audio.empty());
    const MovieStreams fmv = MovieStreams::parse(
        "<xml><StreamDefinition><Audio id=\"6\" lang=\"EN\"/><Audio id=\"1\" lang=\"FR\"/><Video id=\"7\" />"
        "</StreamDefinition></xml>",
        "f");
    CHECK(fmv.video == 7);
    CHECK(fmv.audio.size() == std::size_t{2});
    CHECK(fmv.audio[0].id == 6);
    CHECK(fmv.audio[0].language == std::string("EN"));
}

TEST_CASE("title_prompt_fades_in_eight_updates_after_entering", "[menu]")
{
    TitleFlow flow;
    const TitleFlow::Step enter = flow.enter();
    CHECK(fires(enter, "CONTROLLER_ONLY"));
    CHECK(enter.startMusic);
    for (int i = 1; i < TitleFlow::kFadeInDelay; ++i) {
        CHECK(!fires(flow.update({}, 1.0F / 60.0F, false), "FADE_IN"));
    }
    CHECK(fires(flow.update({}, 1.0F / 60.0F, false), "FADE_IN"));
    CHECK(!fires(flow.update({}, 1.0F / 60.0F, false), "FADE_IN"));
}

TEST_CASE("title_prompt_starts_on_a_or_start_and_not_on_b", "[menu]")
{
    TitleFlow flow;
    flow.enter();
    MenuInput b;
    b.back = true;
    CHECK(flow.update(b, 0.1F, false).action == TitleAction::None);
    MenuInput start;
    start.start = true;
    const TitleFlow::Step pressed = flow.update(start, 0.1F, false);
    CHECK(pressed.action == TitleAction::Start);
    CHECK(fires(pressed, "START_OUTRO"));
    MenuInput a;
    a.accept = true;
    CHECK(flow.update(a, 0.1F, false).action == TitleAction::None);
    CHECK(fires(flow.resume(), "START_INTRO"));
    CHECK(flow.update(a, 0.1F, false).action == TitleAction::Start);
}

TEST_CASE("title_prompt_goes_to_the_attract_after_53_idle_seconds", "[menu]")
{
    TitleFlow flow;
    flow.enter();
    constexpr float kStep = 0.5F;
    float t = 0.0F;
    TitleFlow::Step step;
    while (!fires(step, "FADE_OUT")) {
        step = flow.update({}, kStep, false);
        t += kStep;
    }
    CHECK_THAT(t, WithinAbs(53.5, 1e-3));
    CHECK(step.stopMusic);
    MenuInput start;
    start.start = true;
    // Input is ignored while fading out; the attract needs both the Fader's
    // FADE_OUT_COMPLETE and the 2 s wait.
    CHECK(flow.update(start, kStep, false).action == TitleAction::None);
    CHECK(flow.update({}, kStep, true).action == TitleAction::None);
    CHECK(flow.update({}, kStep, false).action == TitleAction::None);
    CHECK(flow.update({}, kStep, false).action == TitleAction::Attract);
    CHECK(flow.inAttract());
    const TitleFlow::Step back = flow.returnFromAttract();
    CHECK(back.startMusic);
    for (int i = 1; i < TitleFlow::kReturnFadeInDelay; ++i) {
        CHECK(!fires(flow.update({}, kStep, false), "FADE_IN"));
    }
    CHECK(fires(flow.update({}, kStep, false), "FADE_IN"));
    CHECK(flow.update(start, kStep, false).action == TitleAction::Start);
}

bool plays(const TitleFlow::Step& step, std::string_view sound)
{
    return std::find(step.sounds.begin(), step.sounds.end(), sound) != step.sounds.end();
}

TEST_CASE("title_prompt_plays_its_slide_cues", "[menu]")
{
    // CSplash plays PressStartSlideOn on entering (0x82856418) and
    // PressStartSlideOff on Start (0x82856510).
    TitleFlow flow;
    const TitleFlow::Step enter = flow.enter();
    CHECK(plays(enter, "PressStartSlideOn"));
    CHECK(!plays(enter, "PressStartSlideOff"));
    CHECK(flow.update({}, 0.1F, false).sounds.empty());
    MenuInput start;
    start.start = true;
    const TitleFlow::Step pressed = flow.update(start, 0.1F, false);
    CHECK(plays(pressed, "PressStartSlideOff"));
    CHECK(flow.resume().sounds.empty());
}

TEST_CASE("ui_sound_table_matches_names_without_case", "[menu]")
{
    const UiSoundTable table = UiSoundTable::parse(R"(<ui4audio><defaults>
    <event play="Pause"                     group="UIInGame/UI/Nav"        cue="HUD_Pause" />
    <event play="MAP_AmbientBackgroundLoop" group="UIInGame/UI/HorizonMap" cue="MAP_AmbientBackgroundLoop" isLooping="true"/>
    <event play="SpeedTrapFlash"            group="General/SpeedCamera"    cue="ShutterSound" />
    <event play="VO_PopUp_OthersStorefront" ggroup="UIInGame/UI/Blank"     cue="Blank" />
  </defaults><overrides><override name="Showroom">
    <event play="Accept"                    group="UIInGame/UI/Blank"      cue="Blank" />
  </override></overrides></ui4audio>)",
        "test");
    CHECK(table.size() == std::size_t{2});
    const UiSound* pause = table.find("pause");
    CHECK(pause != nullptr);
    if (pause) {
        CHECK(pause->event == std::string("UI/Nav/HUD_Pause"));
        CHECK(!pause->looping);
    }
    const UiSound* map = table.find("Map_AmbientBackgroundLoop");
    CHECK((map != nullptr && map->looping));
    CHECK(table.find("SpeedTrapFlash") == nullptr);
    CHECK(table.find("VO_PopUp_OthersStorefront") == nullptr);
    CHECK(table.find("Accept") == nullptr);
    CHECK_THROWS_AS(UiSoundTable::parse("<ui4audio/>", "test"), std::exception);
}

TEST_CASE("title_idle_timer_holds_while_the_sign_in_step_has_the_input", "[menu]")
{
    TitleFlow flow;
    flow.enter();
    MenuInput start;
    start.start = true;
    flow.update(start, 1.0F, false);
    for (int i = 0; i < 120; ++i) {
        CHECK(!fires(flow.update({}, 1.0F, false), "FADE_OUT"));
    }
    CHECK_THAT(flow.idleSeconds(), WithinAbs(0.0, 1e-9));
}

std::vector<std::string> texts(const std::vector<PauseMenuEntry>& entries)
{
    std::vector<std::string> out;
    out.reserve(entries.size());
    for (const PauseMenuEntry& entry : entries) {
        out.push_back(entry.text);
    }
    return out;
}

TEST_CASE("disc_pause_entries_follow_the_builder", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    UiStrings strings(vfs, "en");
    PauseMenuContext freeRoam;
    CHECK(texts(pauseMenuEntries(freeRoam, strings))
        == std::vector<std::string>({"MAP", "MULTIPLAYER", "PHOTO MODE", "MY PROFILE", "QUIT"}));
    PauseMenuContext race;
    race.freeRoam = false;
    CHECK(texts(pauseMenuEntries(race, strings))
        == std::vector<std::string>(
            {"RESTART", "MAP", "MULTIPLAYER", "PHOTO MODE", "MY PROFILE", "QUIT RACE", "QUIT"}));
    PauseMenuContext tutorial;
    tutorial.firstTimeCareer = true;
    CHECK(texts(pauseMenuEntries(tutorial, strings)) == std::vector<std::string>({"RESUME", "QUIT"}));
    CHECK(strings.get("ScreenTitles", "IDS_Pause") == std::string("GAME PAUSED"));
    CHECK(!strings.has("CarChallenges", "IDS_PauseMenu_CarChallenges"));
}

TEST_CASE("disc_pause_scene_has_the_bound_controls", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/925_pause_menu"));
    const int root = scene.contract(0, "UIContract");
    const int menu = scene.contract(scene.contractParameter(root, "COMPONENT_OPTION_LIST"), "MENU");
    CHECK(menu >= 0);
    const int list = scene.contract(scene.contractParameter(menu, "COMPONENT_LIST"), "ANIMATED_SCROLLING_LIST");
    const int group = scene.contractParameter(list, "GROUP_ITEMS");
    int buttons = 0;
    for (const int child : scene.children(group)) {
        const int contract = scene.contract(child, "PAUSE_MENU_BUTTON");
        if (contract >= 0) {
            ++buttons;
            CHECK(scene.contractParameter(contract, "TEXT_BUTTON") >= 0);
            CHECK(scene.slideNames(child).size() == std::size_t{8});
        }
    }
    CHECK(buttons == 7);
    const int bar = scene.contract(scene.contractParameter(root, "COMPONENT_HELP_BUTTON_BAR"), "HELP_BUTTON_BAR");
    CHECK(scene.contractParameter(bar, "GROUP_BUTTONS") >= 0);
    // Every icon a help bar can show names a texture the disc has, but for
    // the table's Triggers.tga, which the disc lacks.
    for (int icon = 0; icon <= static_cast<int>(HelpIcon::Dpad); ++icon) {
        const auto help = static_cast<HelpIcon>(icon);
        CHECK(vfs.exists(helpIconTexture(help)) == (help != HelpIcon::Triggers));
    }

    const Presentation popup(SceneFiles::load(vfs, "media/ui/scenes/ui4/h_message_popup"));
    const int popupRoot = popup.contract(0, "UIContract");
    const int box = popup.contract(popup.contractParameter(popupRoot, "MESSAGE_BOX"), "MESSAGE_BOX");
    CHECK(popup.contractParameter(box, "TEXT_TITLE") >= 0);
    CHECK(popup.contractParameter(box, "TEXT_MESSAGE") >= 0);
}

TEST_CASE("disc_map_data_is_where_the_map_reads_it", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const WorldMapTuning tuning = WorldMapTuning::load(vfs);
    CHECK_THAT(tuning.distanceMax, WithinAbs(7652.0, 1e-3));
    CHECK_THAT(tuning.zoomDefault, WithinAbs(0.38, 1e-6));
    CHECK_THAT(tuning.viewEdgeBottom, WithinAbs(-6900.0, 1e-3));

    const Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/h_map"));
    const int root = scene.contract(0, "UIContract");
    const int overlay = scene.contract(scene.contractParameter(root, "COMPONENT_MAP_OVERLAY"), "MAP_OVERLAY");
    CHECK(scene.contractParameter(overlay, "COMPONENT_CURSOR") >= 0);
    CHECK(scene.contractParameter(overlay, "MODEL_MAP_VIEWPORT") >= 0);

    const NavFile nav = NavFile::parse(vfs.readOrThrow("media/tracks/colorado/colorado.nav"), "colorado.nav");
    int areas = 0;
    for (const NavWay& way : nav.ways) {
        if (way.tag("area_type") == "cursor_area") {
            ++areas;
            CHECK(way.points.size() == std::size_t{5});
        }
    }
    CHECK(areas == 1);
}

TEST_CASE("disc_loading_tips_resolve_from_the_string_tables", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    UiStrings strings(vfs, "en");
    const LoadingDefinitions definitions = LoadingDefinitions::load(vfs, strings);
    const LoadingTipGroup* freeRoam = definitions.find("freeroam");
    CHECK(freeRoam != nullptr);
    if (!freeRoam) {
        return;
    }
    CHECK(freeRoam->tipCount == 2);
    CHECK(freeRoam->random);
    bool raceEncounters = false;
    for (const LoadingTip& tip : freeRoam->tips) {
        CHECK_THAT(tip.displayTime, WithinAbs(8.0, 1e-6));
        CHECK(tip.layout == 1);
        if (tip.title == "RACE ENCOUNTERS") {
            raceEncounters = true;
            CHECK(tip.message.starts_with("See a fellow Horizon racer"));
        }
    }
    CHECK(raceEncounters);
    CHECK(definitions.find("stats") != nullptr);
    CHECK(definitions.find("stats")->tips.empty());

    const Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/2_c_ini_loading"));
    const int root = scene.contract(0, "UIContract");
    for (int i = 1; i <= 8; ++i) {
        CHECK(scene.contractParameter(root, std::format("TEXT_LINE_{}", i)) >= 0);
    }
    CHECK_THAT(textBoxWidth(scene, scene.contractParameter(root, "TEXT_LINE_1")), WithinAbs(0.385 * 1280.0, 1e-2));
    CHECK(scene.contractParameter(root, "MATERIAL_BACKGROUND") >= 0);
}

/// The UI textures the bars are drawn with, decoded once.
class DecodedTextures {
public:
    explicit DecodedTextures(const fh::Vfs& vfs)
        : m_vfs(vfs)
    {
    }

    const xds::Image* get(const std::string& path)
    {
        const auto it = m_images.find(path);
        if (it != m_images.end()) {
            return it->second ? &*it->second : nullptr;
        }
        std::optional<xds::Image> image;
        if (const std::optional<fh::Bytes> data = m_vfs.read(path)) {
            image = xds::Texture::parse(std::span<const std::uint8_t>(*data)).decodeLevel(0);
        }
        const auto& stored = m_images.emplace(path, std::move(image)).first->second;
        return stored ? &*stored : nullptr;
    }

private:
    const fh::Vfs& m_vfs;
    std::map<std::string, std::optional<xds::Image>> m_images;
};

/// How opaque `model` draws at the layer-space point `at`: its colour's
/// alpha times each layer's texel alpha, sampled as UiRenderer does (a
/// clamped layer is transparent outside its square). 0 off the mesh.
float opacityAt(const DrawModel& model, glm::vec2 at, DecodedTextures& textures)
{
    const glm::mat4 inverse = glm::inverse(model.world);
    const glm::vec4 local = inverse * glm::vec4(at, model.world[3].z, 1.0F);
    const std::vector<MeshVertex>& vertices = model.mesh->vertices;
    const std::vector<std::uint16_t>& indices = model.mesh->indices;
    for (std::size_t t = 0; t + 2 < indices.size(); t += 3) {
        const MeshVertex& a = vertices[indices[t]];
        const MeshVertex& b = vertices[indices[t + 1]];
        const MeshVertex& c = vertices[indices[t + 2]];
        const glm::vec2 ab = glm::vec2(b.position) - glm::vec2(a.position);
        const glm::vec2 ac = glm::vec2(c.position) - glm::vec2(a.position);
        const glm::vec2 ap = glm::vec2(local) - glm::vec2(a.position);
        const float area = ab.x * ac.y - ab.y * ac.x;
        if (area == 0.0F) {
            continue;
        }
        const float wb = (ap.x * ac.y - ap.y * ac.x) / area;
        const float wc = (ab.x * ap.y - ab.y * ap.x) / area;
        const float wa = 1.0F - wb - wc;
        constexpr float kEdge = -1e-4F;
        if (wa < kEdge || wb < kEdge || wc < kEdge) {
            continue;
        }
        const glm::vec3 uv(a.uv * wa + b.uv * wb + c.uv * wc, 1.0F);
        float opacity = model.color.a;
        for (const DrawModel::Layer& layer : model.layers) {
            const xds::Image* image = textures.get(layer.texture);
            if (image == nullptr) {
                return 0.0F;
            }
            glm::vec2 st(glm::dot(layer.uvRow0, uv), glm::dot(layer.uvRow1, uv));
            for (int axis = 0; axis < 2; ++axis) {
                const TextureWrap wrap = axis == 0 ? layer.wrapU : layer.wrapV;
                float& value = st[axis];
                if (wrap == TextureWrap::Clamp) {
                    if (value < 0.0F || value > 1.0F) {
                        return 0.0F;
                    }
                } else if (wrap == TextureWrap::Repeat) {
                    value -= std::floor(value);
                } else {
                    value = 1.0F - std::abs(value - 2.0F * std::floor(0.5F * value) - 1.0F);
                }
            }
            const auto x
                = std::min(static_cast<std::uint32_t>(st.x * static_cast<float>(image->width)), image->width - 1);
            const auto y
                = std::min(static_cast<std::uint32_t>(st.y * static_cast<float>(image->height)), image->height - 1);
            opacity
                *= static_cast<float>(image->rgba[(static_cast<std::size_t>(y) * image->width + x) * 4 + 3]) / 255.0F;
        }
        return opacity;
    }
    return 0.0F;
}

// The player's report: on the DISCOUNT SIGNS tip the text straddled the
// edges of its brush bars and the top of its first line was lost under the
// title's dark bar. Every line's capitals, from the baseline to the cap
// height and along the whole line, must lie on the opaque part of the bar
// the scene puts under that line.
TEST_CASE("disc_loading_tip_lines_sit_on_their_bars", "[menu]")
{
    const char* dir = gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    // Seed 33 draws the tip of the report first.
    LoadingScreen screen(vfs, "en", 33);
    screen.show("freeroam");
    CHECK((screen.tip() != nullptr && screen.tip()->title == "DISCOUNT SIGNS"));
    FontLibrary fonts(vfs, "en");
    DecodedTextures textures(vfs);
    constexpr float kStep = 1.0F / 60.0F;
    for (const auto& [width, height] : {std::array{1920, 1080}, std::array{1280, 720}}) {
        // Three seconds in, the tip is held at its full display.
        for (int frame = 0; frame < 180; ++frame) {
            screen.update(kStep);
            screen.draw(width, height);
        }
        const std::vector<LoadingTipLine> lines = screen.tipLines();
        CHECK(lines.size() == std::size_t{3});
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const LoadingTipLine& line = lines[i];
            const VectorFont* font = fonts.font(line.text.font);
            CHECK(font != nullptr);
            if (font == nullptr) {
                continue;
            }
            for (const TextLineLayout& laid : layoutText(*font, line.text)) {
                const float capTop = laid.baseline + font->capHeight() * line.text.size;
                for (const float x : {laid.x, laid.x + 0.5F * laid.width, laid.x + laid.width}) {
                    for (const float y : {laid.baseline, capTop}) {
                        const glm::vec4 at = line.text.world * glm::vec4(x, y, 0.0F, 1.0F);
                        const float opacity = opacityAt(line.bar, glm::vec2(at), textures);
                        if (opacity < 0.5F) {
                            std::fprintf(stderr, "  %dx%d line %zu \"%s\": (%.1f, %.1f) on its bar at %.2f\n", width,
                                height, i + 1, line.text.text.c_str(), static_cast<double>(x), static_cast<double>(y),
                                static_cast<double>(opacity));
                        }
                        CHECK(opacity >= 0.5F);
                    }
                }
            }
        }
    }
}

TEST_CASE("disc_loading_status_takes_the_place_of_please_wait", "[menu]")
{
    const char* dir = gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    LoadingScreen screen(vfs, "en", 0);
    screen.show("freeroam");
    CHECK(screen.message() == std::string("PLEASE WAIT"));
    screen.setStatus("Compiling shaders 120/480");
    CHECK(screen.message() == std::string("Compiling shaders 120/480"));
    // The next screen keeps it until it is cleared.
    screen.show("freeroam");
    CHECK(screen.message() == std::string("Compiling shaders 120/480"));
    screen.update(1.0F / 60.0F);
    screen.draw(1280, 720);
    screen.setStatus(std::nullopt);
    CHECK(screen.message() == std::string("PLEASE WAIT"));
}

TEST_CASE("disc_title_scene_binds_the_movie_and_the_prompt", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    UiStrings strings(vfs, "en");
    CHECK(strings.get("Main", "IDS_PressStart") == std::string("PRESS START"));

    Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/120_p_pre_splash"));
    int root = -1;
    for (const int child : scene.children(0)) {
        if (scene.kind(child) == ElementKind::Behavior && scene.contractParameter(child, "TEXT_PRESS_START") >= 0) {
            root = child;
        }
    }
    CHECK(root >= 0);
    CHECK(scene.name(scene.contractParameter(root, "TEXT_PRESS_START")) == std::string("TitleTapeText"));
    const int video = scene.contract(scene.contractParameter(root, "COMPONENT_VIDEO"), "CUSTREND_MOVIERENDERER");
    CHECK(video >= 0);
    CHECK(scene.contractFlag(video, "VIDEOLOOP").value_or(false));
    CHECK(scene.contractFlag(video, "VIDEOAUTOPLAY").value_or(false));
    const int viewport = scene.contractParameter(video, "MODEL_VIEWPORT");
    CHECK(scene.name(viewport) == std::string("loading_image"));
    const BgfFile& bgf = scene.files().bgf;
    std::string path;
    for (const Attribute& attribute : bgf.elements[static_cast<std::size_t>(video)].attributes) {
        if (attribute.hash == anarkHash("VIDEOPATH")) {
            path = bgf.strings[attribute.raw];
        }
    }
    CHECK(path == std::string("GAME:\\Media\\UI\\Videos\\PressStart.wmv"));
    for (const char* event : {"CONTROLLER_ONLY", "FADE_IN", "FADE_OUT", "START_OUTRO", "START_INTRO"}) {
        CHECK(scene.fireEvent(root, event));
    }
    CHECK(scene.slideNames(scene.find("Layer/Fader")).size() == 4);
}

TEST_CASE("disc_title_movie_is_the_wmv3_stream_its_definition_names", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const MovieStreams streams
        = MovieStreams::parse(vfs.readText("media/ui/videos/pressstart.def").value_or(""), "PressStart.def");
    CHECK(streams.video == 2);
    CHECK(streams.audio.empty());
    const MovieInfo info = MovieRenderer::probe(vfs, "GAME:\\Media\\UI\\Videos\\PressStart.wmv");
    CHECK(info.streamId == 2);
    CHECK(info.codec == std::string("wmv3"));
    CHECK(info.width == 1280);
    CHECK(info.height == 720);
    CHECK_THAT(info.duration, WithinAbs(53.93, 0.01));
    CHECK_THAT(info.firstFrameTime, WithinAbs(0.0, 1e-6));
}

TEST_CASE("disc_ui_sounds_resolve_to_uiingame_events", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const UiSoundTable table = UiSoundTable::load(vfs);
    CHECK(table.size() == std::size_t{754});
    fh::audio::AudioLibrary library(vfs);
    const auto project = library.project("Game:\\Media\\Audio\\UI\\UIInGame\\UIInGame.fev");
    const auto event = [&](std::string_view name) -> std::string {
        const UiSound* sound = table.find(name);
        return sound ? sound->event : std::string();
    };
    // The names the pause flow, the pause menu, the map and the title play.
    CHECK(event("Pause") == std::string("UI/Nav/HUD_Pause"));
    CHECK(event("UnPause") == std::string("UI/Nav/HUD_UnPause"));
    CHECK(event("Accept") == std::string("UI/Nav/Accept"));
    CHECK(event("Cancel") == std::string("UI/Nav/Back"));
    CHECK(event("Map_AmbientBackgroundLoop") == std::string("UI/HorizonMap/MAP_AmbientBackgroundLoop"));
    CHECK(event("PressStartSlideOn") == std::string("UI/Blank/Blank"));
    CHECK(event("PressStartSlideOff") == std::string("UI/Blank/Blank"));
    for (const std::string_view name : {"Pause", "UnPause", "Accept", "Cancel", "Map_AmbientBackgroundLoop",
             "PressStartSlideOn", "PressStartSlideOff"}) {
        CHECK(project->findEvent(event(name)) != nullptr);
    }

    // The title's music: SplashScreen loops the front end ambience, whose
    // one sound is the press start music.
    const UiSound* splash = table.find("SplashScreen");
    CHECK((splash != nullptr && splash->looping));
    CHECK(event("SplashScreen") == std::string("UI/FrontEnd/FE_Ambience"));
    const fh::audio::FevEvent* ambience = project->findEvent(event("SplashScreen"));
    CHECK(ambience != nullptr);
    if (ambience != nullptr) {
        CHECK(ambience->layers.size() == std::size_t{1});
        if (ambience->layers.size() == 1) {
            CHECK(ambience->layers[0].sounds.size() == std::size_t{1});
            if (ambience->layers[0].sounds.size() == 1) {
                CHECK(project->soundDefinition(ambience->layers[0].sounds[0].soundDefinition).name
                    == std::string("/FrontEnd/PressStart_Music"));
            }
        }
    }

    // Played on a device with no output: the pause sound sounds, a second
    // Accept restarts the first (the event allows one playback), the map's
    // loop stays started while RadioIsPlaying keeps its only sound silent.
    fh::audio::AudioDevice device{fh::audio::AudioDevice::Offline{}};
    UiSounds sounds(vfs, device.mixer(), library, fh::audio::kMasterBus);
    sounds.play("Pause");
    CHECK(sounds.isPlaying("Pause"));
    CHECK(device.mixer().activeVoiceCount() == std::size_t{1});
    device.render(48000 * 3);
    CHECK(device.mixer().activeVoiceCount() == std::size_t{0});
    CHECK(!sounds.isPlaying("pause"));
    sounds.play("Accept");
    sounds.play("Accept");
    device.render(256);
    CHECK(device.mixer().activeVoiceCount() == std::size_t{1});
    device.render(48000 * 3);
    sounds.play("Map_AmbientBackgroundLoop");
    sounds.setParameter("Map_AmbientBackgroundLoop", "RadioIsPlaying", 1.0F);
    device.render(4800);
    CHECK(sounds.isPlaying("Map_AmbientBackgroundLoop"));
    CHECK(device.mixer().activeVoiceCount() == std::size_t{0});
    sounds.setParameter("Map_AmbientBackgroundLoop", "RadioIsPlaying", 0.0F);
    CHECK(device.mixer().activeVoiceCount() == std::size_t{1});
    sounds.stop("Map_AmbientBackgroundLoop");
    CHECK(!sounds.isPlaying("Map_AmbientBackgroundLoop"));
    sounds.play("PressStartSlideOn");
    CHECK(sounds.played().size() == std::size_t{5});
    CHECK(sounds.played().back() == std::string("PressStartSlideOn"));
}

/// FR04 as gamedb has it, with its texts resolved: a festival race with a
/// ticket and a restriction.
EventScreenInfo festivalRaceInfo()
{
    EventScreenInfo info;
    info.name = "Ferrari Massimo";
    info.shortName = "Ferrari Massimo";
    info.careerEventStyle = 1;
    info.level = 6;
    info.description = "A description";
    info.restrictionDescription = "Must be Ferrari";
    info.laps = 3;
    info.ticket = "239";
    return info;
}

TEST_CASE("disc_event_titles_follow_the_name_rule", "[menu]")
{
    const char* dir = gameDir();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    UiStrings strings(vfs, "en");
    EventScreenInfo info = festivalRaceInfo();
    CHECK(eventTitle(info) == std::string("FERRARI MASSIMO"));
    // Past UI/EventTitleMaxLongNameLength (30) the short name stands in.
    info.name = "A name that runs well past thirty characters";
    info.shortName = "Short name";
    CHECK(eventTitle(info) == std::string("SHORT NAME"));
    info.name = "Caf\xC3\xA9 run";
    CHECK(eventTitle(info) == std::string("CAF\xC3\x89 RUN"));
    const std::vector<std::pair<int, std::string>> kinds{{1, "FESTIVAL RACE"}, {2, "STREET RACE"}, {3, "SHOWCASE"},
        {5, "STAR SHOWDOWN"}, {6, "FESTIVAL HEADLINE RACE"}, {0, ""}, {7, ""}};
    for (const auto& [style, text] : kinds) {
        info.careerEventStyle = style;
        CHECK(eventKindLine(info, strings) == text);
    }
    const std::vector<std::pair<int, std::string>> places{{1, "1ST"}, {2, "2ND"}, {3, "3RD"}, {4, "4TH"}, {11, "11TH"},
        {12, "12TH"}, {13, "13TH"}, {21, "21ST"}, {22, "22ND"}, {101, "101ST"}};
    for (const auto& [place, text] : places) {
        CHECK(placeOrdinal(place, strings) == text);
    }
}

TEST_CASE("disc_event_summary_shows_the_ticket_and_enters", "[menu]")
{
    const char* dir = gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    EventSummaryScreen screen(vfs, "en");
    screen.open(festivalRaceInfo());
    CHECK(screen.isOpen());
    CHECK(screen.text("TEXT_TITLE") == std::string("FESTIVAL RACE"));
    CHECK(screen.text("TEXT_CAR_RESTRICTIONS") == std::string("Must be Ferrari"));
    CHECK(screen.text("TEXT_RESTRICTION_TITLE") == std::string("ENTRY RESTRICTIONS:"));
    // The ticket is the event's own image in its style's folder.
    CHECK(!screen.ticket().empty());
    CHECK(vfs.exists(screen.ticket()));
    CHECK(screen.ticket().find("festivalraces") != std::string::npos);
    screen.update(MenuInput{}, 1.0F / 60.0F);
    screen.draw(1280, 720);
    CHECK(screen.helpExtents().size() == std::size_t{2});
    MenuInput back;
    back.back = true;
    CHECK(screen.update(back, 1.0F / 60.0F) == EventSummaryScreen::Action::Back);
    CHECK(!screen.isOpen());
    screen.open(festivalRaceInfo());
    MenuInput accept;
    accept.accept = true;
    CHECK(screen.update(accept, 1.0F / 60.0F) == EventSummaryScreen::Action::Enter);
}

TEST_CASE("disc_event_title_card_is_done_after_four_seconds", "[menu]")
{
    const char* dir = gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    EventTitleCard card(vfs, "en");
    card.show(festivalRaceInfo());
    CHECK(card.title() == std::string("FERRARI MASSIMO"));
    CHECK(card.subtitle() == std::string("FESTIVAL RACE"));
    constexpr float kStep = 1.0F / 60.0F;
    int doneFrame = -1;
    for (int frame = 0; frame < 300 && doneFrame < 0; ++frame) {
        if (card.update(kStep)) {
            doneFrame = frame;
        }
        card.draw(1280, 720);
    }
    // 4.0 s at 60 Hz: done on the 240th frame.
    CHECK((doneFrame >= 238 && doneFrame <= 240));
}

TEST_CASE("disc_pre_race_menu_starts_or_quits_after_its_box", "[menu]")
{
    const char* dir = gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    PreRaceMenu menu(vfs, "en");
    menu.open(festivalRaceInfo());
    std::vector<std::string> texts;
    for (const PreRaceEntry& entry : menu.entries()) {
        texts.push_back(entry.text);
    }
    CHECK(texts == std::vector<std::string>({"START RACE", "OPTIONS", "QUIT RACE"}));
    CHECK(menu.title() == std::string("FERRARI MASSIMO"));
    CHECK(menu.subtitle() == std::string("FESTIVAL RACE"));
    constexpr float kStep = 1.0F / 60.0F;
    menu.update(MenuInput{}, kStep);
    menu.draw(1280, 720);
    CHECK(menu.helpExtents().size() == std::size_t{1});
    MenuInput down;
    down.down = true;
    MenuInput accept;
    accept.accept = true;
    MenuInput back;
    back.back = true;
    menu.update(down, kStep);
    menu.update(down, kStep);
    CHECK(menu.focus() == 2);
    // The list does not wrap.
    menu.update(down, kStep);
    CHECK(menu.focus() == 2);
    CHECK(menu.update(accept, kStep) == PreRaceMenu::Action::None);
    CHECK(menu.confirming());
    menu.draw(1280, 720);
    // B takes the box down and leaves the menu up; A in the box quits.
    CHECK(menu.update(back, kStep) == PreRaceMenu::Action::None);
    CHECK((!menu.confirming() && menu.isOpen()));
    menu.update(accept, kStep);
    CHECK(menu.update(accept, kStep) == PreRaceMenu::Action::Quit);
    CHECK(!menu.isOpen());
    menu.open(festivalRaceInfo());
    CHECK(menu.update(accept, kStep) == PreRaceMenu::Action::Race);
    CHECK(!menu.isOpen());
}

TEST_CASE("disc_finish_position_goes_after_its_tape", "[menu]")
{
    const char* dir = gameDir();
    fh::test::requireGlContext();
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    PostRaceFinishedScreen screen(vfs, "en");
    screen.show(3, 1);
    CHECK(screen.title() == std::string("FINISHED"));
    CHECK(screen.position() == std::string("3RD PLACE"));
    constexpr float kStep = 1.0F / 60.0F;
    float doneAt = -1.0F;
    bool swoosh = false;
    for (int frame = 1; frame <= 600 && doneAt < 0.0F; ++frame) {
        const bool done = screen.update(kStep);
        for (const std::string& sound : screen.takeSounds()) {
            swoosh = swoosh || sound == "Placement_SwooshOut";
        }
        screen.draw(1280, 720);
        if (done) {
            doneAt = static_cast<float>(frame) * kStep;
        }
    }
    // The tape is in at 1 s of its slide, stays PositionWaitTime (1 s),
    // then its out slide fires HIDE_POSITION_TAPE_COMPLETE a second later.
    CHECK((doneAt > 2.5F && doneAt < 3.5F));
    CHECK(swoosh);
    CHECK(!screen.isShown());
    screen.show(1, 6);
    CHECK(screen.position() == std::string("WINNER"));
    screen.show(2, 6);
    CHECK(screen.position() == std::string("FAILED"));
}

/// Keeps the cases' shader programs out of the player's own cache, and
/// removes the folder they used after them.
class CacheFolder : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo& /*info*/) override
    {
        fh::setEnvironmentVariable(fh::userFolderVariable(fh::UserFolder::Cache), fh::pathToUtf8(folder()));
    }

    void testRunEnded(const Catch::TestRunStats& /*stats*/) override { std::filesystem::remove_all(folder()); }

private:
    static std::filesystem::path folder() { return std::filesystem::temp_directory_path() / "fh1-tst-menu-cache"; }
};

} // namespace

CATCH_REGISTER_LISTENER(CacheFolder)
