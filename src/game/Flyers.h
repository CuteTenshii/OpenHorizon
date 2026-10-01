#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openhorizon {

class Vfs;

/// The discount signs ("flyers") of a game mode: one `ActivityFlyers` per
/// sign in Flyers.xml, named by its GameObjs.xml GameplayID.
/// Verified: a smash counts the signs (0x82972b08, 0x82988298), sets
/// Main/UpgradeDiscount (UInt32) to the collected count (0x8298d4f8) and posts
/// IDS_FlyerCollected (0x82972b48-0x82972d2c). Inferred: the mode folder is
/// the track's name. Missing: the discount's number format.
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
