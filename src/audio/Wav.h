#pragma once

#include "Decode.h"

#include "base/Bytes.h"

#include <filesystem>

namespace openhorizon::audio {

enum class WavFormat {
    /// 16-bit integer PCM; samples outside [-1, 1] are clamped.
    Pcm16,
    /// 32-bit IEEE float (WAVE_FORMAT_IEEE_FLOAT), unclamped, so a render
    /// that clips can be told from one that does not.
    Float32,
};

/// A RIFF WAVE file holding `pcm`.
Bytes encodeWav(const PcmBuffer& pcm, WavFormat format);

/// Writes encodeWav() to `path`. Throws DataError if the file cannot be
/// written.
void writeWav(const std::filesystem::path& path, const PcmBuffer& pcm, WavFormat format);

} // namespace openhorizon::audio
