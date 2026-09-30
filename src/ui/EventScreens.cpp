#include "EventScreens.h"

#include "Presentation.h"
#include "SuperStacker.h"
#include "UiRenderer.h"
#include "VectorFont.h"

#include "base/Bytes.h"
#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <format>
#include <optional>
#include <utility>

namespace openhorizon::ui {

namespace {

constexpr const char* kSummaryScene = "media/ui/scenes/ui4/h_event_summary";
constexpr const char* kTitleCardScene = "media/ui/scenes/ui4/pre_and_post_race";
constexpr const char* kPreRaceScene = "media/ui/scenes/ui4/h_generic_title";
constexpr const char* kPopupScene = "media/ui/scenes/ui4/h_message_popup";
constexpr const char* kFinishedScene = "media/ui/scenes/ui4/h_post_race_finished";

constexpr const char* kCareerEventUi = "CareerEventUI";
constexpr const char* kHelpButtons = "HelpButtons";
constexpr const char* kPauseMenu = "PauseMenu";
constexpr const char* kInGame = "InGame";
constexpr const char* kScreenTitles = "ScreenTitles";
constexpr const char* kPostRaceFlow = "PostRaceFlow";

/// UI/EventTitleMaxLongNameLength, which globalregistry.xml sets to the
/// executable's own default (the initialiser at 0x8322bbb8).
constexpr std::size_t kTitleMaxLongNameLength = 30;
/// CCareerEventTitle's update (0x8271c980) fires onDone once the stopwatch
/// its show started passes the float at 0x820468ec, 4.0; taken as seconds,
/// the stopwatch's unit elsewhere.
constexpr float kTitleCardSeconds = 4.0F;
/// UI/RaceRewards/PositionWaitTime's value in globalregistry.xml, used when
/// the file cannot be read.
constexpr float kPositionWaitTime = 1.0F;

constexpr int kStyleFestivalRace = 1;
constexpr int kStyleShowcase = 3;
constexpr int kStyleHeadlineRace = 6;

/// A value of media/gametunablesettings/globalregistry.xml, if it has it.
std::optional<float> registryValue(const Vfs& vfs, std::string_view id)
{
    const std::optional<Bytes> data = vfs.read("media/gametunablesettings/globalregistry.xml");
    pugi::xml_document document;
    if (!data || !document.load_buffer(data->data(), data->size())) {
        return std::nullopt;
    }
    for (const pugi::xml_node value : document.child("Registry").children("Value")) {
        if (value.attribute("id").as_string() == id) {
            return value.attribute("value").as_float();
        }
    }
    return std::nullopt;
}

/// Capitals as the name setter maps each character (0x82cbf8c0): ASCII
/// letters and the Latin-1 letters of UTF-8 text. Inferred: that the
/// table is an upper-case map, as every event title the game shows is in
/// capitals.
std::string upperCase(std::string_view text)
{
    std::string out(text);
    for (std::size_t i = 0; i < out.size(); ++i) {
        const auto c = static_cast<unsigned char>(out[i]);
        if (c >= 'a' && c <= 'z') {
            out[i] = static_cast<char>(c - 'a' + 'A');
        } else if (c == 0xC3 && i + 1 < out.size()) {
            const auto next = static_cast<unsigned char>(out[i + 1]);
            if (next >= 0xA0 && next <= 0xBE && next != 0xB7) {
                out[i + 1] = static_cast<char>(next - 0x20);
            }
            ++i;
        }
    }
    return out;
}

std::string replaceFirst(std::string text, std::string_view from, std::string_view to)
{
    if (const std::size_t at = text.find(from); at != std::string::npos) {
        text.replace(at, from.size(), to);
    }
    return text;
}

/// The texts, fonts and renderer every screen here draws with.
struct SceneScreen {
    SceneScreen(const Vfs& vfs, const std::string& language, const char* path)
        : strings(vfs, language)
        , fonts(vfs, language)
        , scene(SceneFiles::load(vfs, path))
        , renderer(vfs, fonts)
        , root(scene.contract(0, "UIContract"))
    {
    }

    /// The element a contract parameter of the root contract names.
    int element(std::string_view parameter) const { return scene.contractParameter(root, parameter); }
    void setText(int element, std::string text)
    {
        if (element >= 0) {
            scene.setText(element, std::move(text));
        }
    }
    void hide(int element)
    {
        if (element >= 0) {
            scene.setHidden(element, true);
        }
    }

    UiStrings strings;
    FontLibrary fonts;
    Presentation scene;
    UiRenderer renderer;
    int root = -1;
};

/// Draws a scene's items with the help bar's icons.
void drawScene(SceneScreen& screen, const HelpBar* help, int width, int height,
    const std::vector<SuperStackers*>& stackers, int textureMaterial = -1, const std::string& texture = {})
{
    for (const SuperStackers* s : stackers) {
        s->layout(screen.scene, screen.renderer);
    }
    if (help != nullptr) {
        help->layout(screen.scene, screen.renderer);
    }
    std::vector<DrawItem> items = screen.scene.evaluate();
    if (help != nullptr) {
        help->apply(items);
    }
    if (textureMaterial >= 0 && !texture.empty()) {
        for (DrawItem& item : items) {
            if (item.isText || item.material != textureMaterial) {
                continue;
            }
            if (item.model.layers.empty()) {
                item.model.layers.emplace_back();
            }
            item.model.layers.front().texture = texture;
            item.model.layers.front().imageName = texture;
        }
    }
    applyTextScale(items);
    screen.renderer.begin(width, height);
    screen.renderer.draw(items);
    screen.renderer.end();
}

/// The event ticket's folder under media/ui/textures/horizon/EventTickets
/// by CareerEventStyle, from the switch at 0x828ec658; empty for other
/// styles.
std::string_view ticketFolder(int style)
{
    switch (style) {
    case 1:
        return "FestivalRaces";
    case 2:
        return "StreetRaces";
    case 3:
        return "Showcases";
    case 4:
        return "RallyTickets";
    case 5:
        return "NemesisTickets";
    case 6:
        return "HeadlineRaces";
    default:
        return {};
    }
}

/// An option list of PAUSE_MENU_BUTTON entries in a MENU contract, as the
/// pause menu's (see PauseMenu), with the list events of the scrolling
/// list (0x82865c40) and its SIZE<n> (0x828b53e4).
class OptionList {
public:
    void bind(const Presentation& scene, int menuElement)
    {
        m_menu = scene.contract(menuElement, "MENU");
        const int list = scene.contractParameter(m_menu, "COMPONENT_LIST");
        const int listContract = scene.contract(list, "ANIMATED_SCROLLING_LIST");
        const int group = scene.contractParameter(listContract, "GROUP_ITEMS");
        if (group < 0) {
            throw DataError("event screens: no option list");
        }
        for (const int child : scene.children(group)) {
            Button button;
            button.contract = scene.contract(child, "PAUSE_MENU_BUTTON");
            if (button.contract < 0) {
                continue;
            }
            button.text = scene.contractParameter(button.contract, "TEXT_BUTTON");
            m_buttons.push_back(button);
        }
    }

    /// Shows `texts` in the list with `focus` focused.
    void show(Presentation& scene, const std::vector<std::string>& texts, int focus)
    {
        m_count = static_cast<int>(std::min(texts.size(), m_buttons.size()));
        for (std::size_t i = 0; i < m_buttons.size(); ++i) {
            const bool used = i < texts.size();
            scene.fireEvent(m_buttons[i].contract, used ? "SHOWN" : "HIDDEN");
            if (used && m_buttons[i].text >= 0) {
                scene.setText(m_buttons[i].text, texts[i]);
            }
            scene.fireEvent(m_buttons[i].contract, "HIDE_MESSAGES");
        }
        scene.fireEvent(m_menu, std::format("SIZE{}", m_count));
        m_focus = std::clamp(focus, 0, std::max(0, m_count - 1));
        for (int i = 0; i < m_count; ++i) {
            const char* event = i == m_focus ? "FOCUSED" : (i < m_focus ? "BLURREDABOVE" : "BLURREDBELOW");
            scene.fireEvent(m_buttons[static_cast<std::size_t>(i)].contract, event);
        }
    }

    /// Moves the focus by one; the list does not wrap (BOOLEAN_WRAP_DATA
    /// is false in the scene).
    void move(Presentation& scene, int step)
    {
        const int next = m_focus + step;
        if (next < 0 || next >= m_count) {
            return;
        }
        const int previous = m_focus;
        m_focus = next;
        scene.fireEvent(
            m_buttons[static_cast<std::size_t>(previous)].contract, previous < m_focus ? "BLURABOVE" : "BLURBELOW");
        scene.fireEvent(
            m_buttons[static_cast<std::size_t>(m_focus)].contract, m_focus < previous ? "FOCUSABOVE" : "FOCUSBELOW");
    }

    int focus() const { return m_focus; }
    int count() const { return m_count; }

private:
    struct Button {
        int contract = -1;
        int text = -1;
    };
    int m_menu = -1;
    std::vector<Button> m_buttons;
    int m_focus = 0;
    int m_count = 0;
};

} // namespace

std::string eventTitle(const EventScreenInfo& info)
{
    const bool useShort = info.name.size() > kTitleMaxLongNameLength && !info.shortName.empty();
    return upperCase(useShort ? info.shortName : info.name);
}

std::string eventKindLine(const EventScreenInfo& info, UiStrings& strings)
{
    static constexpr std::array<const char*, 6> kIds{"IDS_TitleFestivalRace", "IDS_TitleStreetRace",
        "IDS_TitleExhibition", "IDS_TitleRally", "IDS_TitleNemesisRace", "IDS_TitleHeadlineRace"};
    if (info.careerEventStyle < 1 || info.careerEventStyle > static_cast<int>(kIds.size())) {
        return {};
    }
    const char* id = kIds[static_cast<std::size_t>(info.careerEventStyle - 1)];
    // IDS_TitleRally belongs to the rally expansion, whose CareerEventUI
    // entry the disc lacks; the kind line then stays empty.
    return strings.has(kCareerEventUi, id) ? strings.get(kCareerEventUi, id) : std::string();
}

std::string placeOrdinal(int place, UiStrings& strings)
{
    int suffix = place % 100;
    if (suffix >= 20) {
        suffix %= 10;
    }
    return std::format("{}{}", place, strings.get(kInGame, std::format("IDS_Place_{}", suffix)));
}

struct EventSummaryScreen::Impl {
    Impl(const Vfs& vfs, const std::string& language)
        : screen(vfs, language, kSummaryScene)
    {
        help.bind(screen.scene, screen.renderer, screen.element("COMPONENT_HELP_BUTTON_BAR"));
        const int rotation = screen.scene.find("Layer/Rotation");
        stackers.bind(screen.scene, screen.renderer, rotation);
        ticketMaterial = screen.element("MATERIAL_TICKET_BG");
    }

    SceneScreen screen;
    HelpBar help;
    SuperStackers stackers;
    int ticketMaterial = -1;
    std::string ticket;
    bool open = false;
    bool drawn = false;
};

EventSummaryScreen::EventSummaryScreen(const Vfs& vfs, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, language))
{
}

EventSummaryScreen::~EventSummaryScreen() = default;

void EventSummaryScreen::open(const EventScreenInfo& info)
{
    Impl& impl = *m_impl;
    SceneScreen& s = impl.screen;
    s.scene.reset();
    // The show handler (0x82733a68).
    if (info.playerCarId != 0 && !info.providedCarName.empty()) {
        s.setText(s.element("TEXT_CAR_TITLE"), info.providedCarName);
    }
    s.setText(s.element("TEXT_DESCRIPTION"), info.description);
    s.setText(s.element("TEXT_TITLE"), eventKindLine(info, s.strings));
    s.setText(s.element("TEXT_CAR_RESTRICTIONS"), info.restrictionDescription);
    s.setText(s.element("TEXT_RESTRICTION_TITLE"), s.strings.get(kCareerEventUi, "IDS_EventRestrictions"));
    s.setText(s.element("TEXT_RESTRICTION_LIST"), info.restrictionDescription);
    if (info.careerEventStyle == kStyleHeadlineRace) {
        s.setText(s.element("TEXT_POINTS_TITLE"), s.strings.get(kCareerEventUi, "IDS_WinnerTakesAll"));
        s.setText(s.element("TEXT_POINTS_VALUE"), s.strings.get(kCareerEventUi, "IDS_HorizonChampion_Line1"));
        s.setText(s.element("TEXT_WRISTBAND"), s.strings.get(kCareerEventUi, "IDS_HorizonChampion_Line2"));
    } else {
        s.setText(s.element("TEXT_POINTS_TITLE"), s.strings.get(kCareerEventUi, "IDS_PointsAvailable"));
        // The points (from Events.Level and WristbandScoring, 0x8253b230)
        // go through a conversion not traced; their tape is left out rather
        // than show the scene's placeholder digits, and its heading with it.
        s.hide(s.scene.find("Layer/Rotation/Ticket/Info/Points"));
        s.hide(s.element("TEXT_POINTS_TITLE"));
    }
    // The prize (Events.CashPrize through 0x826984e8) is converted by code
    // not traced either.
    s.hide(s.scene.find("Layer/Rotation/Ticket/Info/Credits"));
    // The race and track lines take values from the event-info record
    // (0x828f7b90, +36 and +40: a distance and its unit, whose formatting
    // was not traced) and a street race's place name whose source was not
    // traced; the car lines and the eligibility need the player's garage,
    // and the class icons the event's class (0x82704e38) behind a flag not
    // traced. All are left out.
    for (const char* path : {"Layer/Rotation/Ticket/Info/RaceInfo", "Layer/Rotation/Ticket/Info/TrackInfo",
             "Layer/Rotation/Ticket/Info/CarInfo", "Layer/Rotation/Ticket/Info/CarClass/TEXT_ELIGIBILITY",
             "Layer/Rotation/Ticket/Info/CarClass/ClassIcon", "Layer/Rotation/History",
             "Layer/Rotation/Ticket/Info/ExtraInfo", "Layer/GROUP_TEST_TEXT"}) {
        s.hide(s.scene.find(path));
    }
    // The ticket (0x828ee038): the event's EventTicket image in its style's
    // folder.
    impl.ticket.clear();
    if (const std::string_view folder = ticketFolder(info.careerEventStyle); !folder.empty() && !info.ticket.empty()) {
        impl.ticket = texturePathForScene(
            std::format("game:\\media\\ui\\textures\\Horizon\\EventTickets\\{}\\{}.tga", folder, info.ticket));
    }
    const char* scheme = "SHOW_FESTIVAL_RACE";
    if (info.careerEventStyle == kStyleHeadlineRace) {
        scheme = "SHOW_FINAL";
    } else if (info.careerEventStyle == kStyleShowcase) {
        scheme = "SHOW_EXHIBITION";
    } else if (info.careerEventStyle == kStyleFestivalRace && info.level < 0) {
        scheme = "SHOW_HEATS";
    }
    s.scene.fireEvent(s.root, scheme);
    // The player bar shows the profile's level, car and credits, which the
    // screen is not given; its HIDE slide hides it, as the pause menu's is.
    s.scene.goToSlide(s.scene.find("Layer/PlayerBar"), "HIDE");
    // 0x82728138: ENTER EVENT on A with the flow's okCareer, BACK on B.
    impl.help.set(s.scene,
        {{HelpIcon::A, s.strings.get(kHelpButtons, "IDS_EnterEvent")},
            {HelpIcon::B, s.strings.get(kHelpButtons, "IDS_Back")}});
    impl.open = true;
    impl.drawn = false;
}

void EventSummaryScreen::close()
{
    m_impl->open = false;
}

bool EventSummaryScreen::isOpen() const
{
    return m_impl->open;
}

EventSummaryScreen::Action EventSummaryScreen::update(const MenuInput& input, float dt)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return Action::None;
    }
    Action action = Action::None;
    if (input.accept) {
        action = Action::Enter;
    } else if (input.back) {
        action = Action::Back;
    }
    impl.screen.scene.update(dt);
    impl.screen.scene.takeFiredEvents();
    if (action != Action::None) {
        close();
    }
    return action;
}

void EventSummaryScreen::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return;
    }
    drawScene(impl.screen, &impl.help, width, height, {&impl.stackers}, impl.ticketMaterial, impl.ticket);
    impl.drawn = true;
}

std::string EventSummaryScreen::text(std::string_view contractParameter) const
{
    const Impl& impl = *m_impl;
    const int element = impl.screen.element(contractParameter);
    return element >= 0 ? impl.screen.scene.text(element) : std::string();
}

const std::string& EventSummaryScreen::ticket() const
{
    return m_impl->ticket;
}

std::string EventSummaryScreen::slide() const
{
    return m_impl->screen.scene.currentSlide(0);
}

std::vector<HelpButtonExtent> EventSummaryScreen::helpExtents() const
{
    Impl& impl = *m_impl;
    if (!impl.drawn) {
        return {};
    }
    return impl.help.extents(impl.screen.scene, impl.screen.renderer, impl.screen.scene.evaluate());
}

struct EventTitleCard::Impl {
    Impl(const Vfs& vfs, const std::string& language)
        : screen(vfs, language, kTitleCardScene)
    {
        const int barElement = screen.element("COMPONENT_EVENT_BAR");
        bar = screen.scene.contract(barElement, "EVENT_BAR");
        title = screen.scene.contractParameter(bar, "TEXT_TITLE");
        subtitle = screen.scene.contractParameter(bar, "TEXT_SUBTITLE");
        if (bar < 0 || title < 0) {
            throw DataError("{}: no event bar", kTitleCardScene);
        }
        stackers.bind(screen.scene, screen.renderer, barElement);
        help.bind(screen.scene, screen.renderer, screen.element("COMPONENT_HELP_BUTTON_BAR"));
    }

    SceneScreen screen;
    int bar = -1;
    int title = -1;
    int subtitle = -1;
    SuperStackers stackers;
    HelpBar help;
    float elapsed = 0.0F;
    bool shown = false;
};

EventTitleCard::EventTitleCard(const Vfs& vfs, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, language))
{
}

EventTitleCard::~EventTitleCard() = default;

void EventTitleCard::show(const EventScreenInfo& info)
{
    Impl& impl = *m_impl;
    SceneScreen& s = impl.screen;
    s.scene.reset();
    // 0x82728580.
    s.setText(impl.title, eventTitle(info));
    s.setText(impl.subtitle, eventKindLine(info, s.strings));
    // The card binds no help bar and takes no input; the scene's bar is
    // left with every slot hidden (inferred: the screen base's empty bar).
    impl.help.set(s.scene, {});
    // The scene's winnings (TEXT_WINNINGS, TEXT_WINNING: the root
    // contract's, which a post-race screen fills) are not the card's; its
    // show leaves them unset, so they are not drawn (inferred: the card
    // shows its event bar alone).
    s.hide(s.scene.find("Layer/Winnings"));
    s.scene.fireEvent(impl.bar, "EVENT");
    impl.elapsed = 0.0F;
    impl.shown = true;
}

void EventTitleCard::hide()
{
    m_impl->shown = false;
}

bool EventTitleCard::isShown() const
{
    return m_impl->shown;
}

bool EventTitleCard::update(float dt)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return false;
    }
    impl.screen.scene.update(dt);
    impl.screen.scene.takeFiredEvents();
    const bool wasDone = impl.elapsed >= kTitleCardSeconds;
    impl.elapsed += dt;
    return !wasDone && impl.elapsed >= kTitleCardSeconds;
}

void EventTitleCard::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return;
    }
    drawScene(impl.screen, &impl.help, width, height, {&impl.stackers});
}

std::string EventTitleCard::title() const
{
    return m_impl->screen.scene.text(m_impl->title);
}

std::string EventTitleCard::subtitle() const
{
    const Impl& impl = *m_impl;
    return impl.subtitle >= 0 ? impl.screen.scene.text(impl.subtitle) : std::string();
}

struct PreRaceMenu::Impl {
    Impl(const Vfs& vfs, const std::string& language)
        : screen(vfs, language, kPreRaceScene)
        , popup(SceneFiles::load(vfs, kPopupScene))
    {
        Presentation& scene = screen.scene;
        const int menuElement = screen.element("COMPONENT_MENU");
        list.bind(scene, menuElement);
        listStackers.bind(scene, screen.renderer, menuElement);
        const int titlesElement = screen.element("COMPONENT_TITLES");
        titles = scene.contract(titlesElement, "MENU_TITLES");
        title = scene.contractParameter(titles, "TITLE");
        subtitle = scene.contractParameter(titles, "SUBTITLE");
        titleStackers.bind(scene, screen.renderer, titlesElement);
        help.bind(scene, screen.renderer, screen.element("COMPONENT_HELP_BUTTON_BAR"));

        const int popupRoot = popup.contract(0, "UIContract");
        const int box = popup.contract(popup.contractParameter(popupRoot, "MESSAGE_BOX"), "MESSAGE_BOX");
        popupTitle = popup.contractParameter(box, "TEXT_TITLE");
        popupMessage = popup.contractParameter(box, "TEXT_MESSAGE");
        popupHelp.bind(popup, screen.renderer, popup.contractParameter(popupRoot, "COMPONENT_HELP_BUTTON_BAR"));
        popupWrapWidth = textBoxWidth(popup, popupMessage);
        popupStyle = textStyle(popup, popupMessage);
    }

    void openConfirmation()
    {
        UiStrings& strings = screen.strings;
        popup.reset();
        // 0x8281d568: screen 242 of type 4 with PauseMenu's QUIT RACE and
        // the career text (the rally and rivals texts belong to modes this
        // reimplementation does not have).
        if (popupTitle >= 0) {
            popup.setText(popupTitle, strings.get(kPauseMenu, "IDS_QuitRace"));
        }
        if (popupMessage >= 0) {
            std::string message = strings.get(kPauseMenu, "IDS_QuitRace_Desc_Career");
            if (popupWrapWidth > 0.0F && !popupStyle.font.empty()) {
                message = wrapText(message, popupWrapWidth, [&](const std::string& line) {
                    return screen.renderer.measure(popupStyle.font, line, popupStyle.size * kTextPointsToUnits);
                });
            }
            popup.setText(popupMessage, std::move(message));
        }
        // The buttons of message box type 4 were not traced; the pause
        // menu's retire box (0x827eae30), which asks the same in a race,
        // has OK on A and CANCEL on B, and this one takes them (inferred).
        popupHelp.set(popup,
            {{HelpIcon::A, strings.get(kHelpButtons, "IDS_OK")},
                {HelpIcon::B, strings.get(kHelpButtons, "IDS_Cancel")}});
        help.enable(screen.scene, false);
        confirming = true;
    }

    SceneScreen screen;
    Presentation popup;
    OptionList list;
    SuperStackers listStackers;
    SuperStackers titleStackers;
    int titles = -1;
    int title = -1;
    int subtitle = -1;
    HelpBar help;
    int popupTitle = -1;
    int popupMessage = -1;
    float popupWrapWidth = 0.0F;
    TextStyle popupStyle;
    HelpBar popupHelp;
    std::vector<PreRaceEntry> entries;
    bool open = false;
    bool confirming = false;
    bool drawn = false;
};

PreRaceMenu::PreRaceMenu(const Vfs& vfs, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, language))
{
}

PreRaceMenu::~PreRaceMenu() = default;

std::vector<PreRaceEntry> preRaceEntries(UiStrings& strings)
{
    // CPreRace's show handler (0x8281cf68) outside the rivals mode.
    // DIFFICULTY is listed only when 0x82593190 is true and 0x82583db8
    // false, checks whose meaning was not traced; it is left out.
    return {{PreRaceItem::StartRace, strings.get(kInGame, "IDS_RaceMenu")},
        {PreRaceItem::Options, strings.get(kPauseMenu, "IDS_Options")},
        {PreRaceItem::QuitRace, strings.get(kInGame, "IDS_QuitRaceMenu")}};
}

void PreRaceMenu::open(const EventScreenInfo& info)
{
    Impl& impl = *m_impl;
    SceneScreen& s = impl.screen;
    s.scene.reset();
    impl.entries = preRaceEntries(s.strings);
    // MENU_TITLES (0x82664ca0): the name over the kind line, TWO_TITLES
    // when there is a kind line, ONE_TITLE otherwise.
    const std::string kind = eventKindLine(info, s.strings);
    s.setText(impl.title, eventTitle(info));
    s.setText(impl.subtitle, kind);
    s.scene.fireEvent(impl.titles, kind.empty() ? "ONE_TITLE" : "TWO_TITLES");
    std::vector<std::string> texts;
    texts.reserve(impl.entries.size());
    for (const PreRaceEntry& entry : impl.entries) {
        texts.push_back(entry.text);
    }
    impl.list.show(s.scene, texts, 0);
    // 0x82817538: SELECT on A; BACK on B only with flag +437, which nothing
    // found sets.
    impl.help.set(s.scene, {{HelpIcon::A, s.strings.get(kHelpButtons, "IDS_Select")}});
    impl.confirming = false;
    impl.open = true;
    impl.drawn = false;
}

void PreRaceMenu::close()
{
    m_impl->open = false;
    m_impl->confirming = false;
}

bool PreRaceMenu::isOpen() const
{
    return m_impl->open;
}

PreRaceMenu::Action PreRaceMenu::update(const MenuInput& input, float dt)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return Action::None;
    }
    Action action = Action::None;
    Presentation& scene = impl.screen.scene;
    if (impl.confirming) {
        if (input.accept) {
            // Inferred: confirming the box fires the flow's quit.
            action = Action::Quit;
        } else if (input.back) {
            impl.confirming = false;
            impl.help.enable(scene, true);
        }
    } else if (input.accept && impl.list.count() > 0) {
        // The select handler (0x82821b00, jump table 0x820662e0).
        switch (impl.entries[static_cast<std::size_t>(impl.list.focus())].item) {
        case PreRaceItem::StartRace:
            action = Action::Race;
            break;
        case PreRaceItem::QuitRace:
            impl.openConfirmation();
            break;
        case PreRaceItem::Options:
        case PreRaceItem::Difficulty:
            action = Action::Options;
            break;
        }
    } else if (input.up) {
        impl.list.move(scene, -1);
    } else if (input.down) {
        impl.list.move(scene, 1);
    }
    scene.update(dt);
    scene.takeFiredEvents();
    if (impl.confirming) {
        impl.popup.update(dt);
        impl.popup.takeFiredEvents();
    }
    if (action == Action::Race || action == Action::Quit) {
        close();
    }
    return action;
}

void PreRaceMenu::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return;
    }
    drawScene(impl.screen, &impl.help, width, height, {&impl.listStackers, &impl.titleStackers});
    if (impl.confirming) {
        impl.popupHelp.layout(impl.popup, impl.screen.renderer);
        std::vector<DrawItem> items = impl.popup.evaluate();
        impl.popupHelp.apply(items);
        applyTextScale(items);
        impl.screen.renderer.begin(width, height);
        impl.screen.renderer.draw(items);
        impl.screen.renderer.end();
    }
    impl.drawn = true;
}

const std::vector<PreRaceEntry>& PreRaceMenu::entries() const
{
    return m_impl->entries;
}

int PreRaceMenu::focus() const
{
    return m_impl->list.focus();
}

bool PreRaceMenu::confirming() const
{
    return m_impl->confirming;
}

std::string PreRaceMenu::title() const
{
    const Impl& impl = *m_impl;
    return impl.title >= 0 ? impl.screen.scene.text(impl.title) : std::string();
}

std::string PreRaceMenu::subtitle() const
{
    const Impl& impl = *m_impl;
    return impl.subtitle >= 0 ? impl.screen.scene.text(impl.subtitle) : std::string();
}

std::vector<HelpButtonExtent> PreRaceMenu::helpExtents() const
{
    Impl& impl = *m_impl;
    if (!impl.drawn) {
        return {};
    }
    return impl.help.extents(impl.screen.scene, impl.screen.renderer, impl.screen.scene.evaluate());
}

struct PostRaceFinishedScreen::Impl {
    Impl(const Vfs& vfs, const std::string& language)
        : screen(vfs, language, kFinishedScene)
        , waitTime(registryValue(vfs, "UI/RaceRewards/PositionWaitTime").value_or(kPositionWaitTime))
    {
        // Only the title's stackers run: the position tape's SUPER_STACKER
        // places TEXT_POSITION, which the IN and OUT slides animate, and
        // SuperStackers writes x over the slides' tracks from the loaded
        // position, which lies off screen. The tape keeps its authored
        // length instead.
        stackers.bind(screen.scene, screen.renderer, screen.scene.find("Layer/RotationHelper/ScreenTitle"));
        help.bind(screen.scene, screen.renderer, screen.element("COMPONENT_HELP_BUTTON_BAR"));
    }

    SceneScreen screen;
    float waitTime;
    SuperStackers stackers;
    HelpBar help;
    bool first = false;
    bool shown = false;
    /// Seconds left before the tape goes, once the scene said it is in.
    std::optional<float> countdown;
    std::vector<std::string> sounds;
};

PostRaceFinishedScreen::PostRaceFinishedScreen(const Vfs& vfs, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, language))
{
}

PostRaceFinishedScreen::~PostRaceFinishedScreen() = default;

void PostRaceFinishedScreen::show(int place, int careerEventStyle)
{
    Impl& impl = *m_impl;
    SceneScreen& s = impl.screen;
    s.scene.reset();
    s.setText(s.element("TEXT_SCREEN_TITLE"), s.strings.get(kScreenTitles, "IDS_PostRace_Finished"));
    s.setText(s.element("TEXT_ONLINE"), std::string());
    // COMPONENT_TIMER counts down the online session's results; offline
    // the screen has none to show.
    s.hide(s.element("COMPONENT_TIMER"));
    std::string position;
    if (careerEventStyle == kStyleHeadlineRace) {
        position = s.strings.get(kCareerEventUi, place == 1 ? "IDS_Headline_Event_Won" : "IDS_Headline_Event_Lost");
    } else {
        position = replaceFirst(s.strings.get(kPostRaceFlow, "IDS_Place"), "{0}", placeOrdinal(place, s.strings));
    }
    s.setText(s.element("TEXT_POSITION"), position);
    // No help bar on the offline path: every slot hidden.
    impl.help.set(s.scene, {});
    impl.first = place == 1;
    s.scene.fireEvent(s.root, impl.first ? "SHOW_POSITION_TAPE_FIRST" : "SHOW_POSITION_TAPE_NON_FIRST");
    impl.countdown.reset();
    impl.sounds.clear();
    impl.shown = true;
}

void PostRaceFinishedScreen::hide()
{
    m_impl->shown = false;
}

bool PostRaceFinishedScreen::isShown() const
{
    return m_impl->shown;
}

bool PostRaceFinishedScreen::update(float dt)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return false;
    }
    SceneScreen& s = impl.screen;
    // The update (0x82821420): the countdown runs on the frame's time.
    if (impl.countdown) {
        *impl.countdown -= dt;
        if (*impl.countdown <= 0.0F) {
            impl.countdown.reset();
            s.scene.fireEvent(s.root, impl.first ? "HIDE_POSITION_TAPE_FIRST" : "HIDE_POSITION_TAPE_NON_FIRST");
            impl.sounds.emplace_back("Placement_SwooshOut");
        }
    }
    s.scene.update(dt);
    bool done = false;
    for (const Presentation::FiredEvent& event : s.scene.takeFiredEvents()) {
        if (event.event == "SHOW_POSITION_TAPE_COMPLETE") {
            impl.countdown = impl.waitTime;
        } else if (event.event == "HIDE_POSITION_TAPE_COMPLETE") {
            done = true;
        }
    }
    if (done) {
        impl.shown = false;
    }
    return done;
}

void PostRaceFinishedScreen::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.shown) {
        return;
    }
    drawScene(impl.screen, &impl.help, width, height, {&impl.stackers});
}

std::string PostRaceFinishedScreen::title() const
{
    const SceneScreen& s = m_impl->screen;
    const int element = s.element("TEXT_SCREEN_TITLE");
    return element >= 0 ? s.scene.text(element) : std::string();
}

std::string PostRaceFinishedScreen::position() const
{
    const SceneScreen& s = m_impl->screen;
    const int element = s.element("TEXT_POSITION");
    return element >= 0 ? s.scene.text(element) : std::string();
}

std::vector<std::string> PostRaceFinishedScreen::takeSounds()
{
    return std::exchange(m_impl->sounds, {});
}

} // namespace openhorizon::ui
