#pragma once

#include "MenuCommon.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Vfs;
}

/// The front-end screens of a career event, from the event description to
/// the finish position.
namespace openhorizon::ui {

/// An event's gamedb rows (Events, Tracks, EventUIColors), texts resolved.
struct EventScreenInfo {
    /// Events.Name and Events.ShortName.
    std::string name;
    std::string shortName;
    /// Events.CareerEventStyle: 1 festival race, 2 street race, 3 showcase,
    /// 4 rally, 5 nemesis race, 6 headline race.
    int careerEventStyle = 0;
    /// Events.Level; below zero for the festival's heats.
    int level = 0;
    std::string description;
    std::string restrictionDescription;
    /// Races.NumLaps, 0 for a point-to-point race.
    int laps = 0;
    /// Tracks.Length, metres.
    float lengthMetres = 0.0F;
    bool metric = true;
    /// Events.CashPrize, credits.
    int cashPrize = 0;
    /// Events.PlayerCarId, 0 when the player brings their own car.
    int playerCarId = 0;
    /// Data_Car.ModelShort of the provided car (0x82733af0); empty without one.
    std::string providedCarName;
    /// Events.EventTicket: the ticket image's name in its style's folder.
    std::string ticket;
    /// EventUIColors.Color1 to Color7 (0xRRGGBB), empty without a row.
    std::vector<std::uint32_t> colours;
};

/// Events.Name in capitals (0x82519d88, table 0x82cbf8c0), or ShortName past
/// UI/EventTitleMaxLongNameLength (30, globalregistry.xml; 0x8322bbb8).
std::string eventTitle(const EventScreenInfo& info);
/// CareerEventUI's IDS_TitleFestivalRace ... IDS_TitleHeadlineRace for
/// styles 1 to 6 (0x826f6328), empty otherwise.
std::string eventKindLine(const EventScreenInfo& info, UiStrings& strings);
/// The number and InGame's IDS_Place_<m>, m the place modulo 100, modulo 10
/// again from 20 on ("1ST", "12TH", "22ND").
std::string placeOrdinal(int place, UiStrings& strings);

/// CCareerEventDescription (0x827272e8) on media/ui/scenes/ui4/
/// h_event_summary, screen 35. Verified: show 0x82733a68, restrictions
/// 0x828ec508, ticket folder by style 0x828ec658 and path 0x828ee038, help
/// bar 0x82728138, A fires okCareer 0x82734200.
///
/// Inferred: A also enters with the player's own car (the garage check,
/// 0x82720db8, is not reimplemented) and B fires return.
///
/// Left out, not traced and hidden: points value (0x8253b230), prize
/// (0x826984e8), race and track lines (0x828f7b90), car status and class
/// icons (0x827209a0, 0x82704e38), best finish, COLOR_SCHEME, and the Y, X,
/// Back and Start buttons (flags +364 to +374).
///
/// Construct with a current OpenGL 4.5 context.
class EventSummaryScreen {
public:
    enum class Action {
        None,
        /// A, ENTER EVENT: the flow's okCareer.
        Enter,
        /// B, BACK: the flow's return.
        Back,
    };

    /// Throws openhorizon::DataError if the scene cannot be loaded.
    explicit EventSummaryScreen(const Vfs& vfs, const std::string& language = "en");
    ~EventSummaryScreen();
    EventSummaryScreen(const EventSummaryScreen&) = delete;
    EventSummaryScreen& operator=(const EventSummaryScreen&) = delete;

    void open(const EventScreenInfo& info);
    void close();
    bool isOpen() const;
    /// Closes itself on every action but None.
    Action update(const MenuInput& input, float dt);
    /// Draws over the bound framebuffer, blending, depth testing off.
    void draw(int width, int height);

    /// A bound element's text by root contract name ("TEXT_TITLE"); empty
    /// when the scene has no such element.
    std::string text(std::string_view contractParameter) const;
    /// The ticket image drawn in MATERIAL_TICKET_BG, as a game path; empty
    /// when the event has none.
    const std::string& ticket() const;
    /// The scene's root slide: the colour scheme the show chose.
    std::string slide() const;
    /// The help buttons in use as the last draw() laid them out.
    std::vector<HelpButtonExtent> helpExtents() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// The race intro's title card: CCareerEventTitle (0x82720e70) on
/// media/ui/scenes/ui4/pre_and_post_race, screen 37. Verified: binds only
/// COMPONENT_EVENT_BAR (show 0x82728580), done 4.0 s after (0x8271c980).
///
/// Inferred: the winnings group is not drawn. Left out: the headline race's
/// CS_ChampionofChampionsSlideIn and the scene's PLAY_SOUND_EVENT cues.
///
/// Construct with a current OpenGL 4.5 context.
class EventTitleCard {
public:
    /// Throws openhorizon::DataError if the scene cannot be loaded.
    explicit EventTitleCard(const Vfs& vfs, const std::string& language = "en");
    ~EventTitleCard();
    EventTitleCard(const EventTitleCard&) = delete;
    EventTitleCard& operator=(const EventTitleCard&) = delete;

    void show(const EventScreenInfo& info);
    void hide();
    bool isShown() const;
    /// Returns true on the frame the card is done (its onDone).
    bool update(float dt);
    void draw(int width, int height);

    std::string title() const;
    std::string subtitle() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// Numbered as CPreRace adds them (0x828f5818) and selects them (0x82821b00).
enum class PreRaceItem : int {
    StartRace = 1,
    QuitRace = 2,
    Options = 6,
    Difficulty = 7,
};

/// One entry as the menu shows it.
struct PreRaceEntry {
    PreRaceItem item = PreRaceItem::StartRace;
    std::string text;
};

/// The entries CPreRace lists outside the rivals mode, with their texts.
std::vector<PreRaceEntry> preRaceEntries(UiStrings& strings);

/// CPreRace (0x82817378) on media/ui/scenes/ui4/h_generic_title, screen
/// 296, with the QUIT RACE box (h_message_popup, screen 242). Verified:
/// show 0x8281cf68, help bar 0x82817538, QUIT RACE box 0x8281d568.
///
/// Inferred: confirming the box fires quit; its buttons (type 4 untraced).
/// Left out: DIFFICULTY (0x82593190, 0x82583db8), the OPTIONS child list,
/// BACK on B (flag +437), the rivals texts, titles HIDE (+1996, +3548).
///
/// Construct with a current OpenGL 4.5 context.
class PreRaceMenu {
public:
    enum class Action {
        None,
        /// START RACE: the flow's race.
        Race,
        /// QUIT RACE, confirmed: the flow's quit.
        Quit,
        /// OPTIONS: the caller shows the option screens.
        Options,
    };

    /// Throws openhorizon::DataError if a scene cannot be loaded.
    explicit PreRaceMenu(const Vfs& vfs, const std::string& language = "en");
    ~PreRaceMenu();
    PreRaceMenu(const PreRaceMenu&) = delete;
    PreRaceMenu& operator=(const PreRaceMenu&) = delete;

    void open(const EventScreenInfo& info);
    void close();
    bool isOpen() const;
    /// Closes itself on Race and Quit.
    Action update(const MenuInput& input, float dt);
    void draw(int width, int height);

    const std::vector<PreRaceEntry>& entries() const;
    int focus() const;
    /// Whether the QUIT RACE message box is up.
    bool confirming() const;
    std::string title() const;
    std::string subtitle() const;
    std::vector<HelpButtonExtent> helpExtents() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

/// CPostRaceFinishPosition (0x8281c020) on media/ui/scenes/ui4/
/// h_post_race_finished, screen 292. Verified: update 0x82821420 waits
/// UI/RaceRewards/PositionWaitTime (globalregistry.xml, 1.0 s); no input.
///
/// Inferred: results flag +432 is set once the player has finished. Not
/// drawn: COMPONENT_TIMER (online).
///
/// Construct with a current OpenGL 4.5 context.
class PostRaceFinishedScreen {
public:
    /// Throws openhorizon::DataError if the scene cannot be loaded.
    explicit PostRaceFinishedScreen(const Vfs& vfs, const std::string& language = "en");
    ~PostRaceFinishedScreen();
    PostRaceFinishedScreen(const PostRaceFinishedScreen&) = delete;
    PostRaceFinishedScreen& operator=(const PostRaceFinishedScreen&) = delete;

    /// Shows the player's finishing `place` (1-based) of an event of
    /// `careerEventStyle`.
    void show(int place, int careerEventStyle);
    void hide();
    bool isShown() const;
    /// Returns true on the frame the screen is done (its onDone).
    bool update(float dt);
    void draw(int width, int height);

    std::string title() const;
    std::string position() const;
    /// UI sounds the code asked for since the last call, not the scene's own
    /// PLAY_SOUND_EVENT ones.
    std::vector<std::string> takeSounds();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace openhorizon::ui
