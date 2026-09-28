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

namespace fh::world {

/// Reports how far a long job has come: `done` of `total` items.
using Progress = std::function<void(std::size_t done, std::size_t total)>;

/// The entry's contents. Throws DataError, naming the entry, if it cannot be
/// read or decoded.
Bytes readEntry(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry);

/// The first `size` bytes of an entry (all of it, if shorter), decompressing
/// only the LZX chunks that cover them. The CRC cannot be checked on a partial
/// read. Throws DataError if the entry cannot be read.
Bytes readEntryPrefix(const lzx::ZipArchive& archive, const lzx::ZipEntry& entry, std::size_t size);

/// Indexes into `archive.entries()` of the entries whose names end with
/// `suffix` (ignoring case), one per distinct name, in archive order. A
/// track's bin.zip stores most files several times, some under names that
/// differ only in case, with identical contents; one copy is enough.
std::vector<std::size_t> distinctEntries(const lzx::ZipArchive& archive, std::string_view suffix);

/// The number written right before `suffix` at the end of `text`, where the
/// digits follow the character `prefix` (letters match ignoring case): for
/// ("coloradoout.06454.rmb.bin", '.', ".rmb.bin") that is 6454. Nothing if
/// the text does not have that shape or the number does not fit 32 bits.
std::optional<std::uint32_t> numberBefore(std::string_view text, char prefix, std::string_view suffix);

/// Runs `job(i)` for every i below `count` on all hardware threads and waits
/// for them. `progress`, if set, is called now and then with the number of
/// finished items, never from two threads at once. The first exception a job
/// throws stops the remaining work and is rethrown here.
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

} // namespace fh::world
