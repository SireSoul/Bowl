#pragma once

#include "bowl/core/reflect.hpp"
#include "bowl/core/serialize.hpp"
#include "bowl/ecs/component.hpp"
#include "bowl/ecs/world.hpp"
#include "bowl/net/host.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <vector>

// The pieces of a game where one machine runs the world (the host, as in Stardew Valley's co-op) and the others are
// shown it: typed messages for what players do, and replication for what the world is.
//
//     // both sides, the same registrations
//     net::Messages messages;
//     messages.on<Gift>([&](net::PeerId from, const Gift& g) { ... });  // the host applies it, as that player
//     replicator.replicate<ecs::Transform2D>(net::Sync::Continuous);   // (or replica.replicate on players)
//     replicator.replicate<Health>();
//
//     // the host, every tick after the simulation
//     replicator.update(host);
//     // a player
//     messages.send(host, server_peer, Gift{.item = 3, .to = "calla"});
//     while (auto e = host.receive()) if (!messages.handle(*e) && !replica.handle(*e)) { ... }
namespace bowl::net {

namespace detail {
constexpr u32 fnv1a(std::string_view s) {
    u32 h = 2166136261u;
    for (char c : s) h = (h ^ static_cast<u8>(c)) * 16777619u;
    return h;
}
// The same on every compiler: the name as written in BOWL_REFLECT or BOWL_COMPONENT.
template <reflect::Reflected T> u32 type_key() { return fnv1a(reflect::info<T>().name); }
std::vector<u8> encode(const json::Value& value);
std::optional<json::Value> decode(std::span<const u8> bytes);
} // namespace detail

// Plain structs as messages. Each type is reflected (BOWL_REFLECT) and registered on both ends with on<T>(); it goes as
// its fields (serialization, as in saves: strings, vectors, nested structs and so on all work), named by its reflected
// name, so a Windows host and a Linux player agree. Messages starts every message it sends with one byte (`tag`), so
// a game can send its own bytes alongside as long as they don't start with it.
class Messages {
public:
    static constexpr u8 tag = 0xB1;

    template <reflect::Reflected T> void on(std::function<void(PeerId, const T&)> handler) {
        handlers_[detail::type_key<T>()] = [call = std::move(handler)](PeerId from, const json::Value& j) {
            T message{};
            serial::Context ctx;
            if (serial::from_json(j, message, ctx)) call(from, message);
        };
    }
    template <reflect::Reflected T> bool send(Host& host, PeerId peer, const T& message, Delivery delivery = Delivery::Reliable) {
        return host.send(peer, encode(message), delivery);
    }
    template <reflect::Reflected T> void broadcast(Host& host, const T& message, Delivery delivery = Delivery::Reliable) {
        host.broadcast(encode(message), delivery);
    }
    // Every Message event goes through here: true if it was one of these (its handler, if any, has run).
    bool handle(const Event& event);

private:
    template <reflect::Reflected T> std::vector<u8> encode(const T& message) {
        serial::Context ctx;
        std::vector<u8> out{tag};
        u32 key = detail::type_key<T>();
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(key >> (8 * i)));
        auto body = detail::encode(serial::to_json(message, ctx));
        out.insert(out.end(), body.begin(), body.end());
        return out;
    }
    std::unordered_map<u32, std::function<void(PeerId, const json::Value&)>> handlers_;
};

// Marks an entity the host shares with players. The Replicator gives it its network id; on players, the Replica's
// entities have it too, with the same id.
struct Replicated {
    u32 id = 0;
};
BOWL_COMPONENT(Replicated, id)

// How a replicated component travels. OnChange: reliably, when its value changes (health, a bag, a name: anything
// that must arrive). Continuous: unreliably and often, newest wins (positions and anything else that moves).
enum class Sync : u8 { OnChange, Continuous };

namespace detail {
struct ComponentKind {
    u32 key = 0;
    Sync sync = Sync::OnChange;
    std::function<bool(const ecs::World&, ecs::Entity)> has;
    std::function<json::Value(const ecs::World&, ecs::Entity, serial::Context&)> save;
    std::function<void(ecs::World&, ecs::Entity, const json::Value&, serial::Context&)> load; // adds it if missing
    std::function<void(ecs::World&, ecs::Entity)> remove;
};
template <reflect::Reflected C> ComponentKind component_kind(Sync sync) {
    ComponentKind k;
    k.key = type_key<C>();
    k.sync = sync;
    k.has = [](const ecs::World& w, ecs::Entity e) { return w.has<C>(e); };
    k.save = [](const ecs::World& w, ecs::Entity e, serial::Context& ctx) { return serial::to_json(w.get<C>(e), ctx); };
    k.load = [](ecs::World& w, ecs::Entity e, const json::Value& j, serial::Context& ctx) {
        C* c = w.try_get<C>(e);
        if (!c) c = &w.add<C>(e);
        serial::from_json(j, *c, ctx);
    };
    k.remove = [](ecs::World& w, ecs::Entity e) {
        if (w.has<C>(e)) w.remove<C>(e);
    };
    return k;
}
} // namespace detail

struct ReplicationSettings {
    u32 continuous_messages = 12; // unreliable messages (up to 1 KB each) per player per update; the rest wait a turn
    u32 resend_every = 10;        // updates after which an unchanged Continuous value goes again (it may have been lost)
};

// The host's side: shares every entity with a Replicated component, and the components registered with replicate(),
// with each connected peer. Entity fields inside components travel too, remapped on the other end, when the entity
// they point at is replicated (others arrive as nothing). Call update() once a tick, after the simulation.
class Replicator : NonCopyable {
public:
    explicit Replicator(ecs::World& world, ReplicationSettings settings = {});
    ~Replicator();

    template <reflect::Reflected C> void replicate(Sync sync = Sync::OnChange) { add_kind(detail::component_kind<C>(sync)); }
    // Which entities a peer is shown (all, without one): for example, the ones on the map that player is on. An entity
    // that stops being relevant is taken away from that player, and sent again in full if it becomes relevant again.
    void relevant(std::function<bool(PeerId, ecs::Entity)> filter);
    void update(Host& host);

    u32 net_id(ecs::Entity e) const;          // 0 if it isn't replicated (yet)
    ecs::Entity entity(u32 net_id) const;     // the null entity if none

private:
    void add_kind(detail::ComponentKind kind);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A player's side: builds, updates and removes the host's replicated entities in this world, from the Replicator's
// messages, with the same component registrations as the host. Entities it makes have Replicated with the host's id.
class Replica : NonCopyable {
public:
    static constexpr u8 reliable_tag = 0xB2, continuous_tag = 0xB3;

    explicit Replica(ecs::World& world);
    ~Replica();

    template <reflect::Reflected C> void replicate(Sync sync = Sync::OnChange) { add_kind(detail::component_kind<C>(sync)); }
    // Every Message event goes through here: true if it was replication (and has been applied).
    bool handle(const Event& event);
    // Removes every entity it made (after leaving a game).
    void clear();

    ecs::Entity entity(u32 net_id) const; // the null entity if none
    usize size() const;

private:
    void add_kind(detail::ComponentKind kind);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace bowl::net

