#pragma once

#include "RadioSequencer.h"

#include "audio/Decode.h"
#include "audio/Fsb.h"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace fh {
class Vfs;
}

namespace fh::radio {

/// The radio's files on the disc, under media/audio/radio (verified: the
/// paths are built at 0x82c9caa0 from "Game:\Media\Audio\Radio\" and the
/// System element's fsbMusic and fsbVO):
///
/// - RadioSystem.xml: stations, playlists, idents, DJ lines, levels, fades.
/// - <fsbMusic>.fsb (Radio_Music.fsb, 680 MB, 127 MPEG samples at 48 kHz):
///   every music track, each with SongStart, EventStart and IdentStart sync
///   points.
/// - <fsbVO>_<language>.fsb (Radio_VO_EN.fsb and five other languages, 536
///   MPEG samples): idents, with a StartNextTrack sync point, and DJ lines.
/// - RadioSoundbankInfo_Music.xml and RadioSoundbankInfo_VO_<language>.xml:
///   each sound's sample index and cue points (see SoundBankInfo). The game
///   loads the voice info for its language and falls back to the English
///   file, whose path it holds in full (0x82c9c8b4).
/// - The .lst files beside the banks are the banks' build lists; the game
///   reads them only when told to regenerate the sound bank info.
///
/// The voice bank's language: the game lists the Radio_VO_*.fsb files and
/// passes "EN" as the default (0x82d74960); that it takes the console's
/// language when a bank exists for it is inferred from that. Here the
/// caller names the language and English is used when there is no bank
/// for it.
///
/// The banks are read in place, never loaded whole: only their headers are
/// parsed, and samples are streamed from the files. The banks must therefore
/// be loose files on the disc (they are on every known disc layout); a disc
/// that has them only inside an archive throws DataError.
class RadioAssets {
public:
    /// Throws DataError if a file is missing or malformed.
    static std::shared_ptr<const RadioAssets> load(const Vfs& vfs, std::string_view voiceLanguage = "EN");

    ~RadioAssets();
    RadioAssets(const RadioAssets&) = delete;
    RadioAssets& operator=(const RadioAssets&) = delete;

    std::shared_ptr<const RadioCatalog> catalog() const { return m_catalog; }
    const audio::FsbBank& bank(RadioBank bank) const;
    /// The language code of the voice bank in use, e.g. "EN".
    const std::string& voiceLanguage() const { return m_voiceLanguage; }

    /// Reads the bank's file; safe to call from any thread.
    audio::ByteSource source(RadioBank bank) const;
    /// A progressive decoder for one sample, starting at `startFrame`.
    audio::MpegSampleStream openStream(RadioBank bank, int sampleIndex, std::size_t startFrame) const;

private:
    struct BankFile;

    RadioAssets();

    std::shared_ptr<const RadioCatalog> m_catalog;
    std::string m_voiceLanguage;
    std::shared_ptr<BankFile> m_music;
    std::shared_ptr<BankFile> m_voice;
};

} // namespace fh::radio
