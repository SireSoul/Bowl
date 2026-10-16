#include "bowl/net/host.hpp"
#include "bowl/net/lockstep.hpp"
#include "bowl/net/rollback.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <deque>
#include <string>
#include <thread>

using namespace bowl;
using namespace bowl::net;

namespace {

std::vector<u8> bytes_of(std::string_view s) { return {s.begin(), s.end()}; }
std::string text_of(const std::vector<u8>& b) { return {b.begin(), b.end()}; }

// Steps `update(now)` on a made-up clock, 5 ms at a time, until `done` says so or `seconds` pass. The packets go
// through real sockets, though, and a busy machine (a CI runner) can take longer to deliver them than the made-up
// clock takes to run out, so then it keeps stepping, with the clock held still, for up to a real second.
template <class Update, class Done> bool run(f64& now, f64 seconds, Update update, Done done) {
    for (f64 end = now + seconds; now < end; now += 0.005) {
        update(now);
        if (done()) return true;
    }
    for (auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
         std::chrono::steady_clock::now() < give_up;) {
        update(now);
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template <class Done> bool pump(Host& a, Host& b, f64& now, f64 seconds, Done done) {
    return run(now, seconds, [&](f64 t) { a.update(t), b.update(t); }, done);
}

// Sits between a client and a server, passing packets along, so a test can look at them, change them or send them
// again: someone on the network in between.
struct Middle {
    Socket socket;
    Address server, client;
    std::vector<std::vector<u8>> from_client; // every packet the client sent, as it was
    bool tamper_next_data = false;             // flip a byte in the client's next Data packet

    explicit Middle(u16 server_port) : server(Address::loopback(server_port)) { REQUIRE(socket.open()); }
    Address address() const { return Address::loopback(socket.port()); }
    void pass() {
        std::array<u8, 2048> buffer;
        Address from;
        while (auto got = socket.receive(from, buffer)) {
            std::vector<u8> packet(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(*got));
            if (from == server) {
                if (client.valid()) socket.send(client, packet);
                continue;
            }
            client = from;
            from_client.push_back(packet);
            if (tamper_next_data && packet.size() > 40 && packet[4] == 4) { // kind 4: Data
                packet.back() ^= 0x55;
                tamper_next_data = false;
            }
            socket.send(server, packet);
        }
    }
};

bool contains(const std::vector<u8>& haystack, std::string_view needle) {
    return std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
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

TEST_CASE("net: connections are encrypted: nothing on the way can read, change, replay or forge a packet") {
    Host server, client;
    REQUIRE(server.start({.timeout = 1}));
    REQUIRE(client.start());
    Middle middle(server.port());
    PeerId to_server = client.connect(middle.address());
    f64 now = 0;
    auto step = [&](f64 seconds, auto done) {
        return run(now, seconds, [&](f64 t) { client.update(t), middle.pass(), server.update(t), middle.pass(); }, done);
    };
    PeerId to_client = 0;
    bool client_in = false;
    REQUIRE(step(2, [&] {
        while (auto e = server.receive()) if (e->type == Event::Type::Connected) to_client = e->peer;
        while (auto e = client.receive()) client_in = client_in || e->type == Event::Type::Connected;
        return to_client && client_in;
    }));

    std::vector<std::string> got;
    bool server_lost_client = false;
    auto collect = [&] {
        while (auto e = server.receive()) {
            if (e->type == Event::Type::Message) got.push_back(text_of(e->data));
            if (e->type == Event::Type::Disconnected) server_lost_client = true;
        }
    };
    CHECK(client.send(to_server, bytes_of("attack at dawn")));
    REQUIRE(step(2, [&] { return collect(), got.size() == 1; }));
    CHECK(got[0] == "attack at dawn");
    std::vector<u8> carried; // the packet that carried it
    for (const auto& packet : middle.from_client) {
        CHECK_FALSE(contains(packet, "attack at dawn")); // never readable on the way
        if (packet.size() > 40 && packet[4] == 4 && packet.size() > carried.size()) carried = packet; // the biggest
    }
    REQUIRE_FALSE(carried.empty());

    // A changed packet is dropped; the reliable message is sent again and arrives once.
    middle.tamper_next_data = true;
    CHECK(client.send(to_server, bytes_of("hold the line")));
    REQUIRE(step(3, [&] { return collect(), got.size() == 2; }));
    CHECK(got[1] == "hold the line");
    CHECK_FALSE(middle.tamper_next_data);

    // A packet sent again later isn't delivered again.
    middle.socket.send(middle.server, carried);
    step(0.5, [&] { return collect(), false; });
    CHECK(got.size() == 2);

    // A made-up disconnect, with the connection's token but no keys, is ignored.
    std::vector<u8> forged(carried.begin(), carried.begin() + 13); // protocol, kind, token
    forged[4] = 5;                                                 // kind 5: Disconnect
    forged.insert(forged.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F});
    forged.resize(forged.size() + 16); // a tag that isn't one
    middle.socket.send(middle.server, forged);
    step(0.5, [&] { return collect(), false; });
    CHECK_FALSE(server_lost_client);
    CHECK(server.peers() == std::vector<PeerId>{to_client});

    // The client vanishes. Its old packets, sent again and again, don't keep it alive on the server.
    int steps = 0;
    CHECK(run(now, 3, [&](f64 t) {
        if (steps++ % 20 == 0) middle.socket.send(middle.server, carried);
        server.update(t);
    }, [&] { return collect(), server_lost_client; }));
}

TEST_CASE("net: with a secret, only hosts that know it connect") {
    Host server, stranger, friend_;
    REQUIRE(server.start({.secret = "open sesame"}));
    REQUIRE(stranger.start({.secret = "let me in", .timeout = 1}));
    REQUIRE(friend_.start({.secret = "open sesame"}));
    PeerId s = stranger.connect(Address::loopback(server.port()));
    PeerId f = friend_.connect(Address::loopback(server.port()));
    f64 now = 0;
    int server_connections = 0;
    bool stranger_gone = false, friend_in = false;
    run(now, 3, [&](f64 t) { server.update(t), stranger.update(t), friend_.update(t); }, [&] {
        while (auto e = server.receive()) server_connections += e->type == Event::Type::Connected;
        while (auto e = stranger.receive()) stranger_gone = stranger_gone || (e->type == Event::Type::Disconnected && e->peer == s);
        while (auto e = friend_.receive()) friend_in = friend_in || (e->type == Event::Type::Connected && e->peer == f);
        return stranger_gone && friend_in;
    });
    CHECK(friend_in);
    CHECK(stranger_gone);           // never answered with keys it could use
    CHECK(server_connections == 1); // and the server never counted it as connected
    CHECK(server.peers().size() == 1);
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
    run(now, 2, [&](f64 t) { server.update(t), a.update(t), b.update(t); }, [&] {
        while (auto e = a.receive()) a_in = a_in || (e->type == Event::Type::Connected && e->peer == a_id);
        while (auto e = b.receive()) b_out = b_out || (e->type == Event::Type::Disconnected && e->peer == b_id);
        while (server.receive()) {}
        return a_in && b_out;
    });
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
