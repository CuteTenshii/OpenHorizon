#pragma once

#include <string>
#include <string_view>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::audio {

/// audiomixersnapshots.xml's DynamicsData/CrashData (read at 0x82C72C80 into +0x5F94 threshold, +0x5F98 drop)
/// and the mixer's check (0x82C657F8, from 0x82C74520).
/// Verified: the focus car's speed (m/s, slot 9 at 0x82C65718) times 2.2369 is mph; the Crash snapshot (id 15)
/// applies when the previous update's speed was above `threshold` and fell by more than `drop`. Disc: 50, 10.
/// Inferred: one check per mix update; the game's update rate was not traced.
/// Not reproduced: the flag it sets and clears above 5 mph (+0x5F9C), and the mixer's skip flags (+4, +5).
class CrashMix {
public:
    /// 0 without CrashData (0x82C73E40 zeroes the speed kept); only the loader sets them.
    float threshold = 0.0F;
    float drop = 0.0F;

    /// Throws DataError naming `source` if the text is not XML with a MixSnapshots root.
    static CrashMix parse(std::string_view xml, const std::string& source);
    /// Throws DataError if the file is missing or malformed.
    static CrashMix load(const Vfs& vfs);

    /// `speed` in metres per second; true when the Crash snapshot is to be applied.
    bool update(float speed);
    /// Forgets the speed kept, as when the car is placed somewhere new.
    void reset() { m_previousMph = 0.0F; }

private:
    float m_previousMph = 0.0F;
};

} // namespace openhorizon::audio
