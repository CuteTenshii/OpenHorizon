#include "JobPool.h"

#include <algorithm>

namespace fh {

JobPool::JobPool(unsigned threads)
{
    if (threads == 0) {
        const unsigned hardware = std::thread::hardware_concurrency();
        threads = hardware > 1 ? hardware - 1 : 1;
    }
    m_threads.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        m_threads.emplace_back([this] { run(); });
    }
}

JobPool::~JobPool()
{
    {
        const std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_queue = {};
    }
    m_wake.notify_all();
    for (std::thread& thread : m_threads) {
        thread.join();
    }
}

void JobPool::submit(float priority, std::function<void()> job)
{
    {
        const std::lock_guard lock(m_mutex);
        m_queue.push(Entry{priority, m_nextOrder++, std::move(job)});
    }
    m_wake.notify_one();
}

void JobPool::clear()
{
    const std::lock_guard lock(m_mutex);
    m_queue = {};
}

void JobPool::clearAndWait()
{
    std::unique_lock lock(m_mutex);
    m_queue = {};
    m_idle.wait(lock, [this] { return m_running == 0; });
}

std::size_t JobPool::pending() const
{
    const std::lock_guard lock(m_mutex);
    return m_queue.size();
}

void JobPool::run()
{
    while (true) {
        std::function<void()> job;
        {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stopping || !m_queue.empty(); });
            if (m_stopping) {
                return;
            }
            job = std::move(const_cast<Entry&>(m_queue.top()).job);
            m_queue.pop();
            ++m_running;
        }
        job();
        // Destroy the job's captures before reporting idle: they may hold
        // the last reference to something its owner is tearing down.
        job = nullptr;
        {
            const std::lock_guard lock(m_mutex);
            --m_running;
        }
        m_idle.notify_all();
    }
}

} // namespace fh
