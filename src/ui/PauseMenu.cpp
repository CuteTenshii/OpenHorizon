#include "PauseMenu.h"

#include "Presentation.h"
#include "UiRenderer.h"
#include "VectorFont.h"

#include "base/Error.h"
#include "base/Log.h"

#include <algorithm>
#include <format>
#include <optional>

namespace fh::ui {

namespace {

constexpr const char* kPauseScene = "media/ui/scenes/ui4/925_pause_menu";
constexpr const char* kPopupScene = "media/ui/scenes/ui4/h_message_popup";

/// The tables the builder passes to the text lookup with each id.
constexpr const char* kPauseMenu = "PauseMenu";
constexpr const char* kHelpButtons = "HelpButtons";
constexpr const char* kScreenTitles = "ScreenTitles";
constexpr const char* kMain = "Main";
constexpr const char* kCarChallenges = "CarChallenges";

/// A confirmation the menu raises before acting (message box, screen 222).
struct Confirmation {
    std::string title;
    std::string message;
    std::string confirm;
    std::string cancel;
    PauseAction action = PauseAction::None;
};

} // namespace

std::vector<PauseMenuEntry> pauseMenuEntries(const PauseMenuContext& context, UiStrings& strings)
{
    std::vector<PauseMenuEntry> out;
    const auto add = [&](PauseItem item, const char* table, const char* id) {
        if (strings.has(table, id)) {
            out.push_back({item, strings.get(table, id)});
        }
    };
    if (context.firstTimeCareer) {
        add(PauseItem::Resume, kHelpButtons, "IDS_Resume");
        add(PauseItem::Quit, kPauseMenu, "IDS_Quit");
        return out;
    }
    if (context.inMission) {
        add(PauseItem::RestartMission, kPauseMenu, "IDS_RestartMission");
    }
    if (!context.freeRoam) {
        add(PauseItem::Restart, kPauseMenu, "IDS_Restart");
    }
    if (context.canFinish) {
        add(PauseItem::Finish, kHelpButtons, "IDS_Finish");
    }
    if (context.mapAvailable) {
        add(PauseItem::Map, kPauseMenu, "IDS_Map");
    }
    // CAR CHALLENGES and HORIZON RALLY come with downloadable content whose
    // tables are not on the disc; `add` leaves them out when their text is
    // missing, which it always is here.
    add(PauseItem::CarChallenges, kCarChallenges, "IDS_PauseMenu_CarChallenges");
    if (context.freeRoam) {
        add(PauseItem::HorizonRally, kMain, "IDS_HorizonRally");
    }
    if (context.multiplayerAvailable && !context.inMission) {
        add(PauseItem::Multiplayer, kPauseMenu, "IDS_Multiplayer");
    }
    if (context.photoModeAvailable) {
        add(PauseItem::PhotoMode, kPauseMenu, "IDS_Photo");
    }
    if ((context.freeRoam || context.messageCentreOutsideFreeRoam) && !context.inMission) {
        add(PauseItem::MessageCentre, kPauseMenu, "IDS_MessageCentre");
    }
    if (context.sponsorChallenges) {
        add(PauseItem::SponsorChallenges, kPauseMenu, "IDS_SponsorChallenge");
    }
    add(PauseItem::MyProfile, kPauseMenu, "IDS_MyProfile");
    if (context.headTracking) {
        add(PauseItem::HeadTracking, kPauseMenu, "IDS_HeadTracking");
    }
    if ((!context.freeRoam || context.freeRoamActivity) && !context.canFinish) {
        add(PauseItem::QuitRace, kPauseMenu, "IDS_QuitRace");
    }
    if (context.inMission) {
        add(PauseItem::QuitMission, kPauseMenu, "IDS_QuitMission");
    } else if (context.multiplayerAvailable && !context.freeRoamActivity) {
        add(PauseItem::Quit, kPauseMenu, "IDS_Quit");
    }
    return out;
}

struct PauseMenu::Impl {
    Impl(const Vfs& vfs, const std::string& language);

    void bindPause();
    void bindPopup();
    void show();
    void setFocus(int index, bool opening);
    PauseAction select();
    PauseAction confirm(bool accepted);
    void openConfirmation(const Confirmation& confirmation);

    UiStrings strings;
    FontLibrary fonts;
    Presentation pause;
    Presentation popup;
    UiRenderer renderer;

    int rootContract = -1;
    int menuContract = -1;
    int title = -1;
    struct Button {
        int contract = -1;
        int text = -1;
        int element = -1;
    };
    std::vector<Button> buttons;
    HelpBar pauseHelp;

    int popupTitle = -1;
    int popupMessage = -1;
    float popupWrapWidth = 0.0F;
    std::string popupFont;
    float popupFontSize = 0.0F;
    HelpBar popupHelp;

    PauseMenuContext context;
    std::vector<PauseMenuEntry> entries;
    int focus = 0;
    bool open = false;
    std::optional<Confirmation> confirmation;
};

PauseMenu::Impl::Impl(const Vfs& vfs, const std::string& language)
    : strings(vfs, language)
    , fonts(vfs, language)
    , pause(SceneFiles::load(vfs, kPauseScene))
    , popup(SceneFiles::load(vfs, kPopupScene))
    , renderer(vfs, fonts)
{
    bindPause();
    bindPopup();
}

void PauseMenu::Impl::bindPause()
{
    rootContract = pause.contract(0, "UIContract");
    title = pause.contractParameter(rootContract, "TEXT_SCREEN_TITLE");
    const int optionList = pause.contractParameter(rootContract, "COMPONENT_OPTION_LIST");
    menuContract = pause.contract(optionList, "MENU");
    const int list = pause.contractParameter(menuContract, "COMPONENT_LIST");
    const int listContract = pause.contract(list, "ANIMATED_SCROLLING_LIST");
    const int group = pause.contractParameter(listContract, "GROUP_ITEMS");
    if (group < 0) {
        throw DataError("{}: no option list", kPauseScene);
    }
    for (const int child : pause.children(group)) {
        Button button;
        button.contract = pause.contract(child, "PAUSE_MENU_BUTTON");
        if (button.contract < 0) {
            continue;
        }
        button.element = child;
        button.text = pause.contractParameter(button.contract, "TEXT_BUTTON");
        buttons.push_back(button);
    }
    pauseHelp.bind(pause, pause.contractParameter(rootContract, "COMPONENT_HELP_BUTTON_BAR"));
}

void PauseMenu::Impl::bindPopup()
{
    const int root = popup.contract(0, "UIContract");
    const int box = popup.contractParameter(root, "MESSAGE_BOX");
    const int boxContract = popup.contract(box, "MESSAGE_BOX");
    popupTitle = popup.contractParameter(boxContract, "TEXT_TITLE");
    popupMessage = popup.contractParameter(boxContract, "TEXT_MESSAGE");
    popupHelp.bind(popup, popup.contractParameter(root, "COMPONENT_HELP_BUTTON_BAR"));
    if (popupMessage >= 0) {
        // The message element has wordwrap set and a boxwidth of 0.313,
        // which, read as a fraction of the 1280-unit design width, spans
        // the popup's panel (inferred from that fit).
        const BgfElement& element = popup.files().bgf.elements[static_cast<std::size_t>(popupMessage)];
        const std::uint32_t boxWidth = anarkHash("boxwidth");
        for (const Attribute& attribute : element.attributes) {
            if (attribute.hash == boxWidth) {
                popupWrapWidth = attribute.asFloat() * static_cast<float>(popup.files().bgf.width);
            }
        }
        const std::uint32_t id = element.id;
        for (const FbfRecord& record : popup.files().fbf.records) {
            if (record.id == id) {
                popupFont = record.text.font;
                popupFontSize = static_cast<float>(record.text.size);
            }
        }
    }
}

void PauseMenu::Impl::show()
{
    pause.reset();
    if (title >= 0) {
        pause.setText(title, strings.get(kScreenTitles, "IDS_Pause"));
    }
    // 0x827dfb18: the target time line is hidden unless a timed event sets
    // it; the multiplayer timer only runs in multiplayer.
    pause.fireEvent(rootContract, "HIDETARGET");
    if (const int timer = pause.contractParameter(rootContract, "TEXT_MULTIPLAYER_TIMER"); timer >= 0) {
        pause.setHidden(timer, true);
    }
    // The player bar shows the driver's level, car and credits, which this
    // reimplementation does not keep; the scene's own HIDE hides it.
    const int playerBar = pause.find("Layer/PlayerBar");
    for (const int child : pause.children(playerBar)) {
        if (pause.kind(child) == ElementKind::Behavior) {
            pause.fireEvent(child, "HIDE");
        }
    }
    pauseHelp.set(pause,
        {{HelpIcon::A, strings.get(kHelpButtons, "IDS_Select")},
            {HelpIcon::B, strings.get(kHelpButtons, "IDS_Resume")}});
    for (std::size_t i = 0; i < buttons.size(); ++i) {
        const bool used = i < entries.size();
        pause.fireEvent(buttons[i].contract, used ? "SHOWN" : "HIDDEN");
        if (used && buttons[i].text >= 0) {
            pause.setText(buttons[i].text, entries[i].text);
        }
        pause.fireEvent(buttons[i].contract, "HIDE_MESSAGES");
    }
    // 0x828b53e4: SIZE<n> with n the entries shown, at most the slots.
    const std::size_t shown = std::min(entries.size(), buttons.size());
    pause.fireEvent(menuContract, std::format("SIZE{}", shown));
    if (entries.size() > buttons.size()) {
        logWarning(
            "ui: {} pause entries for {} buttons; the list does not scroll here", entries.size(), buttons.size());
    }
    setFocus(std::clamp(focus, 0, std::max(0, static_cast<int>(shown) - 1)), true);
    open = true;
}

void PauseMenu::Impl::setFocus(int index, bool opening)
{
    const int count = static_cast<int>(std::min(entries.size(), buttons.size()));
    if (count == 0) {
        return;
    }
    const int previous = focus;
    focus = index;
    if (opening) {
        for (int i = 0; i < count; ++i) {
            const char* event = i == focus ? "FOCUSED" : (i < focus ? "BLURREDABOVE" : "BLURREDBELOW");
            pause.fireEvent(buttons[static_cast<std::size_t>(i)].contract, event);
        }
        return;
    }
    if (previous == focus) {
        return;
    }
    pause.fireEvent(buttons[static_cast<std::size_t>(previous)].contract, previous < focus ? "BLURABOVE" : "BLURBELOW");
    pause.fireEvent(buttons[static_cast<std::size_t>(focus)].contract, focus < previous ? "FOCUSABOVE" : "FOCUSBELOW");
}

void PauseMenu::Impl::openConfirmation(const Confirmation& value)
{
    confirmation = value;
    popup.reset();
    if (popupTitle >= 0) {
        popup.setText(popupTitle, value.title);
    }
    if (popupMessage >= 0) {
        std::string message = value.message;
        if (popupWrapWidth > 0.0F && !popupFont.empty()) {
            message = wrapText(message, popupWrapWidth,
                [&](const std::string& line) { return renderer.measure(popupFont, line, popupFontSize); });
        }
        popup.setText(popupMessage, std::move(message));
    }
    popupHelp.set(popup, {{HelpIcon::A, value.confirm}, {HelpIcon::B, value.cancel}});
    // The message box's bar takes the place of the menu's, which the menu
    // hides with its bar's DISABLED while the box is up (the game's screen
    // stack was not traced for this).
    pauseHelp.enable(pause, false);
}

PauseAction PauseMenu::Impl::select()
{
    const PauseItem item = entries[static_cast<std::size_t>(focus)].item;
    switch (item) {
    case PauseItem::Resume:
        return PauseAction::Resume;
    case PauseItem::Map:
        return PauseAction::OpenMap;
    case PauseItem::Restart:
        // 0x827ea1c0: the single-player race text; the mulligan choice it
        // adds when a rewind is available is not offered here.
        openConfirmation({strings.get(kPauseMenu, "IDS_Restart"), strings.get(kPauseMenu, "IDS_RestartDesc"),
            strings.get(kHelpButtons, "IDS_Yes"), strings.get(kHelpButtons, "IDS_No"), PauseAction::Restart});
        return PauseAction::None;
    case PauseItem::QuitRace:
        // 0x827eae30.
        openConfirmation({strings.get(kPauseMenu, "IDS_RetireRace"), strings.get(kPauseMenu, "IDS_RetireRace_Desc"),
            strings.get(kHelpButtons, "IDS_OK"), strings.get(kHelpButtons, "IDS_Cancel"),
            PauseAction::RetireFromEvent});
        return PauseAction::None;
    case PauseItem::Quit:
        // 0x827ea788, single player outside a party: EXIT SINGLE PLAYER.
        openConfirmation(
            {strings.get(kPauseMenu, "IDS_Quit"), strings.get(kPauseMenu, "IDS_QuitRace_Desc_SinglePlayer"),
                strings.get(kHelpButtons, "IDS_Quit_QuitSinglePlayer"), strings.get(kHelpButtons, "IDS_Cancel"),
                PauseAction::QuitToTitle});
        return PauseAction::None;
    case PauseItem::Multiplayer:
        return PauseAction::OpenMultiplayer;
    case PauseItem::PhotoMode:
        return PauseAction::OpenPhotoMode;
    case PauseItem::MessageCentre:
        return PauseAction::OpenMessageCentre;
    case PauseItem::MyProfile:
        return PauseAction::OpenMyProfile;
    case PauseItem::SponsorChallenges:
        return PauseAction::OpenSponsorChallenges;
    case PauseItem::CoopChallenges:
        return PauseAction::OpenCoopChallenges;
    case PauseItem::HeadTracking:
        return PauseAction::OpenHeadTracking;
    case PauseItem::CarChallenges:
        return PauseAction::OpenCarChallenges;
    case PauseItem::HorizonRally:
        return PauseAction::OpenHorizonRally;
    case PauseItem::Finish:
        return PauseAction::Finish;
    case PauseItem::EndFreeRoam:
        return PauseAction::EndFreeRoam;
    case PauseItem::QuitMission:
        return PauseAction::QuitMission;
    case PauseItem::RestartMission:
        return PauseAction::RestartMission;
    }
    return PauseAction::None;
}

PauseAction PauseMenu::Impl::confirm(bool accepted)
{
    const PauseAction action = confirmation->action;
    confirmation.reset();
    pauseHelp.enable(pause, true);
    return accepted ? action : PauseAction::None;
}

PauseMenu::PauseMenu(const Vfs& vfs, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, language))
{
}

PauseMenu::~PauseMenu() = default;

void PauseMenu::open(const PauseMenuContext& context)
{
    Impl& impl = *m_impl;
    impl.context = context;
    impl.entries = pauseMenuEntries(context, impl.strings);
    impl.focus = 0;
    impl.confirmation.reset();
    impl.show();
}

void PauseMenu::reopen()
{
    m_impl->confirmation.reset();
    m_impl->show();
}

void PauseMenu::close()
{
    m_impl->open = false;
    m_impl->confirmation.reset();
}

bool PauseMenu::isOpen() const
{
    return m_impl->open;
}

PauseAction PauseMenu::update(const MenuInput& input, float dt)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return PauseAction::None;
    }
    PauseAction action = PauseAction::None;
    if (impl.confirmation) {
        if (input.accept) {
            action = impl.confirm(true);
        } else if (input.back) {
            action = impl.confirm(false);
        }
    } else {
        const int count = static_cast<int>(std::min(impl.entries.size(), impl.buttons.size()));
        if (input.back) {
            action = PauseAction::Resume;
        } else if (input.accept && count > 0) {
            action = impl.select();
        } else if (input.up && impl.focus > 0) {
            impl.setFocus(impl.focus - 1, false);
        } else if (input.down && impl.focus + 1 < count) {
            impl.setFocus(impl.focus + 1, false);
        }
    }
    impl.pause.update(dt);
    impl.pause.takeFiredEvents();
    if (impl.confirmation) {
        impl.popup.update(dt);
        impl.popup.takeFiredEvents();
    }
    if (action != PauseAction::None) {
        close();
    }
    return action;
}

void PauseMenu::draw(int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.open) {
        return;
    }
    impl.renderer.begin(width, height);
    std::vector<DrawItem> items = impl.pause.evaluate();
    impl.pauseHelp.apply(items);
    impl.renderer.draw(items);
    if (impl.confirmation) {
        std::vector<DrawItem> popupItems = impl.popup.evaluate();
        impl.popupHelp.apply(popupItems);
        impl.renderer.draw(popupItems);
    }
    impl.renderer.end();
}

const std::vector<PauseMenuEntry>& PauseMenu::entries() const
{
    return m_impl->entries;
}

int PauseMenu::focus() const
{
    return m_impl->focus;
}

bool PauseMenu::confirming() const
{
    return m_impl->confirmation.has_value();
}

} // namespace fh::ui
