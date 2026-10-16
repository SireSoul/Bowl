#include "bowl/ecs/world.hpp"

#include <algorithm>
#include <cstring>

namespace bowl::ecs {

// World

World::World() : deferred_(*this) { empty_ = &archetype_for(ComponentMask{}); }

World::~World() {
    deferred_.clear();
    // Run remove hooks so resources tied to components (GPU handles, sounds) are released.
    if (!hooks_.empty()) {
        std::vector<Entity> all;
        for (auto& a : archetypes_) all.insert(all.end(), a->entities().begin(), a->entities().end());
        for (Entity e : all) destroy(e);
    }
    archetypes_.clear();
}

Entity World::reserve_entity() {
    i64 n = free_cursor_.fetch_sub(1, std::memory_order_relaxed);
    if (n > 0) {
        u32 index = free_[static_cast<usize>(n - 1)];
        return {index, records_[index].generation};
    }
    // Past the end of the free list: a brand-new slot, created by materialize_reserved().
    return {static_cast<u32>(records_.size() + static_cast<usize>(-n)), 1};
}

void World::materialize_reserved() {
    i64 cursor = free_cursor_.load(std::memory_order_relaxed);
    if (cursor < 0) {
        records_.resize(records_.size() + static_cast<usize>(-cursor));
        free_.clear();
    } else {
        free_.resize(static_cast<usize>(cursor));
    }
    free_cursor_.store(static_cast<i64>(free_.size()), std::memory_order_relaxed);
}

void World::spawn_reserved(Entity e) {
    Record& r = records_[e.index];
    BOWL_ASSERT(!r.alive && r.generation == e.generation, "spawning an entity id that wasn't reserved");
    r.alive = true;
    r.archetype = empty_;
    r.row = empty_->push(e);
    ++entity_count_;
}

Entity World::create() {
    check_unlocked();
    Entity e = reserve_entity();
    materialize_reserved();
    spawn_reserved(e);
    return e;
}

void World::destroy(Entity e) {
    check_unlocked();
    if (!alive(e)) return;
    materialize_reserved();
    {
        // Copy: hooks may not change structure, but keep iteration independent of the archetype anyway.
        Archetype* a = records_[e.index].archetype;
        std::vector<ComponentId> comps(a->components().begin(), a->components().end());
        for (ComponentId c : comps) run_remove_hooks(c, e);
    }
    Record& r = records_[e.index];
    Entity moved = r.archetype->remove(r.row);
    if (moved) records_[moved.index].row = r.row;
    r.alive = false;
    r.archetype = nullptr;
    // Never 0 (null) and never the stand-in generation.
    u32 next = r.generation + 1;
    r.generation = next == 0 || next == Entity::placeholder_generation ? 1 : next;
    free_.push_back(e.index);
    free_cursor_.store(static_cast<i64>(free_.size()), std::memory_order_relaxed);
    --entity_count_;
}

std::span<const ComponentId> World::components_of(Entity e) const {
    if (!alive(e)) return {};
    return records_[e.index].archetype->components();
}

void* World::component_ptr(Entity e, ComponentId c) const {
    if (!alive(e)) return nullptr;
    const Record& r = records_[e.index];
    return r.archetype->at(c, r.row);
}

bool World::add_component(Entity e, ComponentId c, FunctionRef<void(void*)> init) {
    check_unlocked();
    if (!alive(e)) return false;
    const ComponentInfo& info = component_info(c);
    if (!info.construct_default) return false;
    Record& r = records_[e.index];
    bool existed = r.archetype->has(c);
    if (existed) {
        void* p = r.archetype->at(c, r.row);
        if (p && info.destroy) info.destroy(p);
        if (p) info.construct_default(p);
        init(p);
        return true;
    }
    u32 row = move_entity(e, archetype_with(*r.archetype, c));
    void* p = records_[e.index].archetype->at(c, row);
    if (p) info.construct_default(p);
    init(p);
    run_add_hooks(c, e);
    return true;
}

bool World::remove_component(Entity e, ComponentId c) {
    check_unlocked();
    if (!alive(e) || !records_[e.index].archetype->has(c)) return false;
    run_remove_hooks(c, e);
    move_entity(e, archetype_without(*records_[e.index].archetype, c));
    return true;
}

Archetype& World::archetype_for(const ComponentMask& mask) {
    if (auto it = by_mask_.find(mask); it != by_mask_.end()) return *it->second;
    std::vector<ComponentId> comps;
    mask.for_each([&](ComponentId c) { comps.push_back(c); });
    archetypes_.push_back(std::make_unique<Archetype>(next_archetype_id_++, std::move(comps)));
    Archetype* a = archetypes_.back().get();
    by_mask_.emplace(mask, a);
    return *a;
}

usize World::compact() {
    check_unlocked();
    auto unused = [&](const std::unique_ptr<Archetype>& a) { return a->empty() && a.get() != empty_; };
    usize freed = static_cast<usize>(std::count_if(archetypes_.begin(), archetypes_.end(), unused));
    if (freed == 0) return 0;
    // Nothing may point at what goes: the mask lookup, and the add/remove shortcuts between archetypes.
    for (const auto& a : archetypes_) {
        if (unused(a)) {
            by_mask_.erase(a->mask());
            continue;
        }
        auto gone = [&](const auto& kv) { return kv.second->empty() && kv.second != empty_; };
        std::erase_if(a->add_edges, gone);
        std::erase_if(a->remove_edges, gone);
    }
    std::erase_if(archetypes_, unused);
    ++layout_epoch_;
    return freed;
}

Archetype& World::archetype_with(Archetype& from, ComponentId added) {
    if (auto it = from.add_edges.find(added); it != from.add_edges.end()) return *it->second;
    ComponentMask m = from.mask();
    m.set(added);
    Archetype& to = archetype_for(m);
    from.add_edges.emplace(added, &to);
    to.remove_edges.emplace(added, &from);
    return to;
}

Archetype& World::archetype_without(Archetype& from, ComponentId removed) {
    if (auto it = from.remove_edges.find(removed); it != from.remove_edges.end()) return *it->second;
    ComponentMask m = from.mask();
    m.reset(removed);
    Archetype& to = archetype_for(m);
    from.remove_edges.emplace(removed, &to);
    to.add_edges.emplace(removed, &from);
    return to;
}

u32 World::move_entity(Entity e, Archetype& to) {
    Record& r = records_[e.index];
    if (r.archetype == &to) return r.row;
    Entity moved;
    u32 row = r.archetype->move_to(r.row, to, moved);
    if (moved) records_[moved.index].row = r.row;
    r.archetype = &to;
    r.row = row;
    return row;
}

World::Hooks& World::hooks_for(ComponentId c) {
    if (hooks_.size() <= c) hooks_.resize(c + 1u);
    if (!hooks_[c]) hooks_[c] = std::make_unique<Hooks>();
    return *hooks_[c];
}

void World::run_add_hooks(ComponentId c, Entity e) {
    const Hooks* h = hooks_of(c);
    if (!h || h->on_add.empty()) return;
    LockGuard lock(*this);
    for (auto& hook : h->on_add) hook(*this, e);
}

void World::run_remove_hooks(ComponentId c, Entity e) {
    const Hooks* h = hooks_of(c);
    if (!h || h->on_remove.empty()) return;
    LockGuard lock(*this);
    for (auto& hook : h->on_remove) hook(*this, e);
}

void World::reset() {
    check_unlocked();
    deferred_.clear();
    hooks_.clear();
    resources_.clear();
    archetypes_.clear(); // destroys every component
    ++layout_epoch_; // queries made before must not keep pointers into the old archetypes
    by_mask_.clear();
    records_.clear();
    free_.clear();
    free_cursor_.store(0, std::memory_order_relaxed);
    entity_count_ = 0;
    empty_ = &archetype_for(ComponentMask{});
}

void World::forget_code() {
    check_unlocked();
    deferred_.clear();
    hooks_.clear();
    resources_.clear();
}

void World::flush() {
    // Applying can record more commands (hooks, run() callbacks); keep going until quiet.
    for (int pass = 0; pass < 64 && !deferred_.empty(); ++pass) deferred_.apply();
    BOWL_ASSERT(deferred_.empty(), "deferred commands keep scheduling more commands");
}

// Stand-in ids

namespace {
// Swaps stand-in ids inside a component's reflected fields for the real entities.
class PlaceholderFixer final : public reflect::FieldVisitor {
public:
    explicit PlaceholderFixer(World& world) : world_(world) {}
    void boolean(std::string_view, bool&) override {}
    void integer(std::string_view, i64&, i64, i64) override {}
    void real(std::string_view, f64&) override {}
    void text(std::string_view, std::string&) override {}
    void vector(std::string_view, f32*, int) override {}
    void color(std::string_view, Color&) override {}
    void entity(std::string_view, u64) override {}
    void entity_ref(std::string_view, u64& bits) override {
        Entity e = Entity::from_bits(bits);
        if (e.is_placeholder()) bits = world_.resolve_placeholder(e).bits();
    }
    bool begin_group(std::string_view) override { return true; }
    void end_group() override {}
    bool begin_list(std::string_view, usize&, bool) override { return true; }
    void unsupported(std::string_view, std::string_view) override {}

private:
    World& world_;
};
} // namespace

Entity World::resolve(Entity e) {
    if (!e.is_placeholder()) return e;
    if (!placeholders_) return null_entity; // a stand-in kept past its buffer: it means nothing now
    auto [it, made] = placeholders_->try_emplace(e.index);
    if (made) it->second = create();
    return it->second;
}

Entity World::resolve_placeholder(Entity e) { return resolve(e); }

reflect::FieldVisitor& World::placeholder_fixer() {
    if (!placeholder_fixer_) placeholder_fixer_ = std::make_unique<PlaceholderFixer>(*this);
    return *placeholder_fixer_;
}

// CommandBuffer

CommandBuffer::CommandBuffer(CommandBuffer&& o) noexcept
    : world_(o.world_), commands_(std::move(o.commands_)), blocks_(std::move(o.blocks_)), block_used_(o.block_used_),
      placeholder_tag_(o.placeholder_tag_), next_placeholder_(o.next_placeholder_), has_placeholders_(o.has_placeholders_) {
    o.commands_.clear();
    o.blocks_.clear();
    o.block_used_ = 0;
    o.next_placeholder_ = 0;
    o.has_placeholders_ = false;
}

CommandBuffer& CommandBuffer::operator=(CommandBuffer&& o) noexcept {
    if (this != &o) {
        clear();
        world_ = o.world_;
        commands_ = std::move(o.commands_);
        blocks_ = std::move(o.blocks_);
        block_used_ = o.block_used_;
        placeholder_tag_ = o.placeholder_tag_;
        next_placeholder_ = o.next_placeholder_;
        has_placeholders_ = o.has_placeholders_;
        o.commands_.clear();
        o.blocks_.clear();
        o.block_used_ = 0;
        o.next_placeholder_ = 0;
        o.has_placeholders_ = false;
    }
    return *this;
}

void* CommandBuffer::allocate(usize size, usize align) {
    constexpr usize block_size = 16 * 1024;
    auto fits = [&](const Block& b) {
        auto base = reinterpret_cast<uintptr_t>(b.data.get());
        uintptr_t at = (base + block_used_ + align - 1) & ~(uintptr_t(align) - 1);
        return at + size <= base + b.size;
    };
    if (blocks_.empty() || !fits(blocks_.back())) {
        usize bytes = std::max(block_size, size + align);
        blocks_.push_back({std::make_unique<std::byte[]>(bytes), bytes});
        block_used_ = 0;
    }
    Block& b = blocks_.back();
    auto base = reinterpret_cast<uintptr_t>(b.data.get());
    uintptr_t at = (base + block_used_ + align - 1) & ~(uintptr_t(align) - 1);
    block_used_ = static_cast<usize>(at + size - base);
    return reinterpret_cast<void*>(at);
}

Entity CommandBuffer::create() {
    if (placeholder_tag_) {
        BOWL_ASSERT(placeholder_tag_ < 4096 && next_placeholder_ < (1u << 20), "too many stand-in entities in one buffer");
        Entity e{(placeholder_tag_ << 20) | next_placeholder_++, Entity::placeholder_generation};
        has_placeholders_ = true;
        push<SpawnCommand>(e);
        return e;
    }
    Entity e = world_->reserve_entity();
    push<SpawnCommand>(e);
    return e;
}

void CommandBuffer::destroy(Entity e) { push<DestroyCommand>(e); }

void CommandBuffer::SpawnCommand::apply(World& w) {
    if (e.is_placeholder()) w.resolve(e); // made now, in command order
    else w.spawn_reserved(e);
}
void CommandBuffer::DestroyCommand::apply(World& w) { w.destroy(w.resolve(e)); }

void CommandBuffer::apply() {
    if (commands_.empty()) return;
    world_->materialize_reserved();
    // Detach storage first: applying may record new commands into this same buffer.
    std::vector<Command> commands = std::move(commands_);
    std::vector<Block> blocks = std::move(blocks_);
    commands_.clear();
    blocks_.clear();
    block_used_ = 0;
    // Stand-ins become real entities as the commands reach them.
    std::unordered_map<u32, Entity> placeholders;
    auto* outer = world_->placeholders_;
    if (has_placeholders_) world_->placeholders_ = &placeholders;
    has_placeholders_ = false;
    next_placeholder_ = 0;
    for (Command& c : commands) {
        c.apply(*world_, c.payload);
        c.destroy(c.payload);
    }
    world_->placeholders_ = outer;
    // Keep one block for reuse.
    if (blocks_.empty() && !blocks.empty()) {
        auto biggest = std::max_element(blocks.begin(), blocks.end(), [](const Block& a, const Block& b) { return a.size < b.size; });
        blocks_.push_back(std::move(*biggest));
    }
    if (commands_.capacity() < commands.capacity()) {
        commands.clear();
        commands.insert(commands.end(), commands_.begin(), commands_.end());
        commands_ = std::move(commands);
    }
}

void CommandBuffer::clear() {
    for (Command& c : commands_) c.destroy(c.payload);
    commands_.clear();
    if (blocks_.size() > 1) blocks_.erase(blocks_.begin(), blocks_.end() - 1);
    block_used_ = 0;
    next_placeholder_ = 0;
    has_placeholders_ = false;
}

void CommandBuffer::append(CommandBuffer&& other) {
    if (&other == this || other.commands_.empty()) return;
    has_placeholders_ = has_placeholders_ || other.has_placeholders_;
    other.has_placeholders_ = false;
    other.next_placeholder_ = 0;
    commands_.insert(commands_.end(), other.commands_.begin(), other.commands_.end());
    if (blocks_.empty()) {
        blocks_ = std::move(other.blocks_);
        block_used_ = other.block_used_;
    } else {
        // Our last block stays last, so allocation continues where it was.
        blocks_.insert(blocks_.end() - 1, std::make_move_iterator(other.blocks_.begin()), std::make_move_iterator(other.blocks_.end()));
    }
    other.commands_.clear();
    other.blocks_.clear();
    other.block_used_ = 0;
}

} // namespace bowl::ecs
