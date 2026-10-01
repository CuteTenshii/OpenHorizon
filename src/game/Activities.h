#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openhorizon {

class Vfs;

/// One `<TriggerZone>` of an activity, as its parser (0x8298af28) reads it.
/// Verified: objects name GameObjs.xml objects (0x8296dca0); prompts are
/// Activities string ids, IDS_EnterNow by default (0x8298ae90); `maxMPH` is
/// scaled by 0.44704 (0x82005aec); both flags default to true (flag byte
/// 0x8C, 0x8298af00); gas stations add `triggerZoneRadius` and
/// `tutorialRadius` (0x82978344).
struct ActivityZone {
    std::string object;
    std::string secondObject;
    std::string name;
    float radius = 0.0F;
    std::string promptTitle;
    std::string prompt;
    /// The zone's speed limit in metres per second, when it has one.
    std::optional<float> maxSpeed;
    std::string mapTag;
    std::string mapDescription;
    bool isActivation = true;
    bool requiresInput = true;
    std::optional<float> triggerZoneRadius;
};

/// One `<Activity>` of a game mode's activity files.
struct Activity {
    /// The activity class ("ActivitySpeedCamera", "ActivityGasStation", ...).
    std::string type;
    std::string name;
    /// `scoreboard_id` (0x829582bc); -1 when it has none.
    int scoreboardId = -1;
    /// `<SpeedThreshold minimumSpeed>`, in miles per hour (0x8296cedc).
    std::optional<float> minimumSpeedMph;
    std::vector<ActivityZone> zones;
    /// `<Missions><Mission name>`: the missions an outpost offers.
    std::vector<std::string> missions;
    /// The file under media/gamemodes/<mode> it came from.
    std::string file;
};

/// Every activity of a game mode, read from the files the executable names
/// for a mode, in its order (strings 0x822401xx-0x82240508). E3_speed_camera.xml
/// is left out as the E3 demo's (inferred from its name).
class ActivityCatalogue {
public:
    /// Reads the mode's activity files; missing files are skipped. Throws
    /// DataError if a file is not XML.
    static ActivityCatalogue load(const Vfs& vfs, std::string_view mode);
    /// Parses one activity file's text; `file` names it in errors and in
    /// Activity::file. Throws DataError if it is not XML.
    static std::vector<Activity> parse(std::string_view text, const std::string& file);
    /// A catalogue of the given activities; for tests and tools.
    static ActivityCatalogue fromActivities(std::vector<Activity> activities);

    /// The file names read, relative to the mode's folder.
    static const std::vector<std::string_view>& fileNames();

    const std::vector<Activity>& all() const { return m_activities; }
    /// The activities of one class, in file order.
    std::vector<const Activity*> ofType(std::string_view type) const;
    /// The first zone, of any activity, whose object is `object` (ignoring
    /// case), with its activity.
    std::optional<std::pair<const Activity*, const ActivityZone*>> zoneFor(std::string_view object) const;

private:
    std::vector<Activity> m_activities;
};

} // namespace openhorizon
