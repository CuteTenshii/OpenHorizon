#pragma once

#include "MenuCommon.h"

#include <memory>
#include <string>
#include <vector>

namespace fh {
class Vfs;
}

namespace fh::ui {

/// The entries the game's pause menu can list, numbered as the menu's
/// builder (CPauseMenu, 0x827de1b0) tags them and its select handler
/// (0x827ebee0) switches on them.
enum class PauseItem : int {
    QuitMission = 0,
    RestartMission = 1,
    Map = 2,
    Resume = 3,
    CoopChallenges = 4,
    MessageCentre = 5,
    HorizonRally = 6,
    Multiplayer = 7,
    SponsorChallenges = 8,
    Restart = 10,
    PhotoMode = 11,
    MyProfile = 12,
    HeadTracking = 13,
    /// "QUIT RACE", or "RETIRE" in the multiplayer sessions the builder
    /// checks for; either way the handler (0x827eae30) asks to retire from
    /// the event.
    QuitRace = 14,
    EndFreeRoam = 15,
    Finish = 16,
    Quit = 17,
    CarChallenges = 19,
};

/// What the game should do after the pause menu's update. The menu closes
/// itself for every action but None.
enum class PauseAction {
    None,
    /// B (the help bar's RESUME): the pause screen's onDone.
    Resume,
    /// RESTART, confirmed in its message box.
    Restart,
    /// QUIT RACE, confirmed in the "RETIRE FROM EVENT" message box
    /// (onRetire).
    RetireFromEvent,
    /// QUIT, confirmed in its message box ("Are you sure you want to quit
    /// Horizon single player?", EXIT SINGLE PLAYER): onQuit, then
    /// unload_to_splash in media/gamemodes/pause.xml, which leaves the
    /// game session. With no front end here, the game exits.
    Quit,
    /// MAP: the map screen, which returns to the pause menu when left with
    /// B (map_flow_pause in pause.xml enters pause_screen.enterBack).
    OpenMap,
    /// Entries that lead to screens this reimplementation does not have
    /// yet, returned so the game can show them once it does.
    OpenMultiplayer,
    OpenPhotoMode,
    OpenMessageCentre,
    OpenMyProfile,
    OpenSponsorChallenges,
    OpenCoopChallenges,
    OpenHeadTracking,
    OpenCarChallenges,
    OpenHorizonRally,
    Finish,
    EndFreeRoam,
    QuitMission,
    RestartMission,
};

/// The state the pause menu's builder (0x827de1b0) reads to pick its
/// entries. Each field names the check it stands for; the defaults are
/// what a single-player session of this reimplementation has.
struct PauseMenuContext {
    /// The session's mode: free roam is one of the modes 17, 18 and 19 the
    /// builder tests (0x824b75e8 reads it); a race is none of them, nor one
    /// of the multiplayer modes 6, 13 and 18. The multiplayer sessions'
    /// entries (RETIRE, CO-OP CHALLENGES, END FREE ROAM) are not offered
    /// here.
    bool freeRoam = true;
    /// The first-time career activity is running (0x82701110): the menu
    /// then lists only RESUME and QUIT.
    bool firstTimeCareer = false;
    /// MAP: listed unless the first-time career activity is running and
    /// unfinished (0x82701068).
    bool mapAvailable = true;
    /// PHOTO MODE: hidden when the session forbids it (0x8274b828 with the
    /// profile byte at +2391, and the flag 0x827466f8 reads); both checks
    /// are false in an ordinary session.
    bool photoModeAvailable = true;
    /// The check 0x824ac908 makes on the profile's session descriptor:
    /// false when its byte +88 is 0, which the festival game has (the
    /// first-time career, 0x82701110, only runs with it 0); HORIZON RALLY
    /// needs it true. What the non-zero values of the byte stand for was
    /// not identified.
    bool alternateProfileMode = false;
    /// MULTIPLAYER: hidden in modes 3 and 20 and when the menu's flag +520
    /// is set (0x827daf18); also gates QUIT.
    bool multiplayerAvailable = true;
    /// A PR stunt mission is running (the menu's +460): RESTART PR STUNT at
    /// the top and QUIT PR STUNT in place of QUIT; MULTIPLAYER and MESSAGE
    /// CENTER are dropped.
    bool inMission = false;
    /// Free roam with an activity running that the builder finds through
    /// 0x824c91a8 (r21): QUIT RACE is listed and QUIT is not.
    bool freeRoamActivity = false;
    /// FINISH (0x827d6690 and the race's own check) replaces QUIT RACE.
    bool canFinish = false;
    /// SPONSOR CHALLENGES (0x826f6678): a sponsor challenge is on offer.
    bool sponsorChallenges = false;
    /// HEAD TRACKING: a Kinect sensor with head tracking.
    bool headTracking = false;
};

/// One entry as the menu shows it.
struct PauseMenuEntry {
    PauseItem item = PauseItem::Resume;
    std::string text;
};

/// How far right an entry's text and its brush stroke (the entry's Crisp
/// Tape, the black stroke under the text) reach as last drawn, in the
/// scene's design units: the text at its baseline, the stroke where its
/// texture ends (SUPER_STACKER slides that texture along the tape rather
/// than resizing the tape, so the tape's mesh reaches far past it).
struct PauseEntryExtent {
    float textRight = 0.0F;
    float strokeRight = 0.0F;
};

/// The entries the builder (0x827de1b0) lists for `context`, in order,
/// with their text from the string tables it names. Entries whose text is
/// missing from the disc (downloadable content: HORIZON RALLY, CAR
/// CHALLENGES) are left out, and so is MESSAGE CENTER, an Xbox LIVE
/// feature the game does not list offline, as this reimplementation always
/// is.
std::vector<PauseMenuEntry> pauseMenuEntries(const PauseMenuContext& context, UiStrings& strings);

/// The game's pause menu (CPauseMenu on media/ui/scenes/ui4/925_pause_menu,
/// screen 206), with the message boxes its entries raise
/// (media/ui/scenes/ui4/h_message_popup, screen 222).
///
/// Verified from the executable: the entries and their order, texts and
/// tables (0x827de1b0); the title (IDS_Pause in ScreenTitles, 0x827dfb18);
/// the help bar (SELECT on A, RESUME on B, 0x827d6348); the list's SIZE<n>
/// event (0x828b53e4); the item events the scrolling list fires on focus
/// changes (0x82865c40: the item moved to gets FOCUSABOVE when focus moves
/// up and FOCUSBELOW when it moves down, the item left gets BLURABOVE or
/// BLURBELOW by whether it lies above or below the new focus, and on
/// opening the focus gets FOCUSED and the rest BLURREDABOVE or
/// BLURREDBELOW); that the list does not wrap (BOOLEAN_WRAP_DATA false);
/// what each entry does (0x827ebee0) and the texts of the confirmations
/// (0x827ea1c0, 0x827ea788, 0x827eae30). Not drawn: the player bar (level,
/// car, credits), which this reimplementation has no data for, and the
/// multiplayer timer and target time (HIDETARGET is fired as the game
/// does).
///
/// Construct with a current OpenGL 4.5 context.
class PauseMenu {
public:
    /// Throws fh::DataError if a scene cannot be loaded.
    explicit PauseMenu(const Vfs& vfs, const std::string& language = "en");
    ~PauseMenu();
    PauseMenu(const PauseMenu&) = delete;
    PauseMenu& operator=(const PauseMenu&) = delete;

    /// Builds the entries for `context` and shows the menu with the first
    /// entry focused.
    void open(const PauseMenuContext& context);
    /// Shows the menu again as it was, for coming back from the map
    /// (pause_screen.enterBack).
    void reopen();
    void close();
    bool isOpen() const;

    PauseAction update(const MenuInput& input, float dt);
    /// Draws over the bound framebuffer, blending, depth testing off.
    void draw(int width, int height);

    const std::vector<PauseMenuEntry>& entries() const;
    int focus() const;
    /// Whether a confirmation message box is up.
    bool confirming() const;
    /// The extents of the entries shown, in order, as the last draw() laid
    /// them out; empty before the first draw.
    std::vector<PauseEntryExtent> entryExtents() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace fh::ui
