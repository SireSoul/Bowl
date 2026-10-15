#pragma once

#include "ph/net/lockstep.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace ph::net {

// Rollback: nobody waits. Each machine runs ahead with the inputs it has, guessing the missing ones (a player keeps
// doing what they did last); when a real input turns out different from the guess, it goes back to the saved state of
// that tick and runs forward again. A machine stops (stalled()) only when it's more than `max_rollback` ticks past
// the last tick it knows every input for.
//
// The simulation must be deterministic, and save/load must cover everything it touches. Messages carry the last
// several inputs, so they can go unreliably (Delivery::Unreliable): a lost one is covered by the next.
//
//     net::Rollback<Input, State> rollback({.players = 2, .local = me}, {
//         .step = [&](u32 tick, std::span<const Input> inputs) { simulate(inputs); },
//         .save = [&] { return world_state(); },
//         .load = [&](const State& s) { restore(s); },
//     });
//     // every frame
//     while (auto e = host.receive()) if (e->type == net::Event::Type::Message) rollback.receive(e->data);
//     host.broadcast(rollback.advance(read_input()), net::Delivery::Unreliable);
template <class Input, class State> class Rollback {
public:
    struct Settings {
        u32 players = 2;
        u32 local = 0;        // this machine's player
        u32 delay = 1;        // ticks between taking an input and using it: fewer rollbacks, a little more lag
        u32 max_rollback = 8; // how far ahead of the known inputs a machine may run
    };
    struct Game {
        std::function<void(u32 tick, std::span<const Input> inputs)> step; // one tick, inputs in player order
        std::function<State()> save;
        std::function<void(const State&)> load;
    };

    Rollback(const Settings& settings, Game game) : s_(settings), game_(std::move(game)), last_(settings.players) {
        redundancy_ = 2 * (s_.max_rollback + s_.delay) + 1; // enough to cover any input the others can still need
        for (u32 t = 0; t < s_.delay; ++t)
            for (u32 p = 0; p < s_.players; ++p) confirm(t, p, Input{});
    }

    u32 tick() const { return tick_; }
    // Ticks below this ran with every real input: they're final.
    u32 confirmed() const { return std::min(confirmed_, tick_); }
    bool stalled() const { return tick_ >= confirmed_ + s_.max_rollback; }
    u32 rollbacks() const { return rollbacks_; }
    u32 resimulated() const { return resimulated_; } // ticks run again, in all

    // This player's input for tick() + delay, then one tick (re-running any that guessed wrong first). Returns the
    // message to send the others. While stalled() it runs nothing and ignores the input, but still returns the
    // message so lost ones get through.
    std::vector<u8> advance(const Input& input) {
        if (rollback_from_) { // correct the past first
            u32 from = *rollback_from_;
            rollback_from_.reset();
            game_.load(states_.at(from));
            ++rollbacks_;
            for (u32 t = from; t < tick_; ++t, ++resimulated_) simulate(t);
        }
        if (!stalled()) {
            if (states_.empty()) states_[0] = game_.save();
            confirm(tick_ + s_.delay, s_.local, input);
            newest_local_ = tick_ + s_.delay;
            simulate(tick_++);
            prune();
        }
        return message();
    }

    // The latest local inputs, as advance() returns them.
    std::vector<u8> message() const {
        InputMessage<Input> m{s_.local, 0, {}};
        if (!newest_local_) return m.encode();
        u32 newest = *newest_local_;
        m.first_tick = newest + 1 > redundancy_ ? newest + 1 - redundancy_ : 0;
        for (u32 t = m.first_tick; t <= newest; ++t) m.inputs.push_back(rows_.at(t).values[s_.local]);
        return m.encode();
    }

    // Another player's message. False if it isn't one.
    bool receive(std::span<const u8> bytes) {
        auto m = InputMessage<Input>::decode(bytes);
        if (!m || m->player >= s_.players || m->player == s_.local) return false;
        for (usize i = 0; i < m->inputs.size(); ++i) confirm(m->first_tick + static_cast<u32>(i), m->player, m->inputs[i]);
        return true;
    }

private:
    struct Row {
        std::vector<Input> values; // the real input, or the guess the last run used
        std::vector<bool> have;
        u32 count = 0;
    };
    Row& row(u32 t) {
        auto [it, fresh] = rows_.try_emplace(t);
        if (fresh) it->second.values.resize(s_.players), it->second.have.assign(s_.players, false);
        return it->second;
    }

    void confirm(u32 t, u32 player, const Input& input) {
        if (t < confirmed_) return; // known already
        Row& r = row(t);
        if (r.have[player]) return;
        if (t < tick_ && !(r.values[player] == input)) // the guess was wrong: run again from there
            rollback_from_ = rollback_from_ ? std::min(*rollback_from_, t) : t;
        r.values[player] = input;
        r.have[player] = true;
        ++r.count;
        if (!last_[player] || t >= last_[player]->first) last_[player] = {t, input};
        for (auto it = rows_.find(confirmed_); it != rows_.end() && it->second.count == s_.players; it = rows_.find(confirmed_))
            ++confirmed_;
    }

    void simulate(u32 t) {
        Row& r = row(t);
        for (u32 p = 0; p < s_.players; ++p) // guess: the latest real input this player sent
            if (!r.have[p]) r.values[p] = last_[p] ? last_[p]->second : Input{};
        game_.step(t, r.values);
        states_[t + 1] = game_.save();
    }

    void prune() {
        u32 keep = std::min(confirmed_, tick_); // runs again from here at the earliest
        while (!states_.empty() && states_.begin()->first < keep) states_.erase(states_.begin());
        if (newest_local_) keep = std::min(keep, *newest_local_ + 1 > redundancy_ ? *newest_local_ + 1 - redundancy_ : 0);
        while (!rows_.empty() && rows_.begin()->first < keep) rows_.erase(rows_.begin());
    }

    Settings s_;
    Game game_;
    std::vector<std::optional<std::pair<u32, Input>>> last_;
    std::map<u32, Row> rows_;
    std::map<u32, State> states_; // the state at the start of each tick that may run again
    std::optional<u32> rollback_from_;
    std::optional<u32> newest_local_;
    u32 tick_ = 0, confirmed_ = 0, redundancy_ = 0;
    u32 rollbacks_ = 0, resimulated_ = 0;
};

} // namespace ph::net
