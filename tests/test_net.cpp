#include "ph/net/host.hpp"
#include "ph/net/lockstep.hpp"
#include "ph/net/rollback.hpp"

#include <doctest/doctest.h>

#include <deque>
#include <string>

using namespace ph;
using namespace ph::net;

namespace {

std::vector<u8> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }
std::string text_of(const std::vector<u8>& b) { return {b.begin(), b.end()}; }

// Runs both hosts on a made-up clock until `done` says so or `seconds` pass.
template <class Done> bool pump(Host& a, Host& b, f64& now, f64 seconds, Done done) {
    for (f64 end = now + seconds; now < end; now += 0.005) {
        a.update(now);
        b.update(now);
        if (done()) return true;
    }
    return false;
}

} // namespace

TEST_CASE("net: connect, reliable and unreliable messages, disconnect") {
    Host server, client;
    REQUIRE(server.start());
    REQUIRE(client.start());
    REQUIRE(server.port() != 0);
    PeerId to_server = client.connect(Address::loopback(server.port()));
    CHECK(to_server != 0);

    f64 now = 0;
    PeerId to_client = 0;
    bool client_connected = false;
    CHECK(pump(server, client, now, 2, [&] {
        while (auto e = server.receive())
            if (e->type == Event::Type::Connected) to_client = e->peer;
        while (auto e = client.receive())
            if (e->type == Event::Type::Connected) client_connected = e->peer == to_server;
        return to_client && client_connected;
    }));
    CHECK(server.peers() == std::vector<PeerId>{to_client});

    CHECK(client.send(to_server, bytes_of("hello")));
    CHECK(client.send(to_server, bytes_of("quick"), Delivery::Unreliable));
    CHECK_FALSE(client.send(to_server, std::vector<u8>(Host::max_unreliable + 1), Delivery::Unreliable));
    CHECK_FALSE(client.send(999, bytes_of("nobody")));
    std::vector<std::string> got;
    CHECK(pump(server, client, now, 2, [&] {
        while (auto e = server.receive())
            if (e->type == Event::Type::Message) got.push_back(text_of(e->data));
        return got.size() == 2;
    }));
    CHECK(std::find(got.begin(), got.end(), "hello") != got.end());
    CHECK(std::find(got.begin(), got.end(), "quick") != got.end());
    CHECK(server.stats(to_client)->rtt >= 0);

    client.disconnect(to_server);
    CHECK(client.peers().empty());
    bool gone = false;
    CHECK(pump(server, client, now, 2, [&] {
        while (auto e = server.receive()) gone = gone || (e->type == Event::Type::Disconnected && e->peer == to_client);
        return gone;
    }));
    CHECK(server.peers().empty());
}

TEST_CASE("net: reliable messages arrive once and in order through a bad network") {
    HostSettings bad{.simulated_loss = 0.3f, .simulated_latency = 0.03f, .simulated_jitter = 0.04f};
    Host server, client;
    REQUIRE(server.start(bad));
    REQUIRE(client.start(bad));
    PeerId to_server = client.connect(Address::loopback(server.port()));
    f64 now = 0;
    bool connected = false;
    REQUIRE(pump(server, client, now, 10, [&] {
        while (auto e = client.receive()) connected = connected || e->type == Event::Type::Connected;
        while (server.receive()) {}
        return connected;
    }));

    // Small ones, and a big one cut into pieces.
    std::vector<std::vector<u8>> sent;
    for (int i = 0; i < 300; ++i) {
        std::vector<u8> m = i == 150 ? std::vector<u8>(20000) : bytes_of("message " + std::to_string(i));
        if (i == 150)
            for (usize j = 0; j < m.size(); ++j) m[j] = static_cast<u8>(j * 7 + j / 300);
        sent.push_back(m);
        REQUIRE(client.send(to_server, m));
    }
    std::vector<std::vector<u8>> got;
    CHECK(pump(server, client, now, 30, [&] {
        while (auto e = server.receive())
            if (e->type == Event::Type::Message) got.push_back(std::move(e->data));
        return got.size() >= sent.size();
    }));
    CHECK(got == sent);
    auto stats = client.stats(to_server);
    REQUIRE(stats);
    CHECK(stats->resent > 0); // some were lost and sent again
    CHECK(stats->rtt > 0.05f);
}

TEST_CASE("net: a full host turns new peers away; a silent peer times out") {
    Host server, a, b;
    REQUIRE(server.start({.max_peers = 1}));
    REQUIRE(a.start({.timeout = 1}));
    REQUIRE(b.start());
    PeerId a_id = a.connect(Address::loopback(server.port()));
    PeerId b_id = b.connect(Address::loopback(server.port()));
    f64 now = 0;
    bool a_in = false, b_out = false;
    for (int i = 0; i < 400 && !(a_in && b_out); ++i, now += 0.005) {
        server.update(now), a.update(now), b.update(now);
        while (auto e = a.receive()) a_in = a_in || (e->type == Event::Type::Connected && e->peer == a_id);
        while (auto e = b.receive()) b_out = b_out || (e->type == Event::Type::Disconnected && e->peer == b_id);
        while (server.receive()) {}
    }
    CHECK(a_in);
    CHECK(b_out);

    // The server goes quiet (no more updates): after a's timeout it's gone.
    bool timed_out = false;
    for (int i = 0; i < 400 && !timed_out; ++i, now += 0.005) {
        a.update(now);
        while (auto e = a.receive()) timed_out = e->type == Event::Type::Disconnected && e->peer == a_id;
    }
    CHECK(timed_out);
}

TEST_CASE("net: bytes of plain structs") {
    struct P {
        i32 x;
        f32 y;
    };
    auto b = to_bytes(P{3, 1.5f});
    CHECK(b.size() == sizeof(P));
    auto p = from_bytes<P>(b);
    REQUIRE(p);
    CHECK(p->x == 3);
    CHECK(p->y == 1.5f);
    CHECK_FALSE(from_bytes<P>(std::span(b).first(3)));
}

namespace {

struct Pad {
    i8 dx = 0;
    u8 buttons = 0;
    bool operator==(const Pad&) const = default;
};

// What each player presses on each tick, made up but repeatable. Players hold a direction for a while, like people.
Pad pad_for(u32 player, u32 tick) {
    u32 h = (tick / 6 + 1) * 2654435761u ^ (player + 1) * 40503u;
    return {static_cast<i8>(static_cast<i32>(h % 5) - 2), static_cast<u8>((h >> 8) % 3)};
}

struct Sim {
    i64 x[2] = {0, 0};
    u64 hash = 1469598103934665603ull;
    void step(std::span<const Pad> pads) {
        for (usize p = 0; p < 2; ++p) {
            x[p] += pads[p].dx * (pads[p].buttons + 1);
            hash = (hash ^ static_cast<u64>(x[p] + 1000)) * 1099511628211ull;
        }
    }
};

// A network for tests: messages are lost or held back a few frames, never reordered within a frame.
struct FakeNet {
    struct InFlight {
        int arrive;
        u32 to;
        std::vector<u8> data;
    };
    std::deque<InFlight> flying;
    u64 random = 12345;
    f32 loss = 0;
    int max_delay = 0;
    u64 next() { return random = random * 6364136223846793005ull + 1442695040888963407ull; }
    void send(int frame, u32 to, std::vector<u8> data) {
        if (static_cast<f32>(next() >> 40) / static_cast<f32>(1 << 24) < loss) return;
        flying.push_back({frame + 1 + static_cast<int>((next() >> 33) % static_cast<u64>(max_delay + 1)), to, std::move(data)});
    }
    template <class F> void deliver(int frame, F f) {
        for (auto it = flying.begin(); it != flying.end();)
            if (it->arrive <= frame) f(it->to, it->data), it = flying.erase(it);
            else ++it;
    }
};

} // namespace

TEST_CASE("lockstep: both machines run the same ticks with the same inputs") {
    const u32 delay = 3;
    Lockstep<Pad> machines[2] = {Lockstep<Pad>(2, 0, delay), Lockstep<Pad>(2, 1, delay)};
    Sim sims[2];
    FakeNet net;
    net.max_delay = 4; // reliable delivery: late but never lost
    std::vector<u64> history[2];
    for (int frame = 0; frame < 400; ++frame) {
        net.deliver(frame, [&](u32 to, const std::vector<u8>& d) { CHECK(machines[to].receive(d)); });
        for (u32 m = 0; m < 2; ++m) {
            if (auto msg = machines[m].local(pad_for(m, machines[m].tick() + delay))) net.send(frame, 1 - m, *msg);
            if (machines[m].ready()) {
                sims[m].step(machines[m].inputs());
                history[m].push_back(sims[m].hash);
                machines[m].advance();
            }
        }
    }
    // The reference: the same ticks with every input known.
    Sim ref;
    std::vector<u64> expected;
    for (u32 t = 0; t < std::max(history[0].size(), history[1].size()); ++t) {
        Pad pads[2] = {t < delay ? Pad{} : pad_for(0, t), t < delay ? Pad{} : pad_for(1, t)};
        ref.step(pads);
        expected.push_back(ref.hash);
    }
    CHECK(history[0].size() > 200); // it waited sometimes, but kept going
    for (u32 m = 0; m < 2; ++m) CHECK(std::equal(history[m].begin(), history[m].end(), expected.begin()));
    CHECK_FALSE(Lockstep<Pad>(2, 0).receive(bytes_of("not an input message")));
}

TEST_CASE("rollback: machines guess, correct themselves, and agree on every confirmed tick") {
    const u32 delay = 1;
    Sim sims[2];
    std::vector<u64> history[2]; // the state after each tick, rewritten when a tick runs again
    using RB = Rollback<Pad, Sim>;
    auto make = [&](u32 m) {
        return RB({.players = 2, .local = m, .delay = delay, .max_rollback = 8},
                  {.step =
                       [&, m](u32 tick, std::span<const Pad> pads) {
                           sims[m].step(pads);
                           if (history[m].size() <= tick) history[m].resize(tick + 1);
                           history[m][tick] = sims[m].hash;
                       },
                   .save = [&, m] { return sims[m]; },
                   .load = [&, m](const Sim& s) { sims[m] = s; }});
    };
    RB machines[2] = {make(0), make(1)};
    FakeNet net;
    net.loss = 0.25f; // unreliable: lost often, late sometimes
    net.max_delay = 3;
    for (int frame = 0; frame < 600; ++frame) {
        net.deliver(frame, [&](u32 to, const std::vector<u8>& d) { CHECK(machines[to].receive(d)); });
        for (u32 m = 0; m < 2; ++m) net.send(frame, 1 - m, machines[m].advance(pad_for(m, machines[m].tick() + delay)));
    }
    // One more step each with nothing lost, so pending corrections run.
    net.loss = 0;
    for (u32 m = 0; m < 2; ++m) machines[m].advance(pad_for(m, machines[m].tick() + delay));

    Sim ref;
    u32 confirmed = std::min(machines[0].confirmed(), machines[1].confirmed());
    CHECK(confirmed > 400);
    for (u32 t = 0; t < confirmed; ++t) {
        Pad pads[2] = {t < delay ? Pad{} : pad_for(0, t), t < delay ? Pad{} : pad_for(1, t)};
        ref.step(pads);
        INFO("tick ", t);
        REQUIRE(history[0][t] == ref.hash);
        REQUIRE(history[1][t] == ref.hash);
    }
    CHECK(machines[0].rollbacks() > 0); // it guessed wrong and went back
    CHECK(machines[0].resimulated() > 0);
}
