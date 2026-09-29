#include "Hud.h"

#include "MenuCommon.h"
#include "Minimap.h"
#include "Presentation.h"
#include "SuperStacker.h"
#include "UiRenderer.h"
#include "VectorFont.h"

#include "base/Error.h"
#include "base/Log.h"
#include "data/StringTable.h"
#include "vfs/Vfs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <optional>
#include <unordered_map>

namespace fh::ui {

namespace {

constexpr const char* kScene = "media/ui/scenes/ui4/947_HUD";

/// The rev gauge's setup record, as the HUD fills it before calling the
/// ANALOG_GAUGE setup (0x82865708): scale bottom, scale top (the car's
/// vcall 408, taken to be its top revs), redline (vcall 132), revs per
/// numbered step, at most 10 numbers, the stride grows by 1, odd tops get a
/// half step, no offset.
constexpr float kGaugeMin = 0.0F;
constexpr float kGaugeStep = 1000.0F;
constexpr int kGaugeMaxLabels = 10;
constexpr int kGaugeStrideIncrement = 1;
constexpr bool kGaugeHalfStepWhenOdd = true;

/// NEEDLE_SWOOSH (0x82863820): the trail's length per unit of needle travel
/// in a frame and the seconds per unit of trail change. Both are
/// initialised data of the executable (0x83332d7c and 0x83332d78).
constexpr float kSwooshPerTravel = 28.0F;
constexpr float kSwooshSecondsPerUnit = 0.05F;

/// The text tables the controls read.
constexpr const char* kInGame = "InGame";
constexpr const char* kActivities = "Activities";
constexpr const char* kUnitStrings = "UnitStrings";

/// String tables by name, loaded on first use.
class Strings {
public:
    Strings(const Vfs& vfs, std::string language)
        : m_vfs(vfs)
        , m_language(std::move(language))
    {
    }

    /// The text of `id` in `table`, or the id itself when missing so the gap
    /// stays visible.
    std::string get(std::string_view table, std::string_view id)
    {
        const std::string key = toLower(table);
        auto it = m_tables.find(key);
        if (it == m_tables.end()) {
            std::optional<StringTable> loaded;
            for (const std::string& language : {m_language, std::string("en")}) {
                const std::string path = std::format("media/stringtables/{}/{}.str", language, key);
                if (const std::optional<Bytes> data = m_vfs.read(path)) {
                    try {
                        loaded = StringTable::parse(*data, path);
                        break;
                    } catch (const DataError& e) {
                        logWarning("ui: {}", e.what());
                    }
                }
            }
            it = m_tables.emplace(key, std::move(loaded)).first;
        }
        if (it->second) {
            if (std::optional<std::string> text = it->second->get(stringIdHash(id))) {
                return *text;
            }
        }
        return std::string(id);
    }

private:
    const Vfs& m_vfs;
    std::string m_language;
    std::unordered_map<std::string, std::optional<StringTable>> m_tables;
};

std::string replaceAll(std::string text, std::string_view from, std::string_view to)
{
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

} // namespace

struct Hud::Impl {
    Impl(const Vfs& vfs, const std::string& track, const std::string& language);

    void bind();
    void startSession(bool race);
    void setupGauge(const HudState& state);
    void updateGauge(const HudState& state);
    void updateAssists(const HudState& state);
    void updateSatNav(const HudState& state);
    void updateRace(const HudState& state);
    void updateTimers(const HudState& state);
    void updatePositions(const HudState& state);
    void updateCountdown(const HudState& state);
    void updatePrompt(const HudState& state);
    void updateRadio(const HudState& state);
    void handleFiredEvents();
    void setRadioStation(int station);
    void setRadioTrack(const std::string& title, const std::string& artist);
    void showRadio();
    void drawMinimap(const HudState& state, int width, int height);
    std::string formatElapsed(float seconds);

    void text(int element, std::string value)
    {
        if (element >= 0) {
            scene.setText(element, std::move(value));
        }
    }
    void fire(int contract, std::string_view event)
    {
        if (contract >= 0) {
            scene.fireEvent(contract, event);
        }
    }
    int parameter(int contract, std::string_view name) const { return scene.contractParameter(contract, name); }

    Strings strings;
    FontLibrary fonts;
    Presentation scene;
    UiRenderer renderer;
    std::unique_ptr<MinimapRenderer> minimap;

    int rootContract = -1;
    int hud = -1;
    int hudContract = -1;

    int gearText = -1;
    int unitText = -1;
    std::vector<int> speedDigits;
    int gaugeContract = -1;
    struct Marks {
        std::vector<int> components;
    };
    Marks smallHatches;
    Marks largeHatches;
    Marks midHatches;
    std::vector<int> numberComponents;
    std::vector<int> numberTexts;
    int needle = -1;
    int redline = -1;
    int swooshUp = -1;
    int swooshDown = -1;
    float needleMin = 0.0F;
    float needleMax = 1.0F;
    float redlineMin = 0.0F;
    float redlineMax = 1.0F;
    float gaugeTop = -1.0F;
    float gaugeRedline = -1.0F;
    float previousSwoosh = 0.0F;

    std::optional<bool> abs;
    std::optional<bool> stm;
    std::optional<bool> tcs;

    int placeText = -1;
    int placeMax = -1;
    int lapCurrent = -1;
    int lapFinal = -1;
    int endConditionBlock = -1;

    int timersContract = -1;
    std::array<int, 5> timerValues{};
    std::array<int, 5> timerLabels{};
    int timerLinesShown = 0;

    struct PositionBar {
        int element = -1;
        int contract = -1;
        int name = -1;
        int position = -1;
    };
    std::vector<PositionBar> positionBars;

    int countdownContract = -1;
    int countText = -1;
    int countdownStage = 0;

    int promptContract = -1;
    int promptName = -1;
    int promptAction = -1;
    int promptTitle = -1;
    int promptLocked = -1;
    bool promptShown = false;
    std::optional<std::string> promptTitleShown;
    std::string promptIconShown;

    int mapModelViewport = -1;
    int mapMaterial = -1;

    std::optional<bool> sessionRace;

    int satNavContract = -1;
    int satNavDistance = -1;
    float shownDistance = 0.0F;

    int radioContract = -1;
    int radioControl = -1;
    int radioIcons = -1;
    int radioTrackText = -1;
    int radioArtistText = -1;
    int radioOffText = -1;
    int radioTuningContract = -1;
    /// The HUD_RADIO control's state, as its constructor (0x828b8988) sets
    /// it up: station (+244, none), whether the last station set changed it
    /// (+248), and whether the title and artist lines are showing (+321 and
    /// +320, both at first).
    int radioStation = -1;
    bool radioStationChanged = false;
    bool radioTrackShown = true;
    bool radioArtistShown = true;
    bool radioTuningShown = false;

    /// The scene's SUPER_STACKER contracts, which move and stretch elements
    /// to follow the texts the HUD sets.
    SuperStackers stackers;
};

Hud::Impl::Impl(const Vfs& vfs, const std::string& track, const std::string& language)
    : strings(vfs, language)
    , fonts(vfs, language)
    , scene(SceneFiles::load(vfs, kScene))
    , renderer(vfs, fonts)
{
    bind();
    try {
        minimap = std::make_unique<MinimapRenderer>(vfs, renderer, track);
    } catch (const DataError& e) {
        logWarning("ui: no minimap: {}", e.what());
    }
}

void Hud::Impl::bind()
{
    rootContract = scene.contract(0, "UIContract");
    hud = parameter(rootContract, "COMPONENT_HUD");
    hudContract = scene.contract(hud, "HUD");
    if (hudContract < 0) {
        throw DataError("{}: no HUD contract", kScene);
    }

    gearText = parameter(hudContract, "TEXT_GEAR_ANALOG");
    unitText = parameter(hudContract, "TEXT_SPEED_UNIT_ANALOG");
    const int speedo = parameter(hudContract, "COMPONENT_SPEED_ANALOG");
    const int speedoContract = scene.contract(speedo, "SPEEDO");
    if (const int digits = parameter(speedoContract, "GROUP_DIGITS"); digits >= 0) {
        speedDigits = scene.children(digits);
    }

    const int rpm = parameter(hudContract, "COMPONENT_RPM_ANALOG");
    gaugeContract = scene.contract(rpm, "ANALOG_GAUGE");
    const auto components = [&](std::string_view group) {
        std::vector<int> out;
        if (const int element = parameter(gaugeContract, group); element >= 0) {
            for (const int child : scene.children(element)) {
                if (scene.kind(child) == ElementKind::Component) {
                    out.push_back(child);
                }
            }
        }
        return out;
    };
    smallHatches.components = components("GROUP_SMALL_HATCHES");
    largeHatches.components = components("GROUP_LARGE_HATCHES");
    midHatches.components = components("GROUP_MID_HATCHES");
    numberComponents = components("GROUP_NUMBERS");
    for (const int number : numberComponents) {
        numberTexts.push_back(parameter(scene.contract(number, "RPM_NUMBER"), "TEXT_NUMBER"));
    }
    needle = parameter(gaugeContract, "COMPONENT_NEEDLE_SLIDER");
    redline = parameter(gaugeContract, "COMPONENT_REDLINE_SLIDER");
    const int swooshes = parameter(gaugeContract, "COMPONENT_SWOOSHES");
    const int swooshContract = scene.contract(swooshes, "NEEDLE_SWOOSH");
    swooshUp = parameter(swooshContract, "COMPONENT_UP");
    swooshDown = parameter(swooshContract, "COMPONENT_DOWN");

    placeText = parameter(hudContract, "TEXT_PLACE");
    placeMax = parameter(hudContract, "TEXT_PLACE_MAX");
    lapCurrent = parameter(hudContract, "TEXT_ENDCON_CURRENT");
    lapFinal = parameter(hudContract, "TEXT_ENDCON_FINAL");
    endConditionBlock = lapCurrent >= 0 ? scene.parent(lapCurrent) : -1;

    const int timers = parameter(hudContract, "COMPONENT_TIMERS");
    timersContract = scene.contract(timers, "HUD_TIMERS");
    for (std::size_t i = 0; i < timerValues.size(); ++i) {
        timerValues[i] = parameter(timersContract, std::format("TEXT_LINE_{}", i + 1));
        timerLabels[i] = parameter(timersContract, std::format("TEXT_LINE_{}_LABEL", i + 1));
    }

    const int positions = parameter(hudContract, "COMPONENT_POSITION_INDICATORS");
    if (const int bars = parameter(scene.contract(positions, "POSITION_INDICATORS"), "GROUP_POSITIONBARS"); bars >= 0) {
        for (const int child : scene.children(bars)) {
            PositionBar bar;
            bar.element = child;
            bar.contract = scene.contract(child, "POSITION_BAR");
            if (bar.contract < 0) {
                continue;
            }
            bar.name = parameter(bar.contract, "TEXT_PLAYERNAME");
            bar.position = parameter(bar.contract, "TEXT_POSITION");
            positionBars.push_back(bar);
        }
    }

    const int countdown = parameter(rootContract, "COMPONENT_321GO");
    countdownContract = scene.contract(countdown, "HUD_321GO");
    countText = parameter(countdownContract, "TEXT_COUNT");

    const int prompt = parameter(hudContract, "COMPONENT_ACTIVATION_PROMPT");
    promptContract = scene.contract(prompt, "HUD_ACTIVATION_PROMPT");
    promptName = parameter(promptContract, "TEXT_EVENTNAME");
    promptAction = parameter(promptContract, "TEXT_CALLTOACTION");
    promptTitle = parameter(promptContract, "TEXT_TITLE");
    const int lockedText = parameter(promptContract, "TEXT_LOCKED");
    promptLocked = lockedText >= 0 ? scene.parent(lockedText) : -1;

    satNavContract = scene.contract(parameter(hudContract, "COMPONENT_SAT_NAV"), "HUD_SAT_NAV");
    satNavDistance = parameter(satNavContract, "TEXT_TARGET_DISTANCE");

    const int radio = parameter(hudContract, "COMPONENT_RADIO");
    radioContract = scene.contract(radio, "HUD_RADIO");
    radioControl = parameter(radioContract, "RADIO_CONTROL");
    radioIcons = parameter(radioContract, "COMPONENT_RADIO_ICONS");
    radioTrackText = parameter(radioContract, "TEXT_TRACK");
    radioArtistText = parameter(radioContract, "TEXT_ARTIST");
    radioOffText = parameter(radioContract, "TEXT_RADIO_OFF");
    radioTuningContract = scene.contract(parameter(hudContract, "COMPONENT_RADIO_TUNING"), "HUD_RADIO_TUNING");

    // The map renderer's contract (CUSTREND_MAPRENDERER) names the model
    // whose screen rectangle sizes the map and the material that shows it.
    int mapContract = -1;
    for (std::size_t i = 0; i < scene.elementCount() && mapContract < 0; ++i) {
        mapContract = scene.contract(static_cast<int>(i), "CUSTREND_MAPRENDERER");
    }
    mapModelViewport = parameter(mapContract, "MODEL_VIEWPORT");
    mapMaterial = parameter(mapContract, "MATERIAL_VIEWPORT");

    stackers.bind(scene, renderer);
}

void Hud::Impl::startSession(bool race)
{
    scene.reset();
    gaugeTop = -1.0F;
    gaugeRedline = -1.0F;
    previousSwoosh = 0.0F;
    shownDistance = 0.0F;
    // The HUD's constructor (0x828a9744) clears the three.
    abs = false;
    stm = false;
    tcs = false;
    timerLinesShown = 0;
    countdownStage = 0;
    promptShown = false;
    promptTitleShown.reset();
    promptIconShown.clear();
    sessionRace = race;
    radioStation = -1;
    radioStationChanged = false;
    radioTrackShown = true;
    radioArtistShown = true;
    radioTuningShown = false;
    // HUD_RADIO's attach (0x828b0300) and HUD_RADIO_TUNING's (0x82667e58)
    // fill their fixed texts.
    text(radioOffText, strings.get(kInGame, "IDS_RadioOff"));
    text(parameter(radioTuningContract, "TEXT_RADIO_TUNING"), strings.get("Main", "IDS_RadioTuning"));

    // The map renderer draws into a target the material shows; the model
    // only gives it its size (with USE_MATERIAL_VIEWPORT set, 0x826e2e20),
    // so it is not drawn itself.
    scene.setHidden(mapModelViewport, true);
    // The scene's full-screen transition curtains belong to the
    // HAND_OF_GOD_FADE control, which this HUD does not have; its layer
    // stays out.
    scene.setHidden(scene.find("Layer_Mask"), true);
    // Systems this game does not have yet, switched off with the events the
    // game itself uses for that: the mirror (HIDE_MIRROR), the popularity
    // and skill display (HIDE_POPULARITY, fired when its option is off), the
    // wrong-way warning (WRONG_WAY_OFF).
    fire(rootContract, "HIDE_MIRROR");
    fire(hudContract, "HIDE_POPULARITY");
    fire(hudContract, "WRONG_WAY_OFF");
    // The instruction banner shows whatever the status queue (the
    // CHudStatus classes) puts up and has no event that hides it before
    // then; with no status queue here it stays out.
    scene.setHidden(parameter(hudContract, "COMPONENT_INSTRUCTION"), true);
    // The prompt's call to action has a normal and a locked version drawn
    // over each other; the code that picks one was not found, and this game
    // has no locked events, so the locked one stays out.
    scene.setHidden(promptLocked, true);
    if (!race) {
        // The HUD's setup (0x828a3440) hides the timers and the mini
        // leaderboard, and the place (0x828a3534), outside races.
        fire(hudContract, "HIDE_TIMERS");
        fire(hudContract, "HIDE_MINI_LEADERBOARD");
        fire(hudContract, "HIDE_PLACE");
    }
}

void Hud::Impl::setupGauge(const HudState& state)
{
    // ANALOG_GAUGE setup (0x82865708), transcribed.
    const float top = state.maxRpm;
    const float redlineValue = state.redlineRpm;
    const float inverseStep = 1.0F / kGaugeStep;
    const float topSteps = std::ceil((top - 0.0F) * inverseStep);
    const float bottomSteps = std::floor(kGaugeMin * inverseStep);
    const bool odd = kGaugeHalfStepWhenOdd && static_cast<int>(topSteps) % 2 != 0;
    const float range = topSteps - bottomSteps;
    float stride = static_cast<float>(kGaugeStrideIncrement);
    float labels = range / stride + (odd ? 0.5F : 0.0F);
    while (labels >= static_cast<float>(kGaugeMaxLabels)) {
        stride += static_cast<float>(kGaugeStrideIncrement);
        labels = std::ceil(range / stride + (odd ? 0.5F : 0.0F));
    }
    const float n = std::floor(labels);
    float large = n;
    float mid = n - 1.0F;
    float small = 2.0F * n - 1.0F;
    float last = n;
    if (odd) {
        large = n - 1.0F;
        mid = n - 1.0F;
        small = 2.0F * (n - 1.0F);
        last = n - 1.0F;
    }
    const float spacing = 1.0F / n;
    const float half = spacing * 0.5F;
    const float quarter = half * 0.5F;

    needleMin = kGaugeStep * bottomSteps;
    const float span = last != 0.0F ? kGaugeStep * last * stride / (spacing * last) : 0.0F;
    needleMax = needleMin + span;
    redlineMin = needleMin;
    redlineMax = span;
    if (redline >= 0 && redlineMax != redlineMin) {
        scene.goToProgress(redline, (redlineValue - redlineMin) / (redlineMax - redlineMin));
    }

    for (std::size_t i = 0; i < numberComponents.size(); ++i) {
        const bool enabled = static_cast<float>(i) < last + 1.0F;
        scene.setHidden(numberComponents[i], !enabled);
        if (enabled) {
            const int label = static_cast<int>(static_cast<float>(i) * stride + kGaugeMin);
            text(numberTexts[i], std::to_string(label));
            scene.goToProgress(numberComponents[i], static_cast<float>(i) * spacing);
        }
    }
    const auto placeMarks = [&](const Marks& marks, float count, float start, float step) {
        for (std::size_t i = 0; i < marks.components.size(); ++i) {
            const bool enabled = static_cast<float>(i) < count;
            scene.setHidden(marks.components[i], !enabled);
            if (enabled) {
                scene.goToProgress(marks.components[i], start + static_cast<float>(i) * step);
            }
        }
    };
    placeMarks(largeHatches, large + 1.0F, 0.0F, spacing);
    placeMarks(smallHatches, small + 1.0F, quarter, half);
    placeMarks(midHatches, mid + 1.0F, half, spacing);
    gaugeTop = top;
    gaugeRedline = redlineValue;
}

void Hud::Impl::updateGauge(const HudState& state)
{
    if (state.maxRpm != gaugeTop || state.redlineRpm != gaugeRedline) {
        setupGauge(state);
    }

    // SPEEDO (0x828c4b60): digits from the last, each (v / place) % 10,
    // dimmed to 25% while the value has no digit there.
    const float speed = std::fabs(state.metric ? state.speedKmh : state.speedMph);
    const auto value = static_cast<long long>(speed);
    long long place = 1;
    for (auto it = speedDigits.rbegin(); it != speedDigits.rend(); ++it) {
        text(*it, std::to_string((value / place) % 10));
        scene.setAttribute(*it, attr::Opacity, value < place && place > 1 ? 25.0F : 100.0F);
        place *= 10;
    }
    text(unitText, strings.get(kActivities, state.metric ? "IDS_KmPerHour" : "IDS_Mph"));
    if (state.gear < 0) {
        text(gearText, strings.get(kInGame, "IDS_ReverseGear"));
    } else if (state.gear == 0) {
        text(gearText, strings.get(kInGame, "IDS_NeutralGear"));
    } else {
        text(gearText, std::to_string(state.gear));
    }

    // PROGRESS_BAR update (0x828baf48): progress = (value - min) / (max -
    // min), unclamped; the gauge's bars do not animate.
    if (needle >= 0 && needleMax != needleMin) {
        scene.goToProgress(needle, (state.rpm - needleMin) / (needleMax - needleMin));
    }
    // NEEDLE_SWOOSH (0x82863820) with the revs over the needle's top.
    const float fraction = needleMax != 0.0F ? state.rpm / needleMax : 0.0F;
    const float delta = fraction - previousSwoosh;
    const auto trail = [&](int component, float target) {
        if (component < 0) {
            return;
        }
        const float clamped = std::clamp(target, 0.0F, 1.0F);
        const float seconds = kSwooshSecondsPerUnit * std::fabs(scene.progress(component) - clamped);
        scene.animateProgress(component, clamped, seconds, 1);
    };
    if (delta > 0.0F) {
        trail(swooshUp, 1.0F - kSwooshPerTravel * delta);
        trail(swooshDown, 1.0F);
    } else {
        trail(swooshDown, 1.0F + kSwooshPerTravel * delta);
        trail(swooshUp, 1.0F);
    }
    previousSwoosh = fraction;
}

void Hud::Impl::updateAssists(const HudState& state)
{
    const auto report = [&](std::optional<bool>& shown, bool on, std::string_view onEvent, std::string_view offEvent) {
        if (!shown || *shown != on) {
            fire(hudContract, on ? onEvent : offEvent);
            shown = on;
        }
    };
    report(abs, state.absOn, "ABS_ON", "ABS_OFF");
    report(stm, state.stmOn, "STM_ON", "STM_OFF");
    report(tcs, state.tcsOn, "TCS_ON", "TCS_OFF");
}

void Hud::Impl::updateSatNav(const HudState& state)
{
    // HUD_SAT_NAV's distance (0x828b0768): SHOW_TARGET when a distance
    // appears, the text whenever it changes, HIDE_TARGET when it goes to 0.
    // The text is the game's unit value formatter (0x82d6fbd0 with unit
    // type 10 and one decimal, as the call passes); its kilometre or mile
    // template (UnitStrings IDS_Units_Kilometers and IDS_Units_Miles) and
    // the metre-to-mile factor (1609.344, a constant of the executable) are
    // taken to be what that unit type selects (inferred: its table was not
    // traced).
    const float distance = state.destinationDistance.value_or(0.0F);
    if (distance != 0.0F) {
        if (shownDistance == 0.0F) {
            fire(satNavContract, "SHOW_TARGET");
        }
        if (distance != shownDistance) {
            const float value = state.metric ? distance / 1000.0F : distance / 1609.344F;
            text(satNavDistance,
                replaceAll(strings.get(kUnitStrings, state.metric ? "IDS_Units_Kilometers" : "IDS_Units_Miles"), "{0}",
                    std::format("{:.1f}", value)));
            shownDistance = distance;
        }
    } else if (shownDistance != 0.0F) {
        fire(satNavContract, "HIDE_TARGET");
        shownDistance = 0.0F;
    }
}

void Hud::Impl::updateRace(const HudState& state)
{
    if (!state.inRace) {
        return;
    }
    text(placeText, std::to_string(state.position));
    text(placeMax, std::to_string(state.racers));
    const bool circuit = state.laps > 1;
    if (endConditionBlock >= 0) {
        scene.setHidden(endConditionBlock, !circuit);
    }
    if (circuit) {
        text(lapCurrent, std::to_string(std::clamp(state.lap, 1, state.laps)));
        text(lapFinal, std::to_string(state.laps));
    }
}

std::string Hud::Impl::formatElapsed(float seconds)
{
    // The race status formatter (0x82d6f708): hours, minutes to two digits,
    // seconds to two digits and three decimals, put into the elapsed-time
    // template with hours or without.
    const float t = std::max(seconds, 0.0F);
    const int hours = static_cast<int>(t / 3600.0F);
    const int minutes = (static_cast<int>(t) / 60) % 60;
    const float secondsPart = std::fmod(t, 60.0F);
    std::string pattern = hours >= 1 ? strings.get(kUnitStrings, "IDS_NLSFormat_ElapsedTime_HHMMSS")
                                     : strings.get(kUnitStrings, "IDS_NLSFormat_ElapsedTime_MMSS");
    pattern = replaceAll(std::move(pattern), "{0}", std::to_string(hours));
    pattern = replaceAll(std::move(pattern), "{1}", std::format("{:02}", minutes));
    return replaceAll(std::move(pattern), "{2}", std::format("{:06.3f}", secondsPart));
}

void Hud::Impl::updateTimers(const HudState& state)
{
    if (!state.inRace || timersContract < 0) {
        return;
    }
    // CHudTimersControl (0x828b3f10): the slots present fill the lines in
    // order, then SB_LINES_<count> shows that many.
    const std::array<std::pair<const char*, const std::optional<float>*>, 5> slots{{
        {"IDS_Race", &state.raceTime},
        {"IDS_CurrentLapTimeLabel", &state.lapTime},
        {"IDS_Last", &state.lastLapTime},
        {"IDS_Best", &state.bestLapTime},
        {"IDS_Target", &state.targetTime},
    }};
    int line = 0;
    for (const auto& [label, value] : slots) {
        if (!value->has_value()) {
            continue;
        }
        text(timerLabels[static_cast<std::size_t>(line)], strings.get(kInGame, label));
        text(timerValues[static_cast<std::size_t>(line)], formatElapsed(**value));
        ++line;
    }
    if (line > 0) {
        fire(timersContract, std::format("SB_LINES_{}", line));
    } else if (timerLinesShown > 0) {
        fire(timersContract, "HIDDEN");
    }
    timerLinesShown = line;
}

void Hud::Impl::updatePositions(const HudState& state)
{
    if (!state.inRace) {
        return;
    }
    for (std::size_t i = 0; i < positionBars.size(); ++i) {
        const PositionBar& bar = positionBars[i];
        const bool used = i < state.standings.size();
        scene.setHidden(bar.element, !used);
        if (!used) {
            continue;
        }
        text(bar.name, state.standings[i]);
        text(bar.position, std::to_string(i + 1));
        const bool player = static_cast<int>(i) == state.playerStanding;
        fire(bar.contract, player ? "LOCAL_PLAYER" : (i == 0 ? "FIRST_PLACE" : "OTHER_PLAYER"));
    }
}

void Hud::Impl::updateCountdown(const HudState& state)
{
    if (!state.countdownElapsed) {
        countdownStage = 0;
        return;
    }
    // 0x828a3e00: each stage once, on the first frame past its second.
    const float elapsed = *state.countdownElapsed;
    static constexpr std::array<std::pair<const char*, const char*>, 4> kStages{{
        {"IDS_3", "COUNTDOWN_THREE"},
        {"IDS_2", "COUNTDOWN_TWO"},
        {"IDS_1", "COUNTDOWN_ONE"},
        {"IDS_3_2_1_GO", "COUNTDOWN_GO"},
    }};
    int stage = 0;
    if (elapsed > 3.0F) {
        stage = 4;
    } else if (elapsed > 2.0F) {
        stage = 3;
    } else if (elapsed > 1.0F) {
        stage = 2;
    } else if (elapsed > 0.0F) {
        stage = 1;
    }
    if (stage > countdownStage) {
        const auto& [textId, event] = kStages[static_cast<std::size_t>(stage - 1)];
        text(countText, strings.get(kInGame, textId));
        fire(countdownContract, event);
        countdownStage = stage;
    }
}

void Hud::Impl::updatePrompt(const HudState& state)
{
    const bool wanted = !state.prompt.empty();
    if (wanted) {
        text(promptName, state.prompt);
        text(promptAction, state.promptDetail);
        if (!promptTitleShown || *promptTitleShown != state.promptTitle) {
            text(promptTitle, state.promptTitle);
            fire(promptContract, state.promptTitle.empty() ? "HIDE_TITLE" : "SHOW_TITLE");
            promptTitleShown = state.promptTitle;
        }
        if (!state.promptIcon.empty() && state.promptIcon != promptIconShown) {
            fire(promptContract, "SET_EVENTICON_" + state.promptIcon);
            promptIconShown = state.promptIcon;
        }
    }
    if (wanted != promptShown) {
        fire(promptContract, wanted ? "SHOW" : "HIDE");
        promptShown = wanted;
    }
}

void Hud::Impl::drawMinimap(const HudState& state, int width, int height)
{
    if (!minimap || mapModelViewport < 0) {
        return;
    }
    // The map's target is the viewport model's rectangle in pixels
    // (256 x 256 scene units), rounded down to a multiple of 8 as the
    // render callback does.
    const float scale = std::min(static_cast<float>(width) / 1280.0F, static_cast<float>(height) / 720.0F);
    const glm::mat4& world = scene.worldTransform(mapModelViewport);
    const float units = glm::length(glm::vec3(world[0])) * 100.0F;
    const int size = std::max(8, static_cast<int>(units * scale) / 8 * 8);
    std::vector<MapMarker> markers;
    markers.reserve(state.mapMarkers.size());
    for (const HudMapMarker& marker : state.mapMarkers) {
        markers.push_back({marker.key, marker.value, marker.position, marker.heading});
    }
    std::vector<MapLine> lines;
    lines.reserve(state.mapLines.size());
    for (const HudMapLine& line : state.mapLines) {
        lines.push_back({line.key, line.value, line.points});
    }
    const GLuint texture = minimap->render(
        state.playerMapPosition, state.playerHeading, state.speedMetresPerSecond, size, markers, lines);
    renderer.setTextureOverride("@minimap", texture);
}

void Hud::Impl::setRadioStation(int station)
{
    // 0x828ad930: the logo is slide station + 1 of the icons; a different
    // station shows the panel with the station change.
    radioStationChanged = false;
    scene.goToSlideIndex(radioIcons, station + 1);
    if (station != radioStation) {
        radioStation = station;
        radioStationChanged = true;
        showRadio();
    }
}

void Hud::Impl::setRadioTrack(const std::string& title, const std::string& artist)
{
    // 0x828b0488. The game skips this when handed the very same strings,
    // which a new request never carries.
    const auto line = [&](const std::string& value, int element, bool& shown, std::string_view showEvent,
                          std::string_view hideEvent) {
        if (!value.empty()) {
            if (!shown) {
                fire(radioContract, showEvent);
                shown = true;
            }
            text(element, value);
        } else if (shown) {
            fire(radioContract, hideEvent);
            shown = false;
        }
    };
    line(title, radioTrackText, radioTrackShown, "SHOW_TRACK_NAME", "HIDE_TRACK_NAME");
    line(artist, radioArtistText, radioArtistShown, "SHOW_ARTIST_NAME", "HIDE_ARTIST_NAME");
    showRadio();
}

void Hud::Impl::showRadio()
{
    // 0x828ab530: back to the start of the panel's timeline, then the
    // station-change or track-change slide.
    scene.goToProgress(radioControl, 0.0F);
    fire(radioContract, radioStationChanged ? "SHOW_STATION" : "SHOW_INFO");
}

void Hud::Impl::updateRadio(const HudState& state)
{
    if (state.radio) {
        // The radio status handler (0x826fc568) sets the station and the
        // track lines; both show the panel again.
        setRadioStation(state.radio->station);
        setRadioTrack(state.radio->title, state.radio->artist);
    }
    if (state.radioTuning != radioTuningShown) {
        fire(radioTuningContract, state.radioTuning ? "SHOW" : "HIDE");
        radioTuningShown = state.radioTuning;
    }
}

void Hud::Impl::handleFiredEvents()
{
    for (const Presentation::FiredEvent& fired : scene.takeFiredEvents()) {
        // HUD_RADIO's event hook (0x828b0400): the panel's slides fire
        // HIDE_COMPLETE near their end, and the control then hides the panel
        // (0x828ae540). What it does with START_HIDE (a call through its
        // vtable at +68) was not traced.
        if (fired.target == radioContract && fired.event == "HIDE_COMPLETE") {
            fire(radioContract, "HIDE_INFO");
        }
    }
}

Hud::Hud(const Vfs& vfs, const std::string& track, const std::string& language)
    : m_impl(std::make_unique<Impl>(vfs, track, language))
{
}

Hud::~Hud() = default;

void Hud::draw(const HudState& state, int width, int height)
{
    Impl& impl = *m_impl;
    if (!impl.sessionRace || *impl.sessionRace != state.inRace) {
        impl.startSession(state.inRace);
    }
    impl.updateGauge(state);
    impl.updateAssists(state);
    impl.updateSatNav(state);
    impl.updateRace(state);
    impl.updateTimers(state);
    impl.updatePositions(state);
    impl.updateCountdown(state);
    impl.updatePrompt(state);
    impl.updateRadio(state);
    impl.scene.update(state.dt);
    impl.handleFiredEvents();
    impl.stackers.layout(impl.scene, impl.renderer);
    std::vector<DrawItem> items = impl.scene.evaluate();
    applyTextScale(items);
    impl.drawMinimap(state, width, height);
    // The material's first image is where the game points the map's render
    // target (0x82892a60).
    for (DrawItem& item : items) {
        if (item.isText || item.model.layers.empty() || impl.mapMaterial < 0) {
            continue;
        }
        if (item.material == impl.mapMaterial) {
            item.model.layers.front().texture = "@minimap";
            item.model.layers.front().imageName = "@minimap";
        }
    }
    impl.renderer.begin(width, height);
    impl.renderer.draw(items);
    impl.renderer.end();
}

} // namespace fh::ui
