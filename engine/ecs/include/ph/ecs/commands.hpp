#pragma once

#include "ph/ecs/component.hpp"

#include <functional>
#include <memory>
#include <tuple>
#include <vector>

namespace ph::ecs {

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
    void clear();
    bool empty() const { return commands_.empty(); }
    usize size() const { return commands_.size(); }
    // Moves all of `other`'s commands to the end of this buffer.
    void append(CommandBuffer&& other);

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
};

} // namespace ph::ecs
