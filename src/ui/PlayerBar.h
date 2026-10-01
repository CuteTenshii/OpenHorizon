#pragma once

#include "SuperStacker.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {
class Database;
class Localisation;
} // namespace openhorizon

namespace openhorizon::ui {

class Presentation;
class UiRenderer;
class UiStrings;
struct DrawItem;

/// The game's number formatter (0x82d6d008; units 0x82d6faf8, 0x82d6fc50):
/// clamped to +-999,999,999, `separator` between groups of three.
std::string formatGroupedInteger(std::int64_t value, std::string_view separator);

/// A row of gamedb's CareerWristbandLevels, in Id order.
struct WristbandLevel {
    int id = 0;
    /// The Main/XP a player needs for this band.
    std::int64_t xp = 0;
    /// The band's name, resolved through the CareerWristbandLevels table.
    std::string name;
    /// Under media\UI\Textures\Horizon\Wristbands, without extension.
    std::string thumbnail;
};

/// As 0x8259eae8 loads it ("SELECT Id,XP FROM %s ORDER BY Id"; name and
/// thumbnail via 0x8299b990, 0x8299cc90). Throws DataError when unreadable.
std::vector<WristbandLevel> loadWristbandLevels(const Database& db, Localisation& strings);

/// What the single-player bar shows: profile values and the player's car.
struct PlayerBarValues {
    /// Main/Credits.
    std::int64_t credits = 0;
    /// Main/WristbandLevel: the Id of the band the player wears.
    std::int64_t wristbandLevel = 0;
    /// Main/XP.
    std::int64_t xp = 0;
    /// Data_Car.Id of the player's car; none leaves the car line empty.
    std::optional<int> carId;

    bool operator==(const PlayerBarValues&) const = default;
};

/// The texts the single-player bar sets, before they reach the scene.
struct PlayerBarTexts {
    std::string wristband;
    std::string nextLevel;
    std::string credits;
    std::string car;
    /// The band's image as a scene texture path; empty for none.
    std::string wristbandTexture;
};

/// A front-end scene's PLAYER_BAR contract (h_map and others).
///
/// Verified, single player: parameters bound at 0x828bd100; 0x828c6738 fires
/// SET_FOR_SINGLEPLAYER; career progression (0x824b9718, built 0x824bef10)
/// gives band L (0x82996948) and points XP(L + 1) - Main/XP (0x82995118)
/// with units 58 (0x82d6fc50); thumbnail 0x8299cc90; 0x828c70f8 sets the car
/// line "{0} {1}" (0x82007f84, query 0x8207e500) and credits unit 22
/// (0x82d6faf8, 0x82597a08), factor 1 (0x82d69a50), grouped (0x82d6fec0).
///
/// Missing: the last band's Horizon Final texts (0x828c6dec, table argument
/// not traced), the "collect" colour (0x82996188), the class badge
/// (0x825e6518, 0x825e6620, 0x828c7178; hidden) and the online level bar.
class PlayerBar {
public:
    /// Does nothing when `bar` has no PLAYER_BAR contract.
    void bind(const Presentation& scene, UiRenderer& renderer, int bar);
    bool bound() const { return m_contract >= 0; }

    /// `metric` picks the separator (see thousandsSeparatorId).
    static PlayerBarTexts texts(const PlayerBarValues& values, const std::vector<WristbandLevel>& levels,
        const Database* db, Localisation& names, UiStrings& strings, bool metric);

    void set(Presentation& scene, const PlayerBarTexts& texts);
    /// Call before evaluating the scene.
    void layout(Presentation& scene, UiRenderer& renderer) const;
    void apply(std::vector<DrawItem>& items) const;

    int contract() const { return m_contract; }
    int wristbandText() const { return m_wristband; }
    int nextLevelText() const { return m_nextLevel; }
    int carText() const { return m_car; }
    int creditsText() const { return m_credits; }

private:
    int m_contract = -1;
    int m_wristband = -1;
    int m_wristbandMaterial = -1;
    int m_car = -1;
    int m_credits = -1;
    int m_nextLevel = -1;
    int m_classBadge = -1;
    std::string m_wristbandTexture;
    SuperStackers m_stackers;
};

/// The locale records (0x82d74a48) give English "en" (miles,
/// IDS_NLSFormat_ThousandsSeparator_NA) and "en-eu" (kilometres,
/// IDS_NLSFormat_ThousandsSeparator). Inferred: metric means the second.
std::string_view thousandsSeparatorId(bool metric);

} // namespace openhorizon::ui
