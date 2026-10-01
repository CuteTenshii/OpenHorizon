#pragma once

#include "RadioSequencer.h"

#include "audio/Decode.h"
#include "audio/Fsb.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace openhorizon {
class Vfs;
}

namespace openhorizon::radio {

/// The radio's files under media/audio/radio (paths built at 0x82c9caa0): RadioSystem.xml, the <fsbMusic>.fsb and
/// <fsbVO>_<language>.fsb banks and their RadioSoundbankInfo_*.xml. Voice falls back to English (0x82c9c8b4,
/// 0x82d74960; using the console's language is inferred). Banks are streamed in place, so must be loose files.
class RadioAssets {
public:
    /// Throws DataError if a file is missing or malformed.
    static std::shared_ptr<const RadioAssets> load(const Vfs& vfs, std::string_view voiceLanguage = "EN");

    ~RadioAssets();
    RadioAssets(const RadioAssets&) = delete;
    RadioAssets& operator=(const RadioAssets&) = delete;

    std::shared_ptr<const RadioCatalog> catalog() const { return m_catalog; }
    const audio::FsbBank& bank(RadioBank bank) const;
    /// E.g. "EN".
    const std::string& voiceLanguage() const { return m_voiceLanguage; }

    /// Safe to call from any thread.
    audio::ByteSource source(RadioBank bank) const;
    audio::MpegSampleStream openStream(RadioBank bank, int sampleIndex, std::size_t startFrame) const;

private:
    struct BankFile;

    RadioAssets();

    std::shared_ptr<const RadioCatalog> m_catalog;
    std::string m_voiceLanguage;
    std::shared_ptr<BankFile> m_music;
    std::shared_ptr<BankFile> m_voice;
};

} // namespace openhorizon::radio
