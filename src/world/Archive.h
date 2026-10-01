#pragma once

#include "base/Bytes.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace lzx {
class ZipArchive;
struct ZipEntry;
} // namespace lzx

namespace openhorizon::world {

/// Reports how far a long job has come: `done` of `total` items.
using Progress = std::function<void(std::size_t done, std::size_t total)>;

/// Throws DataError, naming the entry, if it cannot be read or decoded.
Bytes readEntry(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry);

/// The first `size` bytes of an entry, decompressing only the LZX chunks that cover them; the CRC goes unchecked.
/// Throws DataError if the entry cannot be read.
Bytes readEntryPrefix(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry, std::size_t size);

/// Indexes of the entries ending with `suffix` (ignoring case), one per distinct name, in archive order.
/// A track's bin.zip stores most files several times, some names differing only in case, with identical contents.
std::vector<std::size_t> distinctEntries(const lzx::ZipArchive& archive, std::string_view suffix);

/// The digits between `prefix` and the trailing `suffix` (ignoring case): ("coloradoout.06454.rmb.bin", '.',
/// ".rmb.bin") gives 6454. Nothing if the shape differs or the number overflows 32 bits.
std::optional<std::uint32_t> numberBefore(std::string_view text, char prefix, std::string_view suffix);

/// Runs `job(i)` for every i below `count` on all hardware threads. `progress` is never called from two threads at
/// once; the first exception a job throws stops the rest and is rethrown here.
template <typename Job> void parallelFor(std::size_t count, Job&& job, const Progress& progress = {})
{
    constexpr std::size_t kProgressStep = 2048;
    const std::size_t workers
        = std::min<std::size_t>(count, std::max<std::size_t>(1, std::thread::hardware_concurrency()));
    std::atomic<std::size_t> next{0};
    std::atomic<std::size_t> finished{0};
    std::atomic<bool> stop{false};
    std::exception_ptr failure;
    std::mutex mutex;
    const auto work = [&] {
        while (!stop.load(std::memory_order_relaxed)) {
            const std::size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= count) {
                return;
            }
            try {
                job(i);
            } catch (...) {
                const std::lock_guard lock(mutex);
                if (!failure) {
                    failure = std::current_exception();
                }
                stop = true;
                return;
            }
            const std::size_t done = finished.fetch_add(1, std::memory_order_relaxed) + 1;
            if (progress && done % kProgressStep == 0) {
                const std::lock_guard lock(mutex);
                progress(done, count);
            }
        }
    };
    std::vector<std::thread> threads;
    threads.reserve(workers > 0 ? workers - 1 : 0);
    for (std::size_t t = 1; t < workers; ++t) {
        threads.emplace_back(work);
    }
    work();
    for (std::thread& thread : threads) {
        thread.join();
    }
    if (failure) {
        std::rethrow_exception(failure);
    }
    if (progress) {
        progress(count, count);
    }
}

} // namespace openhorizon::world
