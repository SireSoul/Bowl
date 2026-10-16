#include "bowl/net/replication.hpp"

#include <doctest/doctest.h>

#include <chrono>
#include <thread>

using namespace bowl;
using namespace bowl::net;

namespace replication_test {

struct Position {
    Vec2 value;
};
BOWL_COMPONENT(Position, value)

struct Health {
    i32 hp = 0;
    std::string name;
};
BOWL_COMPONENT(Health, hp, name)

struct Follow {
    ecs::Entity target;
};
BOWL_COMPONENT(Follow, target)

struct Gift {
    i32 item = 0;
    std::string to;
};
BOWL_REFLECT(Gift, item, to)

struct Wave {
    i32 times = 0;
};
BOWL_REFLECT(Wave, times)

// A host world and a player's, connected, with the same registrations on both sides.
struct Game {
    ecs::World host_world, player_world;
    Host host, player;
    Replicator replicator{host_world};
    Replica replica{player_world};
    PeerId to_player = 0, to_host = 0;
    f64 now = 0;

    explicit Game(HostSettings settings = {}) {
        REQUIRE(host.start(settings));
        REQUIRE(player.start(settings));
        replicator.replicate<Position>(Sync::Continuous);
        replicator.replicate<Health>();
        replicator.replicate<Follow>();
        replica.replicate<Position>(Sync::Continuous);
        replica.replicate<Health>();
        replica.replicate<Follow>();
        to_host = player.connect(Address::loopback(host.port()));
        REQUIRE(step(5, [&] { return to_player != 0 && player.peers().size() == 1; }));
    }
    void tick(f64 t) {
        host.update(t);
        replicator.update(host);
        player.update(t);
        while (auto e = host.receive())
            if (e->type == Event::Type::Connected) to_player = e->peer;
        while (auto e = player.receive()) replica.handle(*e);
    }
    // Ticks on a made-up clock until `done`, then on a held one for up to a real second (packets use real sockets).
    template <class Done> bool step(f64 seconds, Done done) {
        for (f64 end = now + seconds; now < end; now += 0.005) {
            tick(now);
            if (done()) return true;
        }
        for (auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(1);
             std::chrono::steady_clock::now() < give_up;) {
            tick(now);
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    }
    ecs::Entity on_player(ecs::Entity host_entity) { return replica.entity(replicator.net_id(host_entity)); }
};

} // namespace replication_test

using namespace replication_test;

TEST_CASE("net: typed messages arrive as their struct, from the peer that sent them") {
    Host host, player;
    REQUIRE(host.start());
    REQUIRE(player.start());
    PeerId to_host = player.connect(Address::loopback(host.port()));
    PeerId from_player = 0;
    Messages host_messages;
    std::vector<std::pair<PeerId, Gift>> gifts;
    host_messages.on<Gift>([&](PeerId from, const Gift& g) { gifts.emplace_back(from, g); });
    f64 now = 0;
    bool connected = false;
    for (int i = 0; i < 2000 && !(connected && from_player); ++i, now += 0.005) {
        host.update(now), player.update(now);
        while (auto e = host.receive()) if (e->type == Event::Type::Connected) from_player = e->peer;
        while (auto e = player.receive()) connected = connected || e->type == Event::Type::Connected;
        if (i > 400) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(connected);

    Messages player_messages;
    CHECK(player_messages.send(player, to_host, Gift{.item = 7, .to = "calla"}));
    CHECK(player_messages.send(player, to_host, Wave{.times = 3})); // nobody on the host listens for Wave
    CHECK(player.send(to_host, std::vector<u8>{'h', 'i'}));          // the game's own bytes
    int handled = 0, own = 0; // messages handle() took (Gift and Wave), and the game's own
    for (int i = 0; i < 2000 && handled + own < 3; ++i, now += 0.005) {
        host.update(now), player.update(now);
        while (auto e = host.receive()) {
            if (e->type != Event::Type::Message) continue;
            if (host_messages.handle(*e)) ++handled;
            else ++own;
        }
        if (i > 400) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(handled == 2); // Wave is Messages' too, though nobody listens for it
    CHECK(own == 1);     // the game's own bytes: handle() said false
    REQUIRE(gifts.size() == 1);
    CHECK(gifts[0].first == from_player);
    CHECK(gifts[0].second.item == 7);
    CHECK(gifts[0].second.to == "calla");
}

TEST_CASE("net: replication shows the host's entities to a player, references and all, and follows changes") {
    Game game;
    ecs::World& w = game.host_world;
    ecs::Entity farmer = w.create(Replicated{}, Position{{10, 20}}, Health{100, "farmer"});
    ecs::Entity dog = w.create(Replicated{}, Position{{12, 20}}, Follow{farmer});
    w.create(Position{{0, 0}}); // not replicated: the player never sees it

    REQUIRE(game.step(2, [&] { return game.replica.size() == 2; }));
    ecs::World& p = game.player_world;
    ecs::Entity farmer_p = game.on_player(farmer), dog_p = game.on_player(dog);
    REQUIRE(p.alive(farmer_p));
    REQUIRE(p.alive(dog_p));
    CHECK(p.entity_count() == 2);
    CHECK(p.get<Health>(farmer_p).hp == 100);
    CHECK(p.get<Health>(farmer_p).name == "farmer");
    CHECK(p.get<Position>(dog_p).value == Vec2{12, 20});
    CHECK(p.get<Follow>(dog_p).target == farmer_p); // the host's entity, as the player's own
    CHECK(p.get<Replicated>(farmer_p).id == game.replicator.net_id(farmer));
    CHECK_FALSE(p.has<Health>(dog_p));

    // Changes follow: a reliable one, and a moving one many times (only the newest matters).
    w.get<Health>(farmer).hp = 80;
    for (int i = 0; i < 20; ++i) {
        w.get<Position>(farmer).value.x += 1;
        game.tick(game.now += 0.005);
    }
    CHECK(game.step(2, [&] { return p.get<Health>(farmer_p).hp == 80 && p.get<Position>(farmer_p).value.x == 30; }));

    // A component taken away goes away; an entity destroyed goes away.
    w.remove<Health>(farmer);
    CHECK(game.step(2, [&] { return !p.has<Health>(farmer_p); }));
    w.destroy(dog);
    CHECK(game.step(2, [&] { return !p.alive(dog_p); }));
    CHECK(game.replica.size() == 1);
    // One added later arrives too.
    w.add<Health>(farmer) = {50, "back"};
    CHECK(game.step(2, [&] { return p.has<Health>(farmer_p) && p.get<Health>(farmer_p).name == "back"; }));
}

TEST_CASE("net: replication shows each player only what's relevant to them") {
    Game game;
    ecs::World& w = game.host_world;
    ecs::Entity near = w.create(Replicated{}, Position{{1, 1}}, Health{5, "near"});
    ecs::Entity far = w.create(Replicated{}, Position{{900, 900}}, Health{6, "far"});
    bool far_shown = false;
    game.replicator.relevant([&](PeerId, ecs::Entity e) {
        return game.host_world.get<Position>(e).value.x < 100 || far_shown;
    });
    REQUIRE(game.step(2, [&] { return game.replica.size() == 1; }));
    CHECK(game.player_world.alive(game.on_player(near)));
    CHECK_FALSE(game.on_player(far));

    far_shown = true; // it comes into view: in full
    REQUIRE(game.step(2, [&] { return game.replica.size() == 2; }));
    CHECK(game.player_world.get<Health>(game.on_player(far)).name == "far");
    far_shown = false; // and out of it again
    CHECK(game.step(2, [&] { return game.replica.size() == 1; }));
}

TEST_CASE("net: replication gets there through a bad network") {
    Game game({.simulated_loss = 0.3f, .simulated_latency = 0.03f, .simulated_jitter = 0.04f});
    ecs::World& w = game.host_world;
    std::vector<ecs::Entity> many;
    for (int i = 0; i < 40; ++i)
        many.push_back(w.create(Replicated{}, Position{{static_cast<f32>(i), 0}}, Health{i, "n" + std::to_string(i)}));
    REQUIRE(game.step(10, [&] { return game.replica.size() == 40; }));
    for (int round = 0; round < 50; ++round) {
        for (ecs::Entity e : many) w.get<Position>(e).value.y += 1;
        game.tick(game.now += 0.005);
    }
    for (ecs::Entity e : many) w.get<Health>(e).hp += 1000;
    CHECK(game.step(10, [&] {
        for (ecs::Entity e : many) {
            ecs::Entity pe = game.on_player(e);
            if (!game.player_world.alive(pe)) return false;
            if (game.player_world.get<Position>(pe).value.y != 50) return false; // lost ones are sent again
            if (game.player_world.get<Health>(pe).hp != w.get<Health>(e).hp) return false;
        }
        return true;
    }));
}
