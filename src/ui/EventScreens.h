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

/// The front-end screens of a career event's flow, from the event
/// description at its start in free roam to the finish position after the
/// race.
namespace openhorizon::ui {

/// What the event screens show of an event: its gamedb row as the game
/// reads it for them (Events, Tracks and EventUIColors), with the database
/// texts resolved.
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
    /// Data_Car.ModelShort of the car the event provides (the show handler
    /// reads it for Events.PlayerCarId, 0x82733af0); empty without one.
    std::string providedCarName;
    /// Events.EventTicket: the ticket image's name in its style's folder.
    std::string ticket;
    /// EventUIColors.Color1 to Color7 (0xRRGGBB), empty without a row.
    std::vector<std::uint32_t> colours;
};

/// The event's title as the game shows it on these screens: Events.Name,
/// or Events.ShortName when the name is longer than
/// UI/EventTitleMaxLongNameLength (30, globalregistry.xml; the initialiser
/// at 0x8322bbb8), in capitals (0x82519d88, each character mapped through
/// 0x82cbf8c0).
std::string eventTitle(const EventScreenInfo& info);
/// The kind line under the title: CareerEventUI's IDS_TitleFestivalRace,
/// IDS_TitleStreetRace, IDS_TitleExhibition, IDS_TitleRally,
/// IDS_TitleNemesisRace or IDS_TitleHeadlineRace for styles 1 to 6
/// (0x826f6328), empty for any other style.
std::string eventKindLine(const EventScreenInfo& info, UiStrings& strings);
/// A place as the finish position writes it (CPostRaceFinishPosition): the
/// number followed by InGame's IDS_Place_<m>, where m is the place modulo
/// 100, taken modulo 10 again from 20 on ("1ST", "12TH", "22ND").
std::string placeOrdinal(int place, UiStrings& strings);

/// The event description at the start of a career event in free roam:
/// CCareerEventDescription (0x827272e8) on media/ui/scenes/ui4/
/// h_event_summary, screen 35, which the activation flow's
/// CShowUIScreen_CareerEventDescription shows.
///
/// Verified from the show handler (0x82733a68): TEXT_TITLE is the kind
/// line; TEXT_DESCRIPTION Events.Description; TEXT_CAR_RESTRICTIONS and
/// TEXT_RESTRICTION_LIST Events.RestrictionDescription (0x828ec508);
/// TEXT_RESTRICTION_TITLE CareerEventUI IDS_EventRestrictions;
/// TEXT_CAR_TITLE Data_Car.ModelShort of the provided car; for a headline
/// race TEXT_POINTS_TITLE, TEXT_POINTS_VALUE and TEXT_WRISTBAND are
/// IDS_WinnerTakesAll and IDS_HorizonChampion_Line1 and _Line2, otherwise
/// TEXT_POINTS_TITLE is IDS_PointsAvailable; the scheme event SHOW_FINAL
/// (style 6), SHOW_EXHIBITION (3), SHOW_HEATS (1 with a negative level) or
/// SHOW_FESTIVAL_RACE; MATERIAL_TICKET_BG the ticket
/// game:\media\ui\textures\Horizon\EventTickets\<folder>\<EventTicket>
/// with the folder picked by style at 0x828ec658 (0x828ee038 builds the
/// path). The help bar (0x82728138): ENTER EVENT on A while the flow offers
/// okCareer, BACK on B; A fires okCareer when the event provides the car
/// (0x82734200).
///
/// Inferred: that A enters with the player's own car too (the game then
/// looks at the garage's eligible cars, 0x82720db8, which this
/// reimplementation does not have) and that B fires return.
///
/// Left out, their sources not traced: the points value (Events.Level
/// through WristbandScoring, 0x8253b230) and the prize (Events.CashPrize
/// through 0x826984e8), with the points heading; the race and track lines
/// (a distance and unit from the event-info record 0x828f7b90 and a street
/// race's place); the car status, eligibility and class icons (the garage;
/// 0x827209a0, 0x82704e38); the best finish; the COLOR_SCHEME contracts
/// that take EventUIColors; the RESTRICTIONS (Y), leaderboard and rivals
/// (X), PURCHASE CAR (Back) and CHANGE CAR (Start) buttons, whose flags
/// (+364 to +374) were not traced. Their elements are hidden rather than
/// show the scene's placeholder texts. The player bar goes to its HIDE
/// slide, as the screen is given no profile.
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

    /// The text an element the screen binds shows ("TEXT_TITLE",
    /// "TEXT_DESCRIPTION", ... as the scene's root contract names them);
    /// empty when the scene has no such element.
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

/// The title card over the race's intro: CCareerEventTitle (0x82720e70)
/// on media/ui/scenes/ui4/pre_and_post_race, screen 37, which the race
/// flow's CShowUIScreen_CareerEventTitle shows.
///
/// Verified: the card binds COMPONENT_EVENT_BAR alone (an EVENT_BAR contract
/// with TEXT_TITLE, TEXT_SUBTITLE and TEXT_NEMESIS); its show (0x82728580)
/// sets the event title and the kind line and fires EVENT at the bar, whose
/// slide fires EVENT_NAME_APPEAR and PLAY_SOUND_EVENT itself; its update
/// (0x8271c980) is done 4.0 s after the show. It takes no input.
///
/// Inferred: that the scene's winnings group, which the root contract's
/// TEXT_WINNINGS belong to, is not drawn with the card. Left out: the
/// headline race's slide-in sound (CS_ChampionofChampionsSlideIn) and the
/// scene's own PLAY_SOUND_EVENT cues, which the game plays through its UI
/// sounds.
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

/// The entries of the pre-race menu, numbered as CPreRace adds them
/// (0x828f5818) and its select handler (0x82821b00) switches on them.
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

/// The pre-race menu: CPreRace (0x82817378) on media/ui/scenes/ui4/
/// h_generic_title, screen 296, which the race flow's CShowPreRaceScreen
/// shows, with the QUIT RACE message box (h_message_popup, screen 242).
///
/// Verified (show handler 0x8281cf68): COMPONENT_TITLES' MENU_TITLES gets
/// the event title and the kind line with TWO_TITLES (ONE_TITLE without a
/// kind line); COMPONENT_MENU lists START RACE (InGame IDS_RaceMenu),
/// OPTIONS (PauseMenu IDS_Options) and QUIT RACE (InGame
/// IDS_QuitRaceMenu), with the list events the pause menu's list fires;
/// the help bar (0x82817538) has SELECT on A. START RACE fires the flow's
/// race; QUIT RACE (0x8281d568) raises the box with PauseMenu's
/// IDS_QuitRace and IDS_QuitRace_Desc_Career.
///
/// Inferred: that confirming the box fires quit; the box's buttons (type 4
/// was not traced; the pause menu's retire box has OK and CANCEL). Left
/// out: DIFFICULTY (listed when 0x82593190 and not 0x82583db8, not
/// traced), the OPTIONS child list (CONTROLS, HUD, AUDIO and VIDEO, which
/// the game nests under OPTIONS; OPTIONS returns Action::Options, and the
/// game shows those screens' menu over this one),
/// BACK on B (only with flag +437, which nothing found sets), the rivals
/// mode's texts and the HIDE of the titles (profile +1996 and +3548, not
/// identified).
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

/// The finish position after a race: CPostRaceFinishPosition (0x8281c020)
/// on media/ui/scenes/ui4/h_post_race_finished, screen 292, which the race
/// flow's CShowPostRaceFinishPositionScreen shows.
///
/// Verified: TEXT_SCREEN_TITLE is ScreenTitles IDS_PostRace_Finished,
/// TEXT_ONLINE empty, TEXT_POSITION PostRaceFlow IDS_Place with the place's
/// ordinal (a headline race shows CareerEventUI IDS_Headline_Event_Won for
/// first place, IDS_Headline_Event_Lost otherwise). The show fires
/// SHOW_POSITION_TAPE_FIRST or _NON_FIRST; when the scene fires
/// SHOW_POSITION_TAPE_COMPLETE the update (0x82821420) waits
/// UI/RaceRewards/PositionWaitTime (globalregistry.xml, 1.0 s), fires
/// HIDE_POSITION_TAPE_FIRST or _NON_FIRST and plays Placement_SwooshOut,
/// and the scene's HIDE_POSITION_TAPE_COMPLETE ends it. No help bar and no
/// input offline.
///
/// Inferred: that the results flag +432 the wait needs is set once the
/// player has finished, as every offline show here is. Not drawn: the
/// online session's timer (COMPONENT_TIMER). The tape keeps its authored
/// length (see the source for why its stacker is not run).
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
    /// The UI sounds the screen's code asked for since the last call
    /// (Placement_SwooshOut as the tape goes); the scene's own
    /// PLAY_SOUND_EVENT behaviours are not included.
    std::vector<std::string> takeSounds();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace openhorizon::ui
