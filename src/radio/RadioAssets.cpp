#include "RadioAssets.h"

#include "base/Error.h"
#include "base/Log.h"
#include "vfs/Vfs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <format>

namespace fh::radio {

namespace {

constexpr std::string_view kFolder = "media/audio/radio/";

std::string upper(std::string_view text)
{
    std::string out(text);
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::string readText(const Vfs& vfs, const std::string& path)
{
    if (std::optional<std::string> text = vfs.readText(path)) {
        return std::move(*text);
    }
    throw DataError("{}: not found", path);
}

} // namespace

/// An open bank file and its parsed headers.
struct RadioAssets::BankFile {
    int fd = -1;
    std::string path;
    std::optional<audio::FsbBank> bank;

    ~BankFile()
    {
        if (fd >= 0) {
            ::close(fd);
        }
    }

    std::size_t readAt(std::uint64_t offset, std::span<std::uint8_t> out) const
    {
        std::size_t done = 0;
        while (done < out.size()) {
            const ssize_t got = ::pread(fd, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
            if (got < 0) {
                if (errno == EINTR) {
                    continue;
                }
                throw DataError("{}: read failed at 0x{:x}: {}", path, offset + done, std::strerror(errno));
            }
            if (got == 0) {
                break;
            }
            done += static_cast<std::size_t>(got);
        }
        return done;
    }

    static std::shared_ptr<BankFile> open(const Vfs& vfs, const std::string& gamePath)
    {
        const std::optional<std::filesystem::path> host = vfs.hostPath(gamePath);
        if (!host) {
            throw DataError("{}: not found as a loose file; the radio streams its banks from loose files", gamePath);
        }
        auto file = std::make_shared<BankFile>();
        file->path = host->string();
        file->fd = ::open(file->path.c_str(), O_RDONLY | O_CLOEXEC);
        if (file->fd < 0) {
            throw DataError("{}: cannot open: {}", file->path, std::strerror(errno));
        }
        struct stat info {};
        if (::fstat(file->fd, &info) != 0) {
            throw DataError("{}: cannot stat: {}", file->path, std::strerror(errno));
        }
        Bytes first(audio::FsbBank::kFileHeaderSize);
        if (file->readAt(0, first) != first.size()) {
            throw DataError("{}: too short for an FSB header", file->path);
        }
        Bytes headers(audio::FsbBank::headersSize(first, file->path));
        if (file->readAt(0, headers) != headers.size()) {
            throw DataError("{}: the sample headers run past the end of the file", file->path);
        }
        file->bank = audio::FsbBank::parseHeaders(std::move(headers), static_cast<std::uint64_t>(info.st_size),
            file->path);
        return file;
    }
};

RadioAssets::RadioAssets() = default;
RadioAssets::~RadioAssets() = default;

std::shared_ptr<const RadioAssets> RadioAssets::load(const Vfs& vfs, std::string_view voiceLanguage)
{
    std::shared_ptr<RadioAssets> assets(new RadioAssets());
    auto catalog = std::make_shared<RadioCatalog>();

    const std::string systemPath = std::string(kFolder) + "RadioSystem.xml";
    catalog->system = parseRadioSystem(readText(vfs, systemPath), systemPath);
    const RadioSettings& settings = catalog->system.settings;

    const std::string musicInfoPath = std::string(kFolder) + "RadioSoundbankInfo_Music.xml";
    catalog->musicInfo = parseSoundBankInfo(readText(vfs, musicInfoPath), musicInfoPath);

    std::string language = upper(voiceLanguage);
    const auto voiceBankPath = [&](const std::string& code) {
        return std::format("{}{}_{}.fsb", kFolder, settings.fsbVO, code);
    };
    if (!vfs.exists(voiceBankPath(language))) {
        logInfo("radio: no {} voice bank, using EN", language);
        language = "EN";
    }
    std::string voiceInfoPath = std::format("{}RadioSoundbankInfo_VO_{}.xml", kFolder, language);
    if (!vfs.exists(voiceInfoPath)) {
        voiceInfoPath = std::string(kFolder) + "RadioSoundbankInfo_VO_EN.xml";
    }
    catalog->voiceInfo = parseSoundBankInfo(readText(vfs, voiceInfoPath), voiceInfoPath);

    assets->m_music = BankFile::open(vfs, std::format("{}{}.fsb", kFolder, settings.fsbMusic));
    assets->m_voice = BankFile::open(vfs, voiceBankPath(language));
    assets->m_voiceLanguage = language;

    const auto timings = [](const audio::FsbBank& bank) {
        std::vector<SampleTiming> out;
        out.reserve(bank.size());
        for (const audio::FsbSample& s : bank.samples()) {
            SampleTiming t;
            t.sampleRate = s.frequency;
            t.frames = s.sampleCount;
            for (const audio::FsbSyncPoint& p : s.syncPoints) {
                t.syncPoints.push_back(p.frame);
            }
            out.push_back(std::move(t));
        }
        return out;
    };
    catalog->musicSamples = timings(*assets->m_music->bank);
    catalog->voiceSamples = timings(*assets->m_voice->bank);
    assets->m_catalog = std::move(catalog);
    return assets;
}

const audio::FsbBank& RadioAssets::bank(RadioBank bank) const
{
    return bank == RadioBank::Music ? *m_music->bank : *m_voice->bank;
}

audio::ByteSource RadioAssets::source(RadioBank bank) const
{
    std::shared_ptr<const BankFile> file = bank == RadioBank::Music ? m_music : m_voice;
    return [file](std::uint64_t offset, std::span<std::uint8_t> out) { return file->readAt(offset, out); };
}

audio::MpegSampleStream RadioAssets::openStream(RadioBank bank, int sampleIndex, std::size_t startFrame) const
{
    const audio::FsbBank& b = this->bank(bank);
    const audio::FsbSample& sample = b.sample(static_cast<std::size_t>(sampleIndex));
    return audio::MpegSampleStream(sample, source(bank), std::format("{} sample {} ('{}')", b.source(), sampleIndex,
        sample.name), startFrame);
}

} // namespace fh::radio
