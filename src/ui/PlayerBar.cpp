#include "PlayerBar.h"

#include "AnarkFormats.h"
#include "MenuCommon.h"
#include "Presentation.h"
#include "UiRenderer.h"

#include "base/Error.h"
#include "data/Database.h"
#include "data/StringTable.h"

#include <algorithm>
#include <format>

namespace openhorizon::ui {

namespace {

constexpr const char* kUnitStrings = "UnitStrings";
constexpr const char* kInGame = "InGame";
constexpr const char* kWristbandTable = "CareerWristbandLevels";
/// 0x8299cc90 prefixes the Thumbnail with this and appends ".tga".
constexpr const char* kWristbandFolder = "game:\\media\\UI\\Textures\\Horizon\\Wristbands\\";

/// The clamp of 0x82d6d008 (0x3b9ac9ff and its negation).
constexpr std::int64_t kLargestShown = 999'999'999;

std::string replaceAll(std::string text, std::string_view from, std::string_view to)
{
    for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

const WristbandLevel* levelWithId(const std::vector<WristbandLevel>& levels, std::int64_t id)
{
    const auto it
        = std::find_if(levels.begin(), levels.end(), [id](const WristbandLevel& level) { return level.id == id; });
    return it == levels.end() ? nullptr : &*it;
}

} // namespace

std::string formatGroupedInteger(std::int64_t value, std::string_view separator)
{
    const std::int64_t clamped = std::clamp(value, -kLargestShown, kLargestShown);
    const std::string digits = std::to_string(clamped < 0 ? -clamped : clamped);
    std::string text = clamped < 0 ? "-" : "";
    for (std::size_t i = 0; i < digits.size(); ++i) {
        const std::size_t remaining = digits.size() - i;
        if (i > 0 && remaining % 3 == 0) {
            text += separator;
        }
        text += digits[i];
    }
    return text;
}

std::string_view thousandsSeparatorId(bool metric)
{
    return metric ? "IDS_NLSFormat_ThousandsSeparator" : "IDS_NLSFormat_ThousandsSeparator_NA";
}

std::vector<WristbandLevel> loadWristbandLevels(const Database& db, Localisation& strings)
{
    if (!db.hasTable(kWristbandTable)) {
        throw DataError("gamedb: no {} table", kWristbandTable);
    }
    std::vector<WristbandLevel> levels;
    for (const Row& row : db.query("SELECT Id, XP, Name, Thumbnail FROM CareerWristbandLevels ORDER BY Id")) {
        WristbandLevel level;
        level.id = row.i("Id");
        level.xp = row.integer("XP");
        level.name = strings.resolve(kWristbandTable, row.text("Name"));
        level.thumbnail = row.text("Thumbnail");
        levels.push_back(std::move(level));
    }
    return levels;
}

void PlayerBar::bind(const Presentation& scene, UiRenderer& renderer, int bar)
{
    m_contract = scene.contract(bar, "PLAYER_BAR");
    if (m_contract < 0) {
        return;
    }
    m_wristband = scene.contractParameter(m_contract, "TEXT_SP_WRISTBAND");
    m_wristbandMaterial = scene.contractParameter(m_contract, "MATERIAL_SP_WRISTBAND");
    m_car = scene.contractParameter(m_contract, "TEXT_PLAYER_CAR");
    m_credits = scene.contractParameter(m_contract, "TEXT_PLAYER_CREDITS");
    m_nextLevel = scene.contractParameter(m_contract, "TEXT_NEXT_LEVEL");
    m_classBadge = scene.contractParameter(m_contract, "COMPONENT_CLASS_BADGE");
    m_stackers.bind(scene, renderer, bar);
}

PlayerBarTexts PlayerBar::texts(const PlayerBarValues& values, const std::vector<WristbandLevel>& levels,
    const Database* db, Localisation& names, UiStrings& strings, bool metric)
{
    PlayerBarTexts texts;
    const std::string separator = strings.get(kUnitStrings, thousandsSeparatorId(metric));

    // 0x82995118: the XP list is indexed by position in Id order.
    const std::int64_t next = values.wristbandLevel + 1;
    const auto count = static_cast<std::int64_t>(levels.size());
    if (next >= 1 && next < count) {
        const std::int64_t points = levels[static_cast<std::size_t>(next)].xp - values.xp;
        if (points > 0) {
            texts.wristband = replaceAll(
                strings.get(kUnitStrings, "IDS_Units_PTS"), "{0}", formatGroupedInteger(points, separator));
            texts.nextLevel = strings.get(kInGame, "IDS_WristbandNextLevel");
        } else if (const WristbandLevel* upcoming = levelWithId(levels, next)) {
            // 0x8299b990 names the band by its Id.
            texts.nextLevel = replaceAll(strings.get(kInGame, "IDS_CollectWristband"), "{0}", upcoming->name);
        }
    }
    if (const WristbandLevel* worn = levelWithId(levels, values.wristbandLevel); worn && !worn->thumbnail.empty()) {
        texts.wristbandTexture = texturePathForScene(std::string(kWristbandFolder) + worn->thumbnail + ".tga");
    }

    texts.credits = replaceAll(
        strings.get(kUnitStrings, "IDS_Units_Credits"), "{0}", formatGroupedInteger(values.credits, separator));

    if (db && values.carId) {
        const std::optional<Row> row
            = db->queryOne("SELECT c.DisplayName AS carName, m.DisplayName AS makeName FROM Data_Car c INNER JOIN "
                           "List_CarMake m ON c.MakeId=m.ID WHERE c.Id = ?",
                {std::to_string(*values.carId)});
        if (row) {
            texts.car = std::format("{} {}", names.resolve("List_CarMake", row->text("makeName")),
                names.resolve("Data_Car", row->text("carName")));
        }
    }
    return texts;
}

void PlayerBar::set(Presentation& scene, const PlayerBarTexts& texts)
{
    if (m_contract < 0) {
        return;
    }
    scene.fireEvent(m_contract, "SET_FOR_SINGLEPLAYER");
    // Class and PI not traced; the authored "A 596" would read as the car's.
    if (m_classBadge >= 0) {
        scene.setHidden(m_classBadge, true);
    }
    const auto text = [&](int element, const std::string& value) {
        if (element >= 0) {
            scene.setText(element, value);
        }
    };
    text(m_wristband, texts.wristband);
    text(m_nextLevel, texts.nextLevel);
    text(m_credits, texts.credits);
    text(m_car, texts.car);
    m_wristbandTexture = texts.wristbandTexture;
}

void PlayerBar::layout(Presentation& scene, UiRenderer& renderer) const
{
    m_stackers.layout(scene, renderer);
}

void PlayerBar::apply(std::vector<DrawItem>& items) const
{
    if (m_wristbandTexture.empty() || m_wristbandMaterial < 0) {
        return;
    }
    for (DrawItem& item : items) {
        if (item.isText || item.material != m_wristbandMaterial || item.model.layers.empty()) {
            continue;
        }
        item.model.layers.front().texture = m_wristbandTexture;
        item.model.layers.front().imageName = m_wristbandTexture;
    }
}

} // namespace openhorizon::ui
