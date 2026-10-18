#pragma once

#include "bowl/ecs/component.hpp"

#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace bowl::ecs {

class World;

// Records structural changes (create, destroy, add, remove) to apply later, in recording order. Systems use these
// while iterating, because changing an archetype mid-iteration would invalidate it. Payloads are placed in a bump
// arena, so recording is allocation-free in the steady state.
class CommandBuffer {
public:
    explicit CommandBuffer(World& world) : world_(&world) {}
    ~CommandBuffer() { clear(); }
    CommandBuffer(CommandBuffer&&) noexcept;
    CommandBuffer& operator=(CommandBuffer&&) noexcept;
    CommandBuffer(const CommandBuffer&) = delete;
    CommandBuffer& operator=(const CommandBuffer&) = delete;

    // Reserves an entity id now (thread-safe); the entity becomes alive when the buffer is applied.
    //
    // A system's buffer (and each chunk's in a par_each) hands out a stand-in id instead (Entity::is_placeholder),
    // because systems running side by side would otherwise race for ids and get different ones each run. Stand-ins
    // become real entities when the commands are applied, in system and chunk order, so a simulation creates the same
    // ids every time. Use a stand-in in this buffer's commands and in entity fields of reflected components (both are
    // swapped for the real id); don't keep it beyond that.
    Entity create();
    template <class... Ts>
        requires(Component<std::decay_t<Ts>> && ...)
    Entity create(Ts&&... components) {
        Entity e = create();
        if constexpr (sizeof...(Ts) > 0) insert(e, std::forward<Ts>(components)...);
        return e;
    }
    void destroy(Entity e);
    template <Component T, class... Args> void add(Entity e, Args&&... args) {
        push<AddCommand<T>>(e, T{std::forward<Args>(args)...});
    }
    template <class... Ts>
        requires(Component<std::decay_t<Ts>> && ...)
    void insert(Entity e, Ts&&... components) {
        push<InsertCommand<std::decay_t<Ts>...>>(e, std::forward<Ts>(components)...);
    }
    template <Component T> void remove(Entity e) { push<RemoveCommand<T>>(e); }
    // Arbitrary deferred work, run in order with the other commands.
    void run(std::function<void(World&)> fn) { push<RunCommand>(std::move(fn)); }

    // Applies every command to the world in recording order, then clears. Commands targeting entities that died in
    // the meantime are skipped.
    void apply();
    // Drops the commands unapplied. Entity ids create() reserved go back to the world (their handles stay dead), so a
    // buffer thrown away doesn't use up slots for good. (The destructor does the same: a buffer mustn't outlive its
    // world.)
    void clear();
    bool empty() const { return commands_.empty(); }
    usize size() const { return commands_.size(); }
    // Moves all of `other`'s commands to the end of this buffer.
    void append(CommandBuffer&& other);
    // Makes create() hand out stand-in ids marked with `tag` (1 to 4095, unique among the buffers applied together).
    // Schedules do this for their systems.
    void use_placeholders(u32 tag) { placeholder_tag_ = tag; }

private:
    struct Command {
        void (*apply)(World&, void*);
        void (*destroy)(void*);
        void* payload;
    };
    struct Block {
        std::unique_ptr<std::byte[]> data;
        usize size;
    };

    template <class C> struct Ops {
        static void apply(World& w, void* p) { static_cast<C*>(p)->apply(w); }
        static void destroy(void* p) { static_cast<C*>(p)->~C(); }
    };
    template <class C, class... Args> void push(Args&&... args) {
        void* mem = allocate(sizeof(C), alignof(C));
        C* c = ::new (mem) C{std::forward<Args>(args)...};
        commands_.push_back({&Ops<C>::apply, &Ops<C>::destroy, c});
    }
    void* allocate(usize size, usize align);

    struct SpawnCommand {
        Entity e;
        void apply(World& w);
    };
    struct DestroyCommand {
        Entity e;
        void apply(World& w);
    };
    template <class T> struct AddCommand {
        Entity e;
        T value;
        void apply(World& w);
    };
    template <class... Ts> struct InsertCommand {
        Entity e;
        std::tuple<Ts...> values;
        InsertCommand(Entity e_, auto&&... vs) : e(e_), values(std::forward<decltype(vs)>(vs)...) {}
        void apply(World& w);
    };
    template <class T> struct RemoveCommand {
        Entity e;
        void apply(World& w);
    };
    struct RunCommand {
        std::function<void(World&)> fn;
        void apply(World& w) { fn(w); }
    };

    World* world_;
    std::vector<Command> commands_;
    std::vector<Block> blocks_;
    usize block_used_ = 0; // bytes used in blocks_.back()
    u32 placeholder_tag_ = 0;       // 0: real reserved ids
    u32 next_placeholder_ = 0;      // stand-ins handed out since the last apply
    bool has_placeholders_ = false; // its commands (its own or appended) mention stand-ins
    std::vector<Entity> reserved_;  // real ids create() reserved that no apply has spawned yet
    u32 reserved_in_ = 0;           // the world's reset count when they were reserved
    void release_reserved();
};

} // namespace bowl::ecs
