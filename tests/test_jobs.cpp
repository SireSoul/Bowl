#include "ph/core/jobs.hpp"

#include <doctest/doctest.h>

#include <atomic>
#include <numeric>
#include <vector>

using namespace ph;

TEST_CASE("jobs: parallel_for covers every index exactly once") {
    for (int workers : {0, 1, 3, 8}) {
        JobSystem jobs(workers);
        std::vector<std::atomic<int>> hits(100003);
        jobs.parallel_for(hits.size(), 100, [&](usize begin, usize end) {
            for (usize i = begin; i < end; ++i) hits[i].fetch_add(1);
        });
        bool all_once = true;
        for (auto& h : hits) all_once = all_once && h.load() == 1;
        CHECK(all_once);
    }
}

TEST_CASE("jobs: chunking is independent of the worker count") {
    CHECK(JobSystem::chunk_size(1000, 256) == 256);
    CHECK(JobSystem::chunk_size(1'000'000, 256) == 15625);
    CHECK(JobSystem::chunk_size(10, 256) == 256);
}

TEST_CASE("jobs: task groups wait for every job") {
    JobSystem jobs(3);
    TaskGroup group;
    std::atomic<int> sum{0};
    for (int i = 1; i <= 100; ++i) jobs.run(group, [&sum, i] { sum += i; });
    jobs.wait(group);
    CHECK(group.done());
    CHECK(sum == 5050);
}
