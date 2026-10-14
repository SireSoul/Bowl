#pragma once

#include "ph/core/function_ref.hpp"
#include "ph/core/types.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace ph {

// Counts outstanding jobs. Wait on it with JobSystem::wait, which helps run jobs instead of sleeping.
class TaskGroup {
public:
    bool done() const { return pending_.load(std::memory_order_acquire) == 0; }

private:
    friend class JobSystem;
    std::atomic<u32> pending_{0};
};

// A fixed pool of worker threads. The thread that waits always helps, so a pool with 0 workers still works
// (everything runs inline) — that is what single-threaded platforms and deterministic test runs use.
class JobSystem : NonCopyable {
public:
    // workers < 0: hardware threads - 1.
    explicit JobSystem(i32 workers = -1);
    ~JobSystem();

    u32 worker_count() const { return static_cast<u32>(threads_.size()); }
    // Workers plus the calling thread.
    u32 concurrency() const { return worker_count() + 1; }

    void run(TaskGroup& group, std::function<void()> job);
    void wait(TaskGroup& group);

    // Calls fn(begin, end) over [0, count) in contiguous chunks of at least min_chunk items, and returns when all are
    // done. Chunk boundaries depend only on count and min_chunk — never on thread count — so per-chunk results merge
    // deterministically.
    void parallel_for(usize count, usize min_chunk, FunctionRef<void(usize begin, usize end)> fn);

    // Items per chunk parallel_for uses for this count; chunk i covers [i*size, min(count, (i+1)*size)).
    static usize chunk_size(usize count, usize min_chunk);

private:
    struct Job {
        std::function<void()> fn;
        TaskGroup* group;
    };
    bool try_run_one();
    void worker_main();

    std::vector<std::thread> threads_;
    std::deque<Job> queue_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
};

} // namespace ph
