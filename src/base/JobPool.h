#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace openhorizon {

/// Worker threads running queued jobs, lowest priority value first. Jobs
/// must not throw.
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
    /// Drops every job not yet started and waits for running ones, so their
    /// captures can be destroyed. Must not be called from a job.
    void clearAndWait();
    /// Jobs queued and not started yet.
    std::size_t pending() const;
    /// Jobs queued or running.
    std::size_t outstanding() const;
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
    std::condition_variable m_idle;
    unsigned m_running = 0;
    std::priority_queue<Entry> m_queue;
    std::uint64_t m_nextOrder = 0;
    bool m_stopping = false;
    std::vector<std::thread> m_threads;
};

} // namespace openhorizon
