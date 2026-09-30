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

/// Writes an integer as the game's number formatter does (0x82d6d008, the
/// digits of the unit formatters 0x82d6faf8 and 0x82d6fc50): the value
/// clamped to +-999,999,999, a minus sign when negative, and `separator`
/// before every group of three digits but the first.
std::string formatGroupedInteger(std::int64_t value, std::string_view separator);

/// One row of gamedb's CareerWristbandLevels, the career's wristbands in
/// order of Id.
struct WristbandLevel {
    int id = 0;
    /// The Main/XP a player needs for this band.
    std::int64_t xp = 0;
    /// The band's name, resolved through the CareerWristbandLevels table.
    std::string name;
    /// The image under media\UI\Textures\Horizon\Wristbands, without its
    /// extension.
    std::string thumbnail;
};

/// Reads CareerWristbandLevels as the career's progression object loads it
/// (0x8259eae8: "SELECT Id,XP FROM %s ORDER BY Id"; the name and thumbnail
/// through 0x8299b990 and 0x8299cc90 for one Id at a time). Throws
/// DataError when the table cannot be read.
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

/// The PLAYER_BAR contract that front-end scenes such as h_map carry: the
/// player's wristband, the points to the next one, the car and the credits.
///
/// Verified from the executable, for a single-player session:
/// - The contract's parameters are bound at 0x828bd100: TEXT_SP_WRISTBAND
///   (+108), MATERIAL_SP_WRISTBAND (+120), TEXT_PLAYER_CAR (+148),
///   TEXT_PLAYER_CREDITS (+164), TEXT_NEXT_LEVEL (+176).
/// - 0x828c6738 fires SET_FOR_SINGLEPLAYER outside multiplayer and the
///   first-time flow. The career progression object (0x824b9718 picks the
///   career one outside rally and online modes, created at 0x824bef10 over
///   CareerWristbandLevels, Main/WristbandLevel and Main/XP) gives the band
///   L (0x82996948) and, while L is below the last band, the points to the
///   next, XP(L + 1) - Main/XP (0x82995118). Points above zero show as
///   UnitStrings IDS_Units_PTS (unit 58, 0x82d6fc50) in TEXT_SP_WRISTBAND
///   with InGame IDS_WristbandNextLevel in TEXT_NEXT_LEVEL; none left shows
///   InGame IDS_CollectWristband with the next band's name and an empty
///   TEXT_SP_WRISTBAND. MATERIAL_SP_WRISTBAND takes
///   media\UI\Textures\Horizon\Wristbands\<Thumbnail of L>.tga (0x8299cc90).
/// - 0x828c70f8 sets TEXT_PLAYER_CAR to "{0} {1}" (a literal of the
///   executable, 0x82007f84) with the make's and the car's DisplayName
///   (the query at 0x8207e500) and TEXT_PLAYER_CREDITS to Main/Credits
///   (0x82597a08) as UnitStrings IDS_Units_Credits (unit type 22,
///   0x82d6faf8). Both units convert with a factor of 1 (0x82d69a50) and
///   group their digits (their unit types are not 6, 0x82d6fec0).
///
/// Missing: at the last band the game asks gamedb for the Horizon Final's
/// unlock (0x828c6dec: UnlockPointsReq and the event's place) to choose
/// between IDS_WristbandToFinal and IDS_WristbandGetToHorizonFinal; that
/// query's table argument was not traced, so both texts stay empty there.
/// The colour 0x82996188 gives the "collect" text (from the bands' Color
/// column, whose conversion was not traced) is not applied. The class badge
/// (COMPONENT_CLASS_BADGE: the car's class and PI, 0x825e6518 and
/// 0x825e6620, given to the badge at 0x828c7178) is not traced, so it is
/// hidden rather than left showing its authored placeholder; the online
/// level bar is not set.
class PlayerBar {
public:
    /// Binds the PLAYER_BAR contract on `bar` and the stackers below it.
    /// Does nothing when `bar` has no such contract.
    void bind(const Presentation& scene, UiRenderer& renderer, int bar);
    bool bound() const { return m_contract >= 0; }

    /// The texts for `values`; `metric` picks the thousands separator of the
    /// locale the setting stands for (see thousandsSeparatorId).
    static PlayerBarTexts texts(const PlayerBarValues& values, const std::vector<WristbandLevel>& levels,
        const Database* db, Localisation& names, UiStrings& strings, bool metric);

    /// Shows `texts` in the single-player layout.
    void set(Presentation& scene, const PlayerBarTexts& texts);
    /// Lays the rows out for their texts; call before evaluating the scene.
    void layout(Presentation& scene, UiRenderer& renderer) const;
    /// Points the wristband material at the band's image.
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

/// The UnitStrings id of the thousands separator. The locale records the
/// executable builds (0x82d74a48) give English two: "en" with unit system 0
/// (miles) and IDS_NLSFormat_ThousandsSeparator_NA, and "en-eu" with unit
/// system 2 (kilometres) and IDS_NLSFormat_ThousandsSeparator. That the
/// metric setting stands for the second is inferred; the records of other
/// languages were not read.
std::string_view thousandsSeparatorId(bool metric);

} // namespace openhorizon::ui
