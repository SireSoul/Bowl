#include "bowl/ecs/schedule.hpp"

#include "bowl/core/profiler.hpp"
#include "bowl/core/time.hpp"

#include <algorithm>

namespace bowl::ecs {

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

namespace {

std::string names_of(const ComponentMask& mask) {
    std::string out;
    mask.for_each([&](ComponentId c) {
        if (!out.empty()) out += ", ";
        out += component_info(c).name;
    });
    return out;
}

std::string common_names(const std::vector<TypeId>& a, const std::vector<std::string>& a_names,
                         const std::vector<TypeId>& b) {
    std::string out;
    for (usize i = 0; i < a.size(); ++i)
        if (std::find(b.begin(), b.end(), a[i]) != b.end()) {
            if (!out.empty()) out += ", ";
            out += i < a_names.size() ? a_names[i] : std::string("a resource");
        }
    return out;
}

} // namespace

std::string Schedule::conflict_reason(const System& a, const System& b) const {
    if (a.exclusive) return a.name + " is exclusive";
    if (b.exclusive) return b.name + " is exclusive";
    if (a.writes.intersects(b.writes)) return a.name + " and " + b.name + " both write " + names_of(a.writes & b.writes);
    if (a.writes.intersects(b.reads)) return a.name + " writes " + names_of(a.writes & b.reads) + ", which " + b.name + " reads";
    if (b.writes.intersects(a.reads)) return b.name + " writes " + names_of(b.writes & a.reads) + ", which " + a.name + " reads";
    if (auto both = common_names(a.resource_writes, a.resource_write_names, b.resource_writes); !both.empty())
        return a.name + " and " + b.name + " both write " + both;
    if (auto r = common_names(a.resource_writes, a.resource_write_names, b.resource_reads); !r.empty())
        return a.name + " writes " + r + ", which " + b.name + " reads";
    if (auto r = common_names(b.resource_writes, b.resource_write_names, a.resource_reads); !r.empty())
        return b.name + " writes " + r + ", which " + a.name + " reads";
    return {};
}

void Schedule::build_batches() {
    batches_.clear();
    std::vector<usize> current;
    bool after_barrier = false;
    for (usize i = 0; i < systems_.size(); ++i) {
        System& s = *systems_[i];
        s.why_new_batch.clear();
        if (after_barrier) s.why_new_batch = "barrier";
        after_barrier = false;
        auto clash = std::find_if(current.begin(), current.end(), [&](usize j) { return conflicts(s, *systems_[j]); });
        if (clash != current.end()) {
            s.why_new_batch = conflict_reason(*systems_[*clash], s);
            batches_.push_back(std::move(current));
            current.clear();
        }
        current.push_back(i);
        if (s.barrier_after) {
            after_barrier = true;
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

std::vector<SystemInfo> Schedule::describe() {
    const auto& all = batches();
    std::vector<SystemInfo> out(systems_.size());
    for (usize b = 0; b < all.size(); ++b)
        for (usize i : all[b]) {
            const System& s = *systems_[i];
            SystemInfo& info = out[i];
            info.name = s.name;
            info.batch = static_cast<u32>(b);
            info.ms = s.last_ms;
            info.enabled = s.enabled;
            info.exclusive = s.exclusive;
            s.reads.for_each([&](ComponentId c) { info.reads.emplace_back(component_info(c).name); });
            s.writes.for_each([&](ComponentId c) { info.writes.emplace_back(component_info(c).name); });
            info.resource_reads = s.resource_read_names;
            info.resource_writes = s.resource_write_names;
            info.why_new_batch = s.why_new_batch;
        }
    return out;
}

void Schedule::run_system(System& s, World& world, JobSystem& jobs, f32 dt) {
    if (!s.enabled) return;
    if (!s.commands) {
        s.commands = std::make_unique<CommandBuffer>(world);
        // Stand-in ids, tagged by system, so systems running side by side create the same entities every run.
        usize index = 0;
        while (index < systems_.size() && systems_[index].get() != &s) ++index;
        s.commands->use_placeholders(static_cast<u32>(index % 2047) + 1);
    }
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

} // namespace bowl::ecs
