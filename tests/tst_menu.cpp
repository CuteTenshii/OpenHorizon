// Tests of the pause menu, the full-screen map and the title screen: the
// text wrapping, the map camera and the title prompt's timing on values
// built here, and, when FH1_GAME_DIR names an extracted disc, the pause
// menu's entries for free roam and races with their texts from the string
// tables, the structure of the scenes the screens bind, the map's tunables,
// background tiles and cursor area, and the title's movie and music.
// Nothing here needs an OpenGL context.

#include "TestHarness.h"

#include "audio/AudioDevice.h"
#include "audio/AudioLibrary.h"
#include "audio/Fev.h"
#include "base/Bytes.h"
#include "ui/AnarkFormats.h"
#include "ui/LoadingScreen.h"
#include "ui/MenuCommon.h"
#include "ui/Minimap.h"
#include "ui/MovieRenderer.h"
#include "ui/PauseMenu.h"
#include "ui/Presentation.h"
#include "ui/TitleScreen.h"
#include "ui/UiSounds.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

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

TEST(wrap_breaks_at_spaces_within_the_width)
{
    const auto measure = [](const std::string& text) { return static_cast<float>(text.size()); };
    CHECK_EQ(wrapText("one two three", 7.0F, measure), std::string("one two\nthree"));
    CHECK_EQ(wrapText("one\ntwo three", 20.0F, measure), std::string("one\ntwo three"));
    CHECK_EQ(wrapText("unbreakable word", 4.0F, measure), std::string("unbreakable\nword"));
    CHECK_EQ(wrapText("", 10.0F, measure), std::string());
}

TEST(help_icons_are_the_game_paths)
{
    CHECK_EQ(helpIconTexture(HelpIcon::A), std::string("media/ui/textures/horizon/controller/a_button.xds"));
    CHECK_EQ(helpIconTexture(HelpIcon::RightStickNoArrow),
        std::string("media/ui/textures/horizon/controller/rthumbstick_noarrow.xds"));
}

TEST(map_camera_opens_on_the_player_at_the_default_zoom)
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({-1500.0F, 800.0F});
    CHECK_NEAR(camera.zoom(), 0.38, 1e-6);
    CHECK_NEAR(camera.distance(), 500.0 + 7152.0 * 0.38, 1e-2);
    CHECK_NEAR(camera.cursor().x, -1500.0, 1e-3);
    CHECK_NEAR(camera.position().y, 800.0, 1e-3);
}

TEST(map_camera_keeps_the_view_inside_the_edges)
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({6000.0F, 3500.0F});
    // At the default zoom the view is 2 x 3218 m tall and 16/9 of that
    // wide, so its centre stays 5721 m inside the right edge and 3218 m
    // below the top.
    const float distance = camera.distance();
    CHECK_NEAR(camera.position().x, 7700.0 - distance * 16.0 / 9.0, 1e-2);
    CHECK_NEAR(camera.position().y, 4600.0 - distance, 1e-2);
    // The cursor itself stays where it was put, inside the cursor area.
    CHECK_NEAR(camera.cursor().x, 6000.0, 1e-3);
}

TEST(map_cursor_moves_with_the_shaped_stick)
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({0.0F, 0.0F});
    // Half deflection squared (Input_Linearity 2) at the default zoom's
    // speed of 900 + 9100 * 0.38 m/s, for one 60th of a second.
    camera.update({0.5F, 0.0F}, 0.0F, 1.0F / 60.0F);
    CHECK_NEAR(camera.cursor().x, 0.25 * (900.0 + 9100.0 * 0.38) / 60.0, 1e-2);
    CHECK_NEAR(camera.cursor().y, 0.0, 1e-6);
}

TEST(map_camera_follows_only_past_the_dead_area)
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({0.0F, 0.0F});
    // A small move stays inside 0.4 times the distance: the camera stays.
    for (int i = 0; i < 30; ++i) {
        camera.update({0.3F, 0.0F}, 0.0F, 1.0F / 60.0F);
    }
    CHECK(camera.cursor().x > 0.0F);
    CHECK(camera.cursor().x < 0.4F * camera.distance());
    CHECK_NEAR(camera.position().x, 0.0, 1e-3);
    // Far enough and the camera follows, trailing by the dead area once
    // its spring settles (short of the view-edge clamp, 1979 m east).
    for (int i = 0; i < 20; ++i) {
        camera.update({1.0F, 0.0F}, 0.0F, 1.0F / 60.0F);
    }
    for (int i = 0; i < 600; ++i) {
        camera.update({0.0F, 0.0F}, 0.0F, 1.0F / 60.0F);
    }
    CHECK_NEAR(camera.position().x, camera.cursor().x - 0.4 * camera.distance(), 1.0);
}

TEST(map_zoom_stays_within_the_cursor_zoom_limits)
{
    WorldMapCamera camera(shippedTuning(), {-6278.0F, -6137.76F}, {6840.0F, 3811.0F});
    camera.reset({0.0F, 0.0F});
    for (int i = 0; i < 300; ++i) {
        camera.update({0.0F, 0.0F}, 1.0F, 1.0F / 60.0F);
    }
    CHECK_NEAR(camera.zoom(), 0.1, 1e-6);
    CHECK_NEAR(camera.distance(), 500.0 + 7152.0 * 0.1, 1.0);
    for (int i = 0; i < 300; ++i) {
        camera.update({0.0F, 0.0F}, -1.0F, 1.0F / 60.0F);
    }
    CHECK_NEAR(camera.zoom(), 0.38, 1e-6);
}

bool fires(const TitleFlow::Step& step, std::string_view event)
{
    return std::find(step.events.begin(), step.events.end(), event) != step.events.end();
}

TEST(movie_definitions_select_streams_by_number)
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
    CHECK_EQ(fmv.audio.size(), std::size_t{2});
    CHECK_EQ(fmv.audio[0].id, 6);
    CHECK_EQ(fmv.audio[0].language, std::string("EN"));
}

TEST(title_prompt_fades_in_eight_updates_after_entering)
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

TEST(title_prompt_starts_on_a_or_start_and_not_on_b)
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

TEST(title_prompt_goes_to_the_attract_after_53_idle_seconds)
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
    CHECK_NEAR(t, 53.5, 1e-3);
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

TEST(title_prompt_plays_its_slide_cues)
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

TEST(ui_sound_table_matches_names_without_case)
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
    CHECK_EQ(table.size(), std::size_t{2});
    const UiSound* pause = table.find("pause");
    CHECK(pause != nullptr);
    if (pause) {
        CHECK_EQ(pause->event, std::string("UI/Nav/HUD_Pause"));
        CHECK(!pause->looping);
    }
    const UiSound* map = table.find("Map_AmbientBackgroundLoop");
    CHECK(map != nullptr && map->looping);
    CHECK(table.find("SpeedTrapFlash") == nullptr);
    CHECK(table.find("VO_PopUp_OthersStorefront") == nullptr);
    CHECK(table.find("Accept") == nullptr);
    CHECK_THROWS(UiSoundTable::parse("<ui4audio/>", "test"));
}

TEST(title_idle_timer_holds_while_the_sign_in_step_has_the_input)
{
    TitleFlow flow;
    flow.enter();
    MenuInput start;
    start.start = true;
    flow.update(start, 1.0F, false);
    for (int i = 0; i < 120; ++i) {
        CHECK(!fires(flow.update({}, 1.0F, false), "FADE_OUT"));
    }
    CHECK_NEAR(flow.idleSeconds(), 0.0, 1e-9);
}

const char* gameDir()
{
    const char* dir = std::getenv("FH1_GAME_DIR");
    if (dir == nullptr || *dir == '\0') {
        std::fprintf(stderr, "skipped: FH1_GAME_DIR is not set\n");
        return nullptr;
    }
    return dir;
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

TEST(disc_pause_entries_follow_the_builder)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
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
    CHECK_EQ(strings.get("ScreenTitles", "IDS_Pause"), std::string("GAME PAUSED"));
    CHECK(!strings.has("CarChallenges", "IDS_PauseMenu_CarChallenges"));
}

TEST(disc_pause_scene_has_the_bound_controls)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
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
            CHECK_EQ(scene.slideNames(child).size(), std::size_t{8});
        }
    }
    CHECK_EQ(buttons, 7);
    const int bar = scene.contract(scene.contractParameter(root, "COMPONENT_HELP_BUTTON_BAR"), "HELP_BUTTON_BAR");
    CHECK(scene.contractParameter(bar, "GROUP_BUTTONS") >= 0);

    const Presentation popup(SceneFiles::load(vfs, "media/ui/scenes/ui4/h_message_popup"));
    const int popupRoot = popup.contract(0, "UIContract");
    const int box = popup.contract(popup.contractParameter(popupRoot, "MESSAGE_BOX"), "MESSAGE_BOX");
    CHECK(popup.contractParameter(box, "TEXT_TITLE") >= 0);
    CHECK(popup.contractParameter(box, "TEXT_MESSAGE") >= 0);
}

TEST(disc_map_data_is_where_the_map_reads_it)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const WorldMapTuning tuning = WorldMapTuning::load(vfs);
    CHECK_NEAR(tuning.distanceMax, 7652.0, 1e-3);
    CHECK_NEAR(tuning.zoomDefault, 0.38, 1e-6);
    CHECK_NEAR(tuning.viewEdgeBottom, -6900.0, 1e-3);

    const Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/h_map"));
    const int root = scene.contract(0, "UIContract");
    const int overlay = scene.contract(scene.contractParameter(root, "COMPONENT_MAP_OVERLAY"), "MAP_OVERLAY");
    CHECK(scene.contractParameter(overlay, "COMPONENT_CURSOR") >= 0);
    CHECK(scene.contractParameter(overlay, "MODEL_MAP_VIEWPORT") >= 0);

    for (int column = 0; column < 5; ++column) {
        for (int row = 0; row < 3; ++row) {
            CHECK(vfs.exists(std::format("media/ui/textures/horizon/map/background/high-{}-{}.xds", column, row)));
        }
    }
    CHECK(vfs.exists("media/ui/textures/horizon/map/background/low-0-0.xds"));
    CHECK(vfs.exists("media/ui/textures/horizon/map/icons/mapicons/en/mapiconsheet.xds"));

    const NavFile nav = NavFile::parse(vfs.readOrThrow("media/tracks/colorado/colorado.nav"), "colorado.nav");
    int areas = 0;
    for (const NavWay& way : nav.ways) {
        if (way.tag("area_type") == "cursor_area") {
            ++areas;
            CHECK_EQ(way.points.size(), std::size_t{5});
        }
    }
    CHECK_EQ(areas, 1);
}

TEST(disc_loading_tips_resolve_from_the_string_tables)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    UiStrings strings(vfs, "en");
    const LoadingDefinitions definitions = LoadingDefinitions::load(vfs, strings);
    const LoadingTipGroup* freeRoam = definitions.find("freeroam");
    CHECK(freeRoam != nullptr);
    if (!freeRoam) {
        return;
    }
    CHECK_EQ(freeRoam->tipCount, 2);
    CHECK(freeRoam->random);
    bool raceEncounters = false;
    for (const LoadingTip& tip : freeRoam->tips) {
        CHECK_NEAR(tip.displayTime, 8.0, 1e-6);
        CHECK_EQ(tip.layout, 1);
        if (tip.title == "RACE ENCOUNTERS") {
            raceEncounters = true;
            CHECK(tip.message.starts_with("See a fellow Horizon racer"));
        }
    }
    CHECK(raceEncounters);
    CHECK(definitions.find("stats") != nullptr);
    CHECK(definitions.find("stats")->tips.empty());
    for (int i = 1; i <= 10; ++i) {
        CHECK(vfs.exists(std::format("media/ui/textures/horizon/loading/bgloadingimages/image{}.xds", i)));
    }

    const Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/2_c_ini_loading"));
    const int root = scene.contract(0, "UIContract");
    for (int i = 1; i <= 8; ++i) {
        CHECK(scene.contractParameter(root, std::format("TEXT_LINE_{}", i)) >= 0);
    }
    CHECK_NEAR(textBoxWidth(scene, scene.contractParameter(root, "TEXT_LINE_1")), 0.385 * 1280.0, 1e-2);
    CHECK(scene.contractParameter(root, "MATERIAL_BACKGROUND") >= 0);
}

TEST(disc_title_scene_binds_the_movie_and_the_prompt)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    UiStrings strings(vfs, "en");
    CHECK_EQ(strings.get("Main", "IDS_PressStart"), std::string("PRESS START"));

    Presentation scene(SceneFiles::load(vfs, "media/ui/scenes/ui4/120_p_pre_splash"));
    int root = -1;
    for (const int child : scene.children(0)) {
        if (scene.kind(child) == ElementKind::Behavior && scene.contractParameter(child, "TEXT_PRESS_START") >= 0) {
            root = child;
        }
    }
    CHECK(root >= 0);
    CHECK_EQ(scene.name(scene.contractParameter(root, "TEXT_PRESS_START")), std::string("TitleTapeText"));
    const int video = scene.contract(scene.contractParameter(root, "COMPONENT_VIDEO"), "CUSTREND_MOVIERENDERER");
    CHECK(video >= 0);
    CHECK(scene.contractFlag(video, "VIDEOLOOP").value_or(false));
    CHECK(scene.contractFlag(video, "VIDEOAUTOPLAY").value_or(false));
    const int viewport = scene.contractParameter(video, "MODEL_VIEWPORT");
    CHECK_EQ(scene.name(viewport), std::string("loading_image"));
    const BgfFile& bgf = scene.files().bgf;
    std::string path;
    for (const Attribute& attribute : bgf.elements[static_cast<std::size_t>(video)].attributes) {
        if (attribute.hash == anarkHash("VIDEOPATH")) {
            path = bgf.strings[attribute.raw];
        }
    }
    CHECK_EQ(path, std::string("GAME:\\Media\\UI\\Videos\\PressStart.wmv"));
    for (const char* event : {"CONTROLLER_ONLY", "FADE_IN", "FADE_OUT", "START_OUTRO", "START_INTRO"}) {
        CHECK(scene.fireEvent(root, event));
    }
    CHECK(scene.slideNames(scene.find("Layer/Fader")).size() == 4);
}

TEST(disc_title_movie_is_the_wmv3_stream_its_definition_names)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const MovieStreams streams
        = MovieStreams::parse(vfs.readText("media/ui/videos/pressstart.def").value_or(""), "PressStart.def");
    CHECK(streams.video == 2);
    CHECK(streams.audio.empty());
    const MovieInfo info = MovieRenderer::probe(vfs, "GAME:\\Media\\UI\\Videos\\PressStart.wmv");
    CHECK_EQ(info.streamId, 2);
    CHECK_EQ(info.codec, std::string("wmv3"));
    CHECK_EQ(info.width, 1280);
    CHECK_EQ(info.height, 720);
    CHECK_NEAR(info.duration, 53.93, 0.01);
    CHECK_NEAR(info.firstFrameTime, 0.0, 1e-6);
}

TEST(disc_title_music_is_the_front_end_ambience)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const std::string sounds = vfs.readText("media/audio/ui/ui4audio.xml").value_or("");
    pugi::xml_document document;
    CHECK(static_cast<bool>(document.load_buffer(sounds.data(), sounds.size())));
    const pugi::xml_node splash
        = document.child("ui4audio").child("defaults").find_child_by_attribute("event", "play", "SplashScreen");
    CHECK_EQ(std::string(splash.attribute("group").as_string()), std::string("UIInGame/UI/FrontEnd"));
    CHECK_EQ(std::string(splash.attribute("cue").as_string()), std::string("FE_Ambience"));
    CHECK(splash.attribute("isLooping").as_bool());

    fh::audio::AudioLibrary library(vfs);
    const auto project = library.project("Game:\\Media\\Audio\\UI\\UIInGame\\UIInGame.fev");
    const fh::audio::FevEvent* event = project->findEvent("UI/FrontEnd/FE_Ambience");
    CHECK(event != nullptr);
    if (!event) {
        return;
    }
    CHECK_EQ(event->layers.size(), std::size_t{1});
    CHECK_EQ(event->layers[0].sounds.size(), std::size_t{1});
    CHECK_EQ(project->soundDefinition(event->layers[0].sounds[0].soundDefinition).name,
        std::string("/FrontEnd/PressStart_Music"));
}

TEST(disc_ui_sounds_resolve_to_uiingame_events)
{
    const char* dir = gameDir();
    if (!dir) {
        return;
    }
    fh::Vfs vfs;
    vfs.mountDisc(dir);
    const UiSoundTable table = UiSoundTable::load(vfs);
    CHECK_EQ(table.size(), std::size_t{754});
    fh::audio::AudioLibrary library(vfs);
    const auto project = library.project("Game:\\Media\\Audio\\UI\\UIInGame\\UIInGame.fev");
    const auto event = [&](std::string_view name) -> std::string {
        const UiSound* sound = table.find(name);
        return sound ? sound->event : std::string();
    };
    // The names the pause flow, the pause menu, the map and the title play.
    CHECK_EQ(event("Pause"), std::string("UI/Nav/HUD_Pause"));
    CHECK_EQ(event("UnPause"), std::string("UI/Nav/HUD_UnPause"));
    CHECK_EQ(event("Accept"), std::string("UI/Nav/Accept"));
    CHECK_EQ(event("Cancel"), std::string("UI/Nav/Back"));
    CHECK_EQ(event("Map_AmbientBackgroundLoop"), std::string("UI/HorizonMap/MAP_AmbientBackgroundLoop"));
    CHECK_EQ(event("PressStartSlideOn"), std::string("UI/Blank/Blank"));
    CHECK_EQ(event("PressStartSlideOff"), std::string("UI/Blank/Blank"));
    for (const std::string_view name : {"Pause", "UnPause", "Accept", "Cancel", "Map_AmbientBackgroundLoop",
             "PressStartSlideOn", "PressStartSlideOff"}) {
        CHECK(project->findEvent(event(name)) != nullptr);
    }

    // Played on a device with no output: the pause sound sounds, a second
    // Accept restarts the first (the event allows one playback), the map's
    // loop stays started while RadioIsPlaying keeps its only sound silent.
    fh::audio::AudioDevice device{fh::audio::AudioDevice::Offline{}};
    UiSounds sounds(vfs, device.mixer(), library, fh::audio::kMasterBus);
    sounds.play("Pause");
    CHECK(sounds.isPlaying("Pause"));
    CHECK_EQ(device.mixer().activeVoiceCount(), std::size_t{1});
    device.render(48000 * 3);
    CHECK_EQ(device.mixer().activeVoiceCount(), std::size_t{0});
    CHECK(!sounds.isPlaying("pause"));
    sounds.play("Accept");
    sounds.play("Accept");
    device.render(256);
    CHECK_EQ(device.mixer().activeVoiceCount(), std::size_t{1});
    device.render(48000 * 3);
    sounds.play("Map_AmbientBackgroundLoop");
    sounds.setParameter("Map_AmbientBackgroundLoop", "RadioIsPlaying", 1.0F);
    device.render(4800);
    CHECK(sounds.isPlaying("Map_AmbientBackgroundLoop"));
    CHECK_EQ(device.mixer().activeVoiceCount(), std::size_t{0});
    sounds.setParameter("Map_AmbientBackgroundLoop", "RadioIsPlaying", 0.0F);
    CHECK_EQ(device.mixer().activeVoiceCount(), std::size_t{1});
    sounds.stop("Map_AmbientBackgroundLoop");
    CHECK(!sounds.isPlaying("Map_AmbientBackgroundLoop"));
    sounds.play("PressStartSlideOn");
    CHECK_EQ(sounds.played().size(), std::size_t{5});
    CHECK_EQ(sounds.played().back(), std::string("PressStartSlideOn"));
}

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
