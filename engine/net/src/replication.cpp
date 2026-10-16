#include "bowl/net/replication.hpp"

#include "wire.hpp"

#include <algorithm>

namespace bowl::net {

using wire::Reader;
using wire::Writer;

// Replication messages, little-endian:
//   reliable    u8 0xB2, u32 tick, u16 count, then ops: u8 op, u32 net id, and for Set and Remove u32 component key,
//               and for Set u32 length, bytes (the component, as binary JSON)
//   continuous  u8 0xB3, u32 tick, then entries: u32 net id, u32 component key, u16 length, bytes
// The host numbers its updates (tick); a player keeps the newest tick it applied for each entity's component and turns
// older ones away, so a late unreliable value never overwrites a newer one.
namespace {

enum Op : u8 { Spawn = 1, Despawn = 2, Set = 3, Remove = 4 };
constexpr usize max_reliable_batch = 60 * 1024;
constexpr usize continuous_header = 1 + 4;

u64 hash_bytes(std::span<const u8> bytes) {
    u64 h = 14695981039346656037ull;
    for (u8 b : bytes) h = (h ^ b) * 1099511628211ull;
    return h;
}

u64 slot(u32 id, u32 key) { return (static_cast<u64>(id) << 32) | key; }

} // namespace

namespace detail {

std::vector<u8> encode(const json::Value& value) { return json::encode_binary(value); }

std::optional<json::Value> decode(std::span<const u8> bytes) {
    auto parsed = json::decode_binary(bytes);
    if (!parsed) return std::nullopt;
    return std::move(parsed.value);
}

} // namespace detail

bool Messages::handle(const Event& event) {
    if (event.type != Event::Type::Message || event.data.size() < 5 || event.data[0] != tag) return false;
    Reader r{event.data, 1};
    u32 key = r.u32_();
    auto it = handlers_.find(key);
    if (it == handlers_.end()) return true; // ours, but nobody here listens for it
    if (auto body = detail::decode(std::span(event.data).subspan(5))) it->second(event.peer, *body);
    return true;
}

// The host's side.

struct Replicator::Impl {
    ecs::World& world;
    ReplicationSettings settings;
    std::vector<detail::ComponentKind> kinds;
    std::function<bool(PeerId, ecs::Entity)> relevant;
    u32 next_id = 1, tick = 1;
    std::unordered_map<u32, ecs::Entity> by_id;
    std::unordered_map<u64, u64> net_ids; // entity bits to net id: the entity map component saving uses

    struct Sent {
        bool present = false; // the player has the component
        u64 hash = 0;         // of what it was sent last
        u32 at = 0;           // the update it was sent in
    };
    struct Shown {
        std::vector<Sent> components; // by kind
    };
    struct PeerState {
        std::unordered_map<u32, Shown> shown; // the entities this player has, by net id
        usize cursor = 0;                     // where the Continuous values start next time
    };
    std::unordered_map<PeerId, PeerState> peers;

    Impl(ecs::World& w, ReplicationSettings s) : world(w), settings(s) {}

    struct Batch { // reliable ops for one player, cut into messages
        std::vector<std::vector<u8>> messages;
        Writer w;
        usize count_at = 0;
        u16 count = 0;
        u32 tick = 0;
        void begin() {
            w = {};
            w.u8_(Replica::reliable_tag);
            w.u32_(tick);
            count_at = w.out.size();
            w.u16_(0);
            count = 0;
        }
        void op(u8 kind, u32 id) {
            if (w.out.empty()) begin();
            if (w.out.size() > max_reliable_batch || count == 0xFFFF) {
                finish();
                begin();
            }
            w.u8_(kind);
            w.u32_(id);
            ++count;
        }
        void finish() {
            if (w.out.empty() || count == 0) return;
            w.out[count_at] = static_cast<u8>(count);
            w.out[count_at + 1] = static_cast<u8>(count >> 8);
            messages.push_back(std::move(w.out));
            w = {};
        }
    };
};

Replicator::Replicator(ecs::World& world, ReplicationSettings settings)
    : impl_(std::make_unique<Impl>(world, settings)) {}
Replicator::~Replicator() = default;

void Replicator::add_kind(detail::ComponentKind kind) {
    for (auto& k : impl_->kinds)
        if (k.key == kind.key) {
            k = std::move(kind);
            return;
        }
    impl_->kinds.push_back(std::move(kind));
    for (auto& [peer, state] : impl_->peers)
        for (auto& [id, shown] : state.shown) shown.components.resize(impl_->kinds.size());
}

void Replicator::relevant(std::function<bool(PeerId, ecs::Entity)> filter) { impl_->relevant = std::move(filter); }

u32 Replicator::net_id(ecs::Entity e) const {
    auto it = impl_->net_ids.find(e.bits());
    return it == impl_->net_ids.end() ? 0 : static_cast<u32>(it->second);
}

ecs::Entity Replicator::entity(u32 id) const {
    auto it = impl_->by_id.find(id);
    return it == impl_->by_id.end() ? ecs::Entity{} : it->second;
}

void Replicator::update(Host& host) {
    Impl& m = *impl_;
    // The replicated entities now: new ones get an id (as do copies of another, which arrive with its id).
    std::vector<std::pair<u32, ecs::Entity>> current;
    std::unordered_map<u32, ecs::Entity> now_by_id;
    m.world.each<Replicated>([&](ecs::Entity e, Replicated& r) {
        if (r.id == 0 || now_by_id.contains(r.id) || (m.by_id.contains(r.id) && m.by_id[r.id] != e)) r.id = m.next_id++;
        now_by_id[r.id] = e;
        current.emplace_back(r.id, e);
    });
    std::sort(current.begin(), current.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::vector<u32> gone;
    for (auto& [id, e] : m.by_id)
        if (!now_by_id.contains(id)) gone.push_back(id);
    m.by_id = std::move(now_by_id);
    m.net_ids.clear();
    for (auto& [id, e] : current) m.net_ids[e.bits()] = id;
    serial::Context ctx;
    ctx.entity_map = &m.net_ids;

    // Each component once per update, shared by every player that's sent it.
    struct Encoded {
        std::vector<u8> bytes;
        u64 hash = 0;
    };
    std::unordered_map<u64, std::optional<Encoded>> encoded;
    auto encode = [&](u32 id, ecs::Entity e, usize k) -> const std::optional<Encoded>& {
        auto [it, fresh] = encoded.try_emplace(slot(id, static_cast<u32>(k)));
        if (fresh && m.kinds[k].has(m.world, e)) {
            Encoded enc;
            enc.bytes = detail::encode(m.kinds[k].save(m.world, e, ctx));
            enc.hash = hash_bytes(enc.bytes);
            it->second = std::move(enc);
        }
        return it->second;
    };

    std::vector<PeerId> peers = host.peers();
    std::erase_if(m.peers, [&](auto& p) { return std::find(peers.begin(), peers.end(), p.first) == peers.end(); });
    for (PeerId peer : peers) {
        Impl::PeerState& state = m.peers[peer];
        Impl::Batch batch;
        batch.tick = m.tick;
        for (u32 id : gone)
            if (state.shown.erase(id)) batch.op(Despawn, id);
        // Spawns first, so the components after them can name any entity this update shows.
        std::vector<std::pair<u32, ecs::Entity>> shown;
        for (auto& [id, e] : current) {
            if (m.relevant && !m.relevant(peer, e)) {
                if (state.shown.erase(id)) batch.op(Despawn, id);
                continue;
            }
            auto [it, fresh] = state.shown.try_emplace(id);
            if (fresh) {
                it->second.components.resize(m.kinds.size());
                batch.op(Spawn, id);
            }
            shown.emplace_back(id, e);
        }
        // Components: OnChange ones (and every one an entity hasn't had yet) reliably when they change; Continuous
        // ones unreliably, newest wins.
        struct Pending {
            u32 id;
            usize kind;
        };
        std::vector<Pending> continuous;
        for (auto& [id, e] : shown) {
            Impl::Shown& s = state.shown[id];
            for (usize k = 0; k < m.kinds.size(); ++k) {
                Impl::Sent& sent = s.components[k];
                const auto& enc = encode(id, e, k);
                if (!enc) {
                    if (sent.present) {
                        batch.op(Remove, id);
                        batch.w.u32_(m.kinds[k].key);
                        sent = {};
                    }
                    continue;
                }
                bool reliable = m.kinds[k].sync == Sync::OnChange || !sent.present ||
                                continuous_header + 10 + enc->bytes.size() > Host::max_unreliable;
                if (reliable) {
                    if (sent.present && sent.hash == enc->hash) continue;
                    batch.op(Set, id);
                    batch.w.u32_(m.kinds[k].key);
                    batch.w.u32_(static_cast<u32>(enc->bytes.size()));
                    batch.w.bytes(enc->bytes);
                    sent = {true, enc->hash, m.tick};
                } else if (sent.hash != enc->hash || m.tick - sent.at >= m.settings.resend_every) {
                    continuous.push_back({id, k});
                }
            }
        }
        batch.finish();
        for (auto& message : batch.messages) host.send(peer, message, Delivery::Reliable);

        // Continuous values, packed into messages, as many as the budget allows, starting where it stopped last time.
        if (continuous.empty()) continue;
        usize start = state.cursor % continuous.size(), sent_count = 0;
        Writer w;
        u32 messages = 0;
        auto flush = [&] {
            if (w.out.size() > continuous_header) {
                host.send(peer, w.out, Delivery::Unreliable);
                ++messages;
            }
            w = {};
        };
        for (usize i = 0; i < continuous.size() && messages < m.settings.continuous_messages; ++i) {
            const Pending& p = continuous[(start + i) % continuous.size()];
            const auto& enc = *encode(p.id, m.by_id[p.id], p.kind);
            usize size = 4 + 4 + 2 + enc.bytes.size();
            if (!w.out.empty() && w.out.size() + size > Host::max_unreliable) {
                flush();
                if (messages >= m.settings.continuous_messages) break;
            }
            if (w.out.empty()) {
                w.u8_(Replica::continuous_tag);
                w.u32_(m.tick);
            }
            w.u32_(p.id);
            w.u32_(m.kinds[p.kind].key);
            w.u16_(static_cast<u16>(enc.bytes.size()));
            w.bytes(enc.bytes);
            state.shown[p.id].components[p.kind] = {true, enc.hash, m.tick};
            ++sent_count;
        }
        if (messages < m.settings.continuous_messages) flush();
        state.cursor = start + sent_count;
    }
    ++m.tick;
}

// A player's side.

struct Replica::Impl {
    ecs::World& world;
    std::vector<detail::ComponentKind> kinds;
    std::unordered_map<u32, usize> by_key;
    std::unordered_map<u32, ecs::Entity> by_id;
    std::unordered_map<u64, u64> entities; // net id to entity bits: the entity map component loading uses
    std::unordered_map<u64, u32> newest;   // (net id, component key) to the newest tick applied

    explicit Impl(ecs::World& w) : world(w) {}

    void set(u32 id, u32 key, std::span<const u8> bytes, u32 tick) {
        auto kind = by_key.find(key);
        auto e = by_id.find(id);
        if (kind == by_key.end() || e == by_id.end() || !world.alive(e->second)) return;
        u32& newest_tick = newest[slot(id, key)];
        if (tick < newest_tick) return; // something newer already arrived
        auto value = detail::decode(bytes);
        if (!value) return;
        newest_tick = tick;
        serial::Context ctx;
        ctx.entity_map = &entities;
        kinds[kind->second].load(world, e->second, *value, ctx);
    }
    void forget(u32 id) {
        for (auto& kind : kinds) newest.erase(slot(id, kind.key));
        if (auto it = by_id.find(id); it != by_id.end()) {
            if (world.alive(it->second)) world.destroy(it->second);
            by_id.erase(it);
        }
        entities.erase(id);
    }
};

Replica::Replica(ecs::World& world) : impl_(std::make_unique<Impl>(world)) {}
Replica::~Replica() = default;

void Replica::add_kind(detail::ComponentKind kind) {
    if (auto it = impl_->by_key.find(kind.key); it != impl_->by_key.end()) {
        impl_->kinds[it->second] = std::move(kind);
        return;
    }
    impl_->by_key[kind.key] = impl_->kinds.size();
    impl_->kinds.push_back(std::move(kind));
}

bool Replica::handle(const Event& event) {
    if (event.type != Event::Type::Message || event.data.empty()) return false;
    Impl& m = *impl_;
    std::span<const u8> data(event.data);
    if (data[0] == reliable_tag) {
        Reader r{data, 1};
        u32 tick = r.u32_();
        u16 count = r.u16_();
        for (u16 i = 0; i < count && r.ok; ++i) {
            u8 op = r.u8_();
            u32 id = r.u32_();
            if (!r.ok) break;
            if (op == Spawn) {
                if (m.by_id.contains(id)) continue;
                ecs::Entity e = m.world.create();
                m.world.add<Replicated>(e).id = id;
                m.by_id[id] = e;
                m.entities[id] = e.bits();
            } else if (op == Despawn) {
                m.forget(id);
            } else if (op == Set) {
                u32 key = r.u32_(), length = r.u32_();
                auto bytes = r.bytes(length);
                if (r.ok) m.set(id, key, bytes, tick);
            } else if (op == Remove) {
                u32 key = r.u32_();
                auto kind = m.by_key.find(key);
                auto e = m.by_id.find(id);
                if (!r.ok || kind == m.by_key.end() || e == m.by_id.end()) continue;
                m.kinds[kind->second].remove(m.world, e->second);
                m.newest.erase(slot(id, key));
            } else {
                break; // something this version doesn't know: stop rather than misread the rest
            }
        }
        return true;
    }
    if (data[0] == continuous_tag) {
        Reader r{data, 1};
        u32 tick = r.u32_();
        while (r.ok && !r.done()) {
            u32 id = r.u32_(), key = r.u32_();
            u16 length = r.u16_();
            auto bytes = r.bytes(length);
            if (r.ok) m.set(id, key, bytes, tick);
        }
        return true;
    }
    return false;
}

void Replica::clear() {
    std::vector<u32> ids;
    for (auto& [id, e] : impl_->by_id) ids.push_back(id);
    for (u32 id : ids) impl_->forget(id);
}

ecs::Entity Replica::entity(u32 id) const {
    auto it = impl_->by_id.find(id);
    return it == impl_->by_id.end() ? ecs::Entity{} : it->second;
}

usize Replica::size() const { return impl_->by_id.size(); }

} // namespace bowl::net
