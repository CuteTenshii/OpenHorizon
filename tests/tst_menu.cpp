// Tests of the pause menu and the full-screen map: the text wrapping and the
// map camera on values built here, and, when FH1_GAME_DIR names an
// extracted disc, the pause menu's entries for free roam and races with
// their texts from the string tables, the structure of the scenes both
// screens bind, the map's tunables, background tiles and cursor area.
// Nothing here needs an OpenGL context.

#include "TestHarness.h"

#include "base/Bytes.h"
#include "ui/AnarkFormats.h"
#include "ui/MenuCommon.h"
#include "ui/Minimap.h"
#include "ui/PauseMenu.h"
#include "ui/Presentation.h"
#include "ui/WorldMap.h"
#include "vfs/Vfs.h"

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
        == std::vector<std::string>({"MAP", "MULTIPLAYER", "PHOTO MODE", "MESSAGE CENTER", "MY PROFILE", "QUIT"}));
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

} // namespace

int main(int argc, char** argv)
{
    return fh::test::runAllTests(argc > 1 ? argv[1] : "");
}
