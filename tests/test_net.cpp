#include "bowl/net/host.hpp"
#include "bowl/net/lockstep.hpp"
#include "bowl/net/rendezvous.hpp"
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

// Lets `seconds` of the made-up clock pass, for checking that something does *not* happen.
template <class Update> void wait(f64& now, f64 seconds, Update update) {
    for (f64 end = now + seconds; now < end; now += 0.005) update(now);
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
    auto update = [&](f64 t) { client.update(t), middle.pass(), server.update(t), middle.pass(); };
    auto step = [&](f64 seconds, auto done) { return run(now, seconds, update, done); };
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
    wait(now, 0.5, update);
    collect();
    CHECK(got.size() == 2);

    // A made-up disconnect, with the connection's token but no keys, is ignored.
    std::vector<u8> forged(carried.begin(), carried.begin() + 13); // protocol, kind, token
    forged[4] = 5;                                                 // kind 5: Disconnect
    forged.insert(forged.end(), {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F});
    forged.resize(forged.size() + 16); // a tag that isn't one
    middle.socket.send(middle.server, forged);
    wait(now, 0.5, update);
    collect();
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

// A rendezvous server, a host with a room and a player, all on this machine.
struct Rooms {
    Rendezvous rendezvous;
    Host host, player;
    f64 now = 0;
    PeerId to_host = 0, to_player = 0;
    bool player_in = false, player_out = false;
    std::vector<std::string> host_got, player_got;

    Rooms(HostSettings host_settings = {}) {
        REQUIRE(rendezvous.start({.port = 0}));
        REQUIRE(host.start(host_settings));
        REQUIRE(player.start());
        host.host_room(server(), "dragons-1234");
    }
    Address server() const { return Address::loopback(rendezvous.port()); }
    void update(f64 t) { rendezvous.update(t), host.update(t), player.update(t); }
    bool events() {
        while (auto e = host.receive()) {
            if (e->type == Event::Type::Connected) to_player = e->peer;
            if (e->type == Event::Type::Message) host_got.push_back(text_of(e->data));
        }
        while (auto e = player.receive()) {
            if (e->type == Event::Type::Connected) player_in = true;
            if (e->type == Event::Type::Disconnected) player_out = true;
            if (e->type == Event::Type::Message) player_got.push_back(text_of(e->data));
        }
        return false;
    }
    bool join(Route route, f64 seconds = 5) {
        wait(now, 0.1, [&](f64 t) { update(t); }); // the host tells the server about its room
        to_host = player.join_room(server(), "dragons-1234", route);
        return run(now, seconds, [&](f64 t) { update(t); }, [&] { return events(), (player_in && to_player) || player_out; }) &&
               player_in;
    }
    void talk() {
        CHECK(player.send(to_host, bytes_of("hello host")));
        CHECK(host.send(to_player, bytes_of("hello player")));
        CHECK(run(now, 2, [&](f64 t) { update(t); }, [&] { return events(), !host_got.empty() && !player_got.empty(); }));
        CHECK(host_got == std::vector<std::string>{"hello host"});
        CHECK(player_got == std::vector<std::string>{"hello player"});
    }
};

TEST_CASE("net: rooms: a player joins a host through a rendezvous server, directly when it can") {
    Rooms rooms;
    REQUIRE(rooms.join(Route::Auto));
    rooms.talk();
    CHECK_FALSE(rooms.player.stats(rooms.to_host)->relayed);
    CHECK_FALSE(rooms.host.stats(rooms.to_player)->relayed);
    CHECK(rooms.rendezvous.stats().introductions == 1);
    CHECK(rooms.rendezvous.stats().relayed_packets == 0);
}

TEST_CASE("net: rooms: Route::Relay goes through the server, and neither side learns the other's address") {
    Rooms rooms;
    REQUIRE(rooms.join(Route::Relay));
    rooms.talk();
    CHECK(rooms.player.stats(rooms.to_host)->relayed);
    CHECK(rooms.host.stats(rooms.to_player)->relayed);
    CHECK(rooms.host.stats(rooms.to_player)->address == rooms.server()); // all the host sees is the server
    CHECK(rooms.player.stats(rooms.to_host)->address == rooms.server());
    CHECK(rooms.rendezvous.stats().relayed_packets > 0);
}

TEST_CASE("net: rooms: when the host can't be reached directly, the player takes the relay") {
    Rooms rooms({.simulated_unreachable = true});
    REQUIRE(rooms.join(Route::Auto));
    CHECK(rooms.player.stats(rooms.to_host)->relayed);
    rooms.talk();
}

TEST_CASE("net: rooms: joining a room nobody hosts ends quickly") {
    Rooms rooms;
    rooms.host.stop_hosting_room(); // before it ever told the server
    f64 before = rooms.now;
    CHECK_FALSE(rooms.join(Route::Auto));
    CHECK(rooms.player_out);
    CHECK(rooms.now - before < 1); // the server says so: no waiting for a timeout
}

TEST_CASE("net: lobbies: listed rooms show in the list, fullest first; unlisted and other games' rooms don't") {
    Rendezvous rendezvous;
    REQUIRE(rendezvous.start({.port = 0}));
    Address server = Address::loopback(rendezvous.port());
    Host busy, quiet, secret, other_game, guest, browser;
    REQUIRE(busy.start());
    REQUIRE(quiet.start());
    REQUIRE(secret.start());
    REQUIRE(other_game.start({.protocol = 7}));
    REQUIRE(guest.start());
    REQUIRE(browser.start());
    busy.host_room(server, "room-busy", {.listed = true, .label = "Farm co-op", .max_players = 4});
    quiet.host_room(server, "room-quiet", {.listed = true, .label = "Quiet valley"});
    secret.host_room(server, "room-secret"); // unlisted: found only by name
    other_game.host_room(server, "room-other", {.listed = true});
    f64 now = 0;
    auto update = [&](f64 t) {
        rendezvous.update(t), busy.update(t), quiet.update(t), secret.update(t), other_game.update(t), guest.update(t),
            browser.update(t);
    };
    bool guest_in = false;
    guest.join_room(server, "room-busy");
    REQUIRE(run(now, 3, update, [&] {
        while (auto e = guest.receive()) guest_in = guest_in || e->type == Event::Type::Connected;
        while (busy.receive()) {}
        return guest_in;
    }));
    wait(now, 1.1, update); // busy tells the server it has two players now

    bool listed = false;
    browser.list_rooms(server);
    REQUIRE(run(now, 2, update, [&] {
        while (auto e = browser.receive()) listed = listed || e->type == Event::Type::RoomsListed;
        return listed;
    }));
    REQUIRE(browser.listed_rooms().size() == 2);
    CHECK(browser.listed_total() == 2);
    const RoomListing& first = browser.listed_rooms()[0];
    CHECK(first.room == "room-busy");
    CHECK(first.label == "Farm co-op");
    CHECK(first.players == 2);
    CHECK(first.max_players == 4);
    CHECK(browser.listed_rooms()[1].room == "room-quiet");
    CHECK(browser.listed_rooms()[1].players == 1);
    CHECK(browser.listed_rooms()[1].max_players == 33); // max_peers + 1
}

TEST_CASE("net: lobbies: quick match joins the fullest listed room with space, and ends when none has any") {
    Rendezvous rendezvous;
    REQUIRE(rendezvous.start({.port = 0}));
    Address server = Address::loopback(rendezvous.port());
    Host full, roomy, emptyish, player, late;
    REQUIRE(full.start());
    REQUIRE(roomy.start());
    REQUIRE(emptyish.start());
    REQUIRE(player.start());
    REQUIRE(late.start());
    full.host_room(server, "full", {.listed = true, .max_players = 1});    // just the host: no space
    roomy.host_room(server, "roomy", {.listed = true, .max_players = 8});
    emptyish.host_room(server, "emptyish", {.listed = true, .max_players = 8});
    f64 now = 0;
    auto update = [&](f64 t) {
        rendezvous.update(t), full.update(t), roomy.update(t), emptyish.update(t), player.update(t), late.update(t);
    };
    wait(now, 0.1, update);         // the hosts tell the server about their rooms
    late.join_room(server, "roomy"); // roomy gets a player, so it's the fullest room with space
    bool first_in = false;
    REQUIRE(run(now, 3, update, [&] {
        while (auto e = late.receive()) first_in = first_in || e->type == Event::Type::Connected;
        return first_in;
    }));
    wait(now, 1.1, update);

    PeerId matched = player.quick_match(server);
    int roomy_joined = 0, emptyish_joined = 0;
    bool player_in = false;
    REQUIRE(run(now, 3, update, [&] {
        while (auto e = roomy.receive()) roomy_joined += e->type == Event::Type::Connected;
        while (auto e = emptyish.receive()) emptyish_joined += e->type == Event::Type::Connected;
        while (auto e = player.receive()) player_in = player_in || (e->type == Event::Type::Connected && e->peer == matched);
        return player_in;
    }));
    CHECK(roomy_joined == 1); // the player matched into roomy (late joined it before)
    CHECK(emptyish_joined == 0);

    // A server where the only listed room is full: quick match ends at once.
    Rendezvous fresh;
    REQUIRE(fresh.start({.port = 0}));
    Address fresh_server = Address::loopback(fresh.port());
    full.host_room(fresh_server, "full", {.listed = true, .max_players = 1});
    Host nobody;
    REQUIRE(nobody.start());
    wait(now, 0.1, [&](f64 t) { fresh.update(t), full.update(t); });
    PeerId none = nobody.quick_match(fresh_server);
    bool ended = false;
    f64 before = now;
    CHECK(run(now, 3, [&](f64 t) { fresh.update(t), full.update(t), nobody.update(t); }, [&] {
        while (auto e = nobody.receive()) ended = ended || (e->type == Event::Type::Disconnected && e->peer == none);
        return ended;
    }));
    CHECK(now - before < 1); // the server says so: no waiting for a timeout
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
    // Everything can arrive before the first acknowledgement makes it back, 30-70 ms later: wait for one.
    CHECK(pump(server, client, now, 5, [&] { return client.stats(to_server)->rtt > 0; }));
    CHECK(client.stats(to_server)->rtt > 0.05f);
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
