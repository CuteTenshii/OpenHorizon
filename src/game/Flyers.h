#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class Vfs;

/// The discount signs ("flyers") of a game mode and which of them the player
/// has smashed.
///
/// `media/gamemodes/<mode>/Flyers.xml` lists one `ActivityFlyers` activity
/// per sign, named by the GameplayID of the sign's GameObjs.xml entry
/// (flyer_001 to flyer_100 for Colorado). The executable loads it as
/// "GameModes\%s\Flyers.xml"; that the mode folder is the track's name is
/// inferred from Colorado having both.
///
/// What the game does on a smash, read from its code: it counts the
/// collected and total ActivityFlyers activities (call at 0x82972b08), sets
/// the profile value Main/UpgradeDiscount (ForzaProfile.sch, UInt32) to the
/// collected count when it differs (0x8298d4f8, counting with 0x82988298),
/// and posts a notification (0x82972b48 to 0x82972d2c) built from the
/// Activities string table: IDS_FlyerCollected "{0}/{1} DISCOUNT SIGNS
/// SMASHED" with the two counts and IDS_FlyerCollected_Message "Total
/// upgrades discount: {0}" with Main/UpgradeDiscount. How the discount value
/// is formatted (a percent sign or not) is not decoded.
class Flyers {
public:
    /// Reads `media/gamemodes/<mode>/Flyers.xml`. Throws DataError if it is
    /// missing or malformed.
    static Flyers load(const Vfs& vfs, std::string_view mode);
    /// A set of the given GameplayIDs, none collected; for tests and tools.
    static Flyers fromIds(std::vector<std::string> ids);

    /// Signs in the game mode.
    std::size_t total() const { return m_ids.size(); }
    /// Signs smashed so far.
    std::size_t collectedCount() const { return m_collectedCount; }
    /// Main/UpgradeDiscount as the game sets it: one per smashed sign.
    std::uint32_t upgradeDiscount() const { return static_cast<std::uint32_t>(m_collectedCount); }

    /// True if `gameplayId` (ignoring case) is one of the signs.
    bool isFlyer(std::string_view gameplayId) const { return indexOf(gameplayId).has_value(); }
    bool isCollected(std::string_view gameplayId) const;
    /// Marks the sign collected. Returns true only the first time, so the
    /// caller notifies once per sign; false for ids that are not signs.
    bool collect(std::string_view gameplayId);
    /// The sign smashed most recently, or nothing before the first.
    std::optional<std::string> lastCollected() const;

    /// The collected signs' ids in list order, for saving.
    std::vector<std::string> collectedIds() const;
    /// Marks exactly `ids` collected (unknown ids are ignored), as after
    /// loading a save. Clears lastCollected().
    void restore(const std::vector<std::string>& ids);

    const std::vector<std::string>& ids() const { return m_ids; }

private:
    std::optional<std::size_t> indexOf(std::string_view gameplayId) const;

    std::vector<std::string> m_ids;
    std::vector<bool> m_collected;
    std::size_t m_collectedCount = 0;
    std::optional<std::size_t> m_last;
};

} // namespace openhorizon
