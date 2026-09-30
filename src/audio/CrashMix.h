#pragma once

#include <string>
#include <string_view>

namespace fh {
class Vfs;
}

namespace fh::audio {

/// When a crash takes over the mix: the CrashData element of
/// media/audio/audiomixersnapshots.xml's DynamicsData, which the snapshot
/// loader stores in the snapshot mixer (threshold at +0x5F94, drop at
/// +0x5F98, read at 0x82C72C80), and the check the mixer's update runs
/// before its fades (0x82C657F8, called from 0x82C74520).
///
/// Verified: each update takes the focus car's speed (metres per second,
/// set through the mixer's slot 9 at 0x82C65718), multiplies it by 2.2369,
/// and applies the Crash snapshot (id 15) when the speed of the previous
/// update was above `threshold` miles per hour and has fallen by more than
/// `drop` since; the speed is then kept for the next update. The disc's
/// file has threshold 50 and drop 10.
///
/// Inferred: the check runs once per update of the mix, as the game's does;
/// the game's update rate was not traced, so a crash is caught by how much
/// the speed falls between two frames at whatever rate the game runs.
/// Not reproduced: the flag the check also sets and clears again above
/// 5 mph (+0x5F9C), which it does not read itself; and the mixer's two
/// flags that skip the check with the fades (+4, +5), whose setters were
/// not identified.
class CrashMix {
public:
    /// The constructor's values when the file has no CrashData: 0, as the
    /// fields start (0x82C73E40 zeroes the speed kept; the threshold and
    /// drop are only ever set by the loader).
    float threshold = 0.0F;
    float drop = 0.0F;

    /// Throws DataError naming `source` if the text is not XML with a
    /// MixSnapshots root.
    static CrashMix parse(std::string_view xml, const std::string& source);
    /// Throws DataError if the file is missing or malformed.
    static CrashMix load(const Vfs& vfs);

    /// Feeds the focus car's speed in metres per second for one update;
    /// true when the Crash snapshot is to be applied.
    bool update(float speed);
    /// Forgets the speed kept, as when the car is placed somewhere new.
    void reset() { m_previousMph = 0.0F; }

private:
    float m_previousMph = 0.0F;
};

} // namespace fh::audio
