#include "bowl/core/jobs.hpp"

#include "bowl/core/profiler.hpp"

#include <algorithm>
#include <string>

namespace bowl {

JobSystem::JobSystem(i32 workers) {
    if (workers < 0) workers = std::max(0, static_cast<i32>(std::thread::hardware_concurrency()) - 1);
    threads_.reserve(static_cast<usize>(workers));
    for (i32 i = 0; i < workers; ++i)
        threads_.emplace_back([this, i] {
            profiler::set_thread_name(("worker " + std::to_string(i + 1)).c_str());
            worker_main();
        });
}

JobSystem::~JobSystem() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (auto& t : threads_) t.join();
}

void JobSystem::run(TaskGroup& group, std::function<void()> job) {
    group.pending_.fetch_add(1, std::memory_order_relaxed);
    if (threads_.empty()) {
        job();
        group.pending_.fetch_sub(1, std::memory_order_release);
        return;
    }
    {
        std::lock_guard lock(mutex_);
        queue_.push_back({std::move(job), &group});
    }
    wake_.notify_one();
}

bool JobSystem::try_run_one() {
    Job job;
    {
        std::lock_guard lock(mutex_);
        if (queue_.empty()) return false;
        job = std::move(queue_.front());
        queue_.pop_front();
    }
    job.fn();
    job.group->pending_.fetch_sub(1, std::memory_order_release);
    return true;
}

void JobSystem::wait(TaskGroup& group) {
    while (!group.done()) {
        if (!try_run_one()) std::this_thread::yield();
    }
}

void JobSystem::worker_main() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
            if (queue_.empty()) return; // stopping
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        job.fn();
        job.group->pending_.fetch_sub(1, std::memory_order_release);
    }
}

usize JobSystem::chunk_size(usize count, usize min_chunk) {
    constexpr usize max_chunks = 64;
    return std::max<usize>({min_chunk, (count + max_chunks - 1) / max_chunks, 1});
}

void JobSystem::parallel_for(usize count, usize min_chunk, FunctionRef<void(usize, usize)> fn) {
    if (count == 0) return;
    usize size = chunk_size(count, min_chunk);
    if (size >= count || threads_.empty()) {
        for (usize begin = 0; begin < count; begin += size) fn(begin, std::min(count, begin + size));
        return;
    }
    TaskGroup group;
    // The caller runs the first chunk itself; the rest go to the pool.
    for (usize begin = size; begin < count; begin += size) {
        usize end = std::min(count, begin + size);
        run(group, [fn, begin, end] { fn(begin, end); });
    }
    fn(0, size);
    wait(group);
}

} // namespace bowl
