#pragma once

#include "bowl/core/types.hpp"

#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace bowl::net {

// One player's inputs for a run of ticks, as Lockstep and Rollback send them. Inputs are plain structs (trivially
// copyable, the same layout on every machine) with an ==.
template <class Input> struct InputMessage {
    static_assert(std::is_trivially_copyable_v<Input>, "inputs are sent as bytes: make them plain structs");
    static constexpr u32 magic = 0x54504E49; // "INPT"

    u32 player = 0;
    u32 first_tick = 0;
    std::vector<Input> inputs;

    std::vector<u8> encode() const {
        std::vector<u8> out(12 + inputs.size() * sizeof(Input));
        u32 head[3] = {magic, player, first_tick};
        std::memcpy(out.data(), head, sizeof head);
        if (!inputs.empty()) std::memcpy(out.data() + 12, inputs.data(), inputs.size() * sizeof(Input));
        return out;
    }
    static std::optional<InputMessage> decode(std::span<const u8> bytes) {
        if (bytes.size() < 12 || (bytes.size() - 12) % sizeof(Input) != 0) return std::nullopt;
        u32 head[3];
        std::memcpy(head, bytes.data(), sizeof head);
        if (head[0] != magic) return std::nullopt;
        InputMessage m{head[1], head[2], std::vector<Input>((bytes.size() - 12) / sizeof(Input))};
        if (!m.inputs.empty()) std::memcpy(m.inputs.data(), bytes.data() + 12, m.inputs.size() * sizeof(Input));
        return m;
    }
};

// Lockstep: every machine runs the same simulation from the same inputs, and a tick runs only once every player's
// input for it is in. Inputs are taken `delay` ticks ahead so they have time to arrive; with a slow player everyone
// waits. Send the messages reliably (Delivery::Reliable): each one goes once.
//
//     net::Lockstep<Input> lockstep(players, my_index);
//     // every frame
//     if (auto m = lockstep.local(read_input())) host.broadcast(*m);
//     while (auto e = host.receive()) if (e->type == net::Event::Type::Message) lockstep.receive(e->data);
//     if (lockstep.ready()) {
//         simulate(lockstep.inputs()); // one per player, in player order
//         lockstep.advance();
//     }
template <class Input> class Lockstep {
public:
    Lockstep(u32 players, u32 local, u32 delay = 2) : players_(players), local_(local), delay_(delay) {
        for (u32 t = 0; t < delay; ++t) { // the first ticks have no inputs to wait for
            Row& r = row(t);
            for (u32 p = 0; p < players; ++p) set(r, p, Input{});
        }
    }

    u32 tick() const { return tick_; }
    u32 delay() const { return delay_; }

    // This player's input for tick() + delay. Call every frame; the first call for a tick takes it and returns the
    // message to send everyone else, later ones return nothing.
    std::optional<std::vector<u8>> local(const Input& input) {
        u32 t = tick_ + delay_;
        Row& r = row(t);
        if (r.have[local_]) return std::nullopt;
        set(r, local_, input);
        return InputMessage<Input>{local_, t, {input}}.encode();
    }

    // Another player's message. False if it isn't one.
    bool receive(std::span<const u8> bytes) {
        auto m = InputMessage<Input>::decode(bytes);
        if (!m || m->player >= players_) return false;
        for (usize i = 0; i < m->inputs.size(); ++i) {
            u32 t = m->first_tick + static_cast<u32>(i);
            if (t < tick_) continue;
            Row& r = row(t);
            if (!r.have[m->player]) set(r, m->player, m->inputs[i]);
        }
        return true;
    }

    // Whether every input for tick() is in.
    bool ready() const {
        auto it = rows_.find(tick_);
        return it != rows_.end() && it->second.count == players_;
    }
    // The inputs for tick(), one per player. Only while ready().
    std::span<const Input> inputs() const { return rows_.at(tick_).values; }
    void advance() {
        rows_.erase(tick_);
        ++tick_;
    }
    // Players whose input for tick() hasn't come yet.
    std::vector<u32> waiting_for() const {
        std::vector<u32> out;
        auto it = rows_.find(tick_);
        for (u32 p = 0; p < players_; ++p)
            if (it == rows_.end() || !it->second.have[p]) out.push_back(p);
        return out;
    }

private:
    struct Row {
        std::vector<Input> values;
        std::vector<bool> have;
        u32 count = 0;
    };
    Row& row(u32 t) {
        auto [it, fresh] = rows_.try_emplace(t);
        if (fresh) it->second.values.resize(players_), it->second.have.assign(players_, false);
        return it->second;
    }
    void set(Row& r, u32 player, const Input& input) {
        r.values[player] = input;
        r.have[player] = true;
        ++r.count;
    }

    u32 players_, local_, delay_;
    u32 tick_ = 0;
    std::map<u32, Row> rows_;
};

} // namespace bowl::net
