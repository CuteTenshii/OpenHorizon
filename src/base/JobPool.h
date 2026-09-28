#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace fh {

/// Worker threads that run queued jobs, lowest priority value first; for
/// streaming work (decoding world tiles and textures) whose order should
/// follow the camera. Jobs must not throw.
class JobPool {
public:
    /// `threads` of 0 uses the hardware concurrency minus one, at least one.
    explicit JobPool(unsigned threads = 0);
    ~JobPool();
    JobPool(const JobPool&) = delete;
    JobPool& operator=(const JobPool&) = delete;

    void submit(float priority, std::function<void()> job);
    /// Drops every job not yet started.
    void clear();
    std::size_t pending() const;
    unsigned threadCount() const { return static_cast<unsigned>(m_threads.size()); }

private:
    struct Entry {
        float priority = 0.0F;
        std::uint64_t order = 0;
        std::function<void()> job;
        bool operator<(const Entry& other) const
        {
            // std::priority_queue pops the largest; invert so the lowest
            // priority value, then the oldest, comes out first.
            if (priority != other.priority) {
                return priority > other.priority;
            }
            return order > other.order;
        }
    };

    void run();

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::priority_queue<Entry> m_queue;
    std::uint64_t m_nextOrder = 0;
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
};

} // namespace fh
