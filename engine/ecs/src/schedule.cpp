#include "ph/ecs/schedule.hpp"

#include "ph/core/profiler.hpp"
#include "ph/core/time.hpp"

#include <algorithm>

namespace ph::ecs {

Schedule::Builder Schedule::add(std::string name, std::function<void(SystemContext&)> fn) {
    auto s = std::make_unique<System>();
    s->name = std::move(name);
    s->fn = std::move(fn);
    systems_.push_back(std::move(s));
    dirty_ = true;
    return Builder(*this, systems_.size() - 1);
}

void Schedule::barrier() {
    if (!systems_.empty()) systems_.back()->barrier_after = true;
    dirty_ = true;
}

void Schedule::set_enabled(std::string_view name, bool enabled) {
    for (auto& s : systems_)
        if (s->name == name) s->enabled = enabled;
}

static bool overlap(const std::vector<TypeId>& a, const std::vector<TypeId>& b) {
    for (TypeId x : a)
        if (std::find(b.begin(), b.end(), x) != b.end()) return true;
    return false;
}

bool Schedule::conflicts(const System& a, const System& b) const {
    if (a.exclusive || b.exclusive) return true;
    if (a.writes.intersects(b.writes) || a.writes.intersects(b.reads) || b.writes.intersects(a.reads)) return true;
    if (overlap(a.resource_writes, b.resource_writes) || overlap(a.resource_writes, b.resource_reads) ||
        overlap(b.resource_writes, a.resource_reads))
        return true;
    return false;
}

void Schedule::build_batches() {
    batches_.clear();
    std::vector<usize> current;
    for (usize i = 0; i < systems_.size(); ++i) {
        const System& s = *systems_[i];
        bool fits = std::none_of(current.begin(), current.end(), [&](usize j) { return conflicts(s, *systems_[j]); });
        if (!fits) {
            batches_.push_back(std::move(current));
            current.clear();
        }
        current.push_back(i);
        if (s.barrier_after) {
            batches_.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) batches_.push_back(std::move(current));
    dirty_ = false;
}

const std::vector<std::vector<usize>>& Schedule::batches() {
    if (dirty_) build_batches();
    return batches_;
}

void Schedule::run_system(System& s, World& world, JobSystem& jobs, f32 dt) {
    if (!s.enabled) return;
    if (!s.commands) s.commands = std::make_unique<CommandBuffer>(world);
    ProfileScope zone(s.name);
    Stopwatch watch;
    SystemContext ctx{world, *s.commands, jobs, dt};
    s.fn(ctx);
    s.last_ms = watch.milliseconds();
}

void Schedule::apply_commands(usize first, usize last) {
    for (usize i = first; i < last; ++i)
        if (systems_[i]->commands) systems_[i]->commands->apply();
}

void Schedule::run(World& world, JobSystem& jobs, f32 dt) {
    if (dirty_) build_batches();
    usize pending_from = 0; // systems whose commands haven't been applied yet start here
    for (const auto& batch : batches_) {
        if (!parallel_ || batch.size() == 1 || jobs.worker_count() == 0) {
            for (usize i : batch) run_system(*systems_[i], world, jobs, dt);
        } else {
            TaskGroup group;
            for (usize k = 1; k < batch.size(); ++k) {
                System* s = systems_[batch[k]].get();
                jobs.run(group, [this, s, &world, &jobs, dt] { run_system(*s, world, jobs, dt); });
            }
            run_system(*systems_[batch[0]], world, jobs, dt);
            jobs.wait(group);
        }
        usize end = batch.back() + 1;
        if (systems_[batch.back()]->barrier_after) {
            apply_commands(pending_from, end);
            world.flush();
            pending_from = end;
        }
    }
    apply_commands(pending_from, systems_.size());
    world.flush();
}

} // namespace ph::ecs
