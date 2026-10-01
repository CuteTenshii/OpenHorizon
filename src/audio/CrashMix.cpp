#include "CrashMix.h"

#include "base/Error.h"
#include "vfs/Vfs.h"

#include <pugixml.hpp>

#include <optional>

namespace openhorizon::audio {

namespace {

constexpr const char* kSnapshotsPath = "media/audio/audiomixersnapshots.xml";
/// The executable's constant (0x82005AE8).
constexpr float kMphPerMetrePerSecond = 2.2369F;

} // namespace

CrashMix CrashMix::parse(std::string_view xml, const std::string& source)
{
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size())) {
        throw DataError("{}: not XML", source);
    }
    const pugi::xml_node root = document.child("MixSnapshots");
    if (!root) {
        throw DataError("{}: no MixSnapshots root", source);
    }
    CrashMix crash;
    const pugi::xml_node data = root.child("DynamicsData").child("CrashData");
    crash.threshold = data.attribute("threshold").as_float(crash.threshold);
    crash.drop = data.attribute("drop").as_float(crash.drop);
    return crash;
}

CrashMix CrashMix::load(const Vfs& vfs)
{
    const std::optional<std::string> text = vfs.readText(kSnapshotsPath);
    if (!text) {
        throw DataError("{} is missing", kSnapshotsPath);
    }
    return parse(*text, kSnapshotsPath);
}

bool CrashMix::update(float speed)
{
    const float mph = speed * kMphPerMetrePerSecond;
    const bool crashed = m_previousMph - mph > drop && m_previousMph > threshold;
    m_previousMph = mph;
    return crashed;
}

} // namespace openhorizon::audio
