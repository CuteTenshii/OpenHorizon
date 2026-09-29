#pragma once

#include "MenuCommon.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::ui {

/// One tip of media/ui/LoadingDefs.xml, its texts resolved.
struct LoadingTip {
    /// The TipLayout slide it shows on (1 = Layout1).
    int layout = 1;
    /// Seconds on screen.
    float displayTime = 0.0F;
    std::string title;
    std::string message;
};

/// A <LoadingGroup>: the tips for one game mode or loading condition.
struct LoadingTipGroup {
    std::string name;
    /// How many of its tips one loading screen shows before the game moves
    /// on to the "stats" group.
    int tipCount = 1;
    /// order="random" (true) or "sequential".
    bool random = true;
    std::vector<LoadingTip> tips;
};

/// media/ui/LoadingDefs.xml as the loader (0x827649a0) reads it. Texts are
/// "Table:IDS_Id" references looked up in the string tables.
struct LoadingDefinitions {
    std::vector<LoadingTipGroup> groups;

    /// Throws DataError when the file cannot be read.
    static LoadingDefinitions load(const Vfs& vfs, UiStrings& strings);
    const LoadingTipGroup* find(std::string_view name) const;
};

/// One line of a tip's text as the last LoadingScreen::draw() laid it out.
struct LoadingTipLine {
    /// The line's text as drawn, its sizes after the text scale.
    DrawText text;
    /// The line's brush bar (its Pill_Big), as drawn.
    DrawModel bar;
};

/// The game's loading screen (CDynamicTipLoadingScreen on
/// media/ui/scenes/ui4/2_c_ini_loading): a background photo, a tip's title
/// and its text over the scene's brush bars, and PLEASE WAIT with the
/// spinner.
///
/// Verified from the executable: the background (0x827613e8): one of
/// Image1 to Image11, Image14 and Image16 in
/// media/ui/textures/horizon/loading/bgloadingimages, chosen at random among
/// the ones shown least often so far this session, whose count then grows
/// by one (by two when it was the only least-shown one); the tip (0x827479c0:
/// random, never the one just shown, or the next in order); the text split
/// over the eight TEXT_LINE_n elements, each taking what fits its box
/// (0x82764d0c, done here with the element's boxwidth) and the lines in use
/// shown with SHOW_LINE_<n>, the rest hidden with HIDE_LINE_<n>; the timing
/// (0x82762c30): time steps capped at 0.05 s, the tip's layout component
/// driven by its progress from 0 to 0.5 over the first quarter of the
/// display time (0.5 - 0.5 (1 - t)^2), held, then to 1 over the last
/// quarter (0.5 + 0.5 t^2), after which the next tip of the group shows
/// until the group's tipcount has been shown. Each line and the title run
/// the scene's SUPER_STACKER contracts (see SuperStackers), so every bar
/// fits the text set on it.
///
/// Left out: after tipcount tips the game switches to the "stats" group,
/// whose tips it builds from the player's statistics (its entry in
/// LoadingDefs.xml is empty); here the last tip stays up. The tips' counters
/// restart with each loading screen (the game's reset was not traced). Of
/// the thirteen background names, Image11, Image14 and Image16 are not on
/// the disc; only the ten that are take part in the choice. The every-Nth
/// Horizon Rally upsell and the modes 19 and 20 background range belong to
/// downloadable content and multiplayer.
///
/// Construct with a current OpenGL 4.5 context.
class LoadingScreen {
public:
    /// `seed` seeds the choice of background and tip (the game uses the C
    /// library's rand()). Throws fh::DataError when the scene or
    /// LoadingDefs.xml cannot be read.
    explicit LoadingScreen(const Vfs& vfs, const std::string& language = "en", std::uint32_t seed = 0);
    ~LoadingScreen();
    LoadingScreen(const LoadingScreen&) = delete;
    LoadingScreen& operator=(const LoadingScreen&) = delete;

    /// Shows the screen with a tip from `group` (a LoadingGroup name, the
    /// tip_group attribute of the state that shows it: "freeroam" for fast
    /// travel and for returning to free roam from a mission, "workshop",
    /// "paintshop" and so on). An unknown group shows no tip.
    void show(std::string_view group);
    void hide();
    bool isShown() const;

    void update(float dt);
    /// Draws the full screen over the bound framebuffer.
    void draw(int width, int height);

    /// The tip shown, if any.
    const LoadingTip* tip() const;
    /// The game path of the background shown.
    const std::string& background() const;
    /// The lines of the tip's text in use, in order, as the last draw()
    /// laid them out; empty before the first draw after show().
    std::vector<LoadingTipLine> tipLines() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fh::ui
