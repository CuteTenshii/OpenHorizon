#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openhorizon {

class Vfs;

/// One `<TriggerZone>` of an activity, as the zone's parser (0x8298af28)
/// reads it. Verified there: `object` and `secondObject` name GameObjs.xml
/// objects (looked up by name through 0x8296dca0); `radius` is stored as
/// read; `promptTitle` and `prompt` are ids in the Activities string table;
/// `maxMPH` is multiplied by 0.44704 (miles per hour to metres per second,
/// 0x82005aec) on load; `is_activation` and `requires_input` default to true
/// (the flag byte starts at 0x8C, 0x8298af00). When no `prompt` names the
/// call to action, the zone's is the Activities string IDS_EnterNow (set by
/// the zone's constructor, 0x8298ae90). Some activities read further
/// attributes of their own zones: the gas stations `triggerZoneRadius` and
/// `tutorialRadius` (0x82978344), the map ones `mapTag` and
/// `mapDescription`.
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
    /// The leaderboard the activity posts to (`scoreboard_id`, read by
    /// 0x829582bc into the activity at +236); -1 when it has none.
    int scoreboardId = -1;
    /// `<SpeedThreshold minimumSpeed>`, in miles per hour: the speed trap
    /// compares it with the car's speed times 2.2369 (0x8296cedc).
    std::optional<float> minimumSpeedMph;
    std::vector<ActivityZone> zones;
    /// `<Missions><Mission name>`: the missions an outpost offers.
    std::vector<std::string> missions;
    /// The file under media/gamemodes/<mode> it came from.
    std::string file;
};

/// Every activity of a game mode (a track's folder under media/gamemodes),
/// read from the files the executable names for a mode (the
/// "GameModes\%s\..." strings from 0x822401xx to 0x82240508, Leaderboards.xml
/// to state_definitions.xml), in that order. E3_speed_camera.xml is left
/// out: it repeats speed_camera_30 with the E3 demo's graded thresholds, and
/// taking it for the demo's file is inferred from its name. Only files whose
/// root is `<ActivityManager>` hold activities; commented-out activities are
/// skipped, as XML comments are.
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
