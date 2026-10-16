// net::Host's packet handling: what anyone on the internet can send a game. A server and a client connect over
// loopback, then their sockets close and the input is a list of steps: packets from anywhere to either of them (raw, or
// sealed with the connection's keys so they reach the message parsing behind the encryption), answers from a
// rendezvous server, messages sent, and time passing.
//
// Nothing the input makes up goes on the network: the sockets are closed by then, so whatever the hosts send to
// addresses the input names is dropped.

#include "fuzz.hpp"

#include "bowl/net/host.hpp"

#include <chrono>

using namespace bowl;
using namespace bowl::net;

namespace {

void put32(std::vector<u8>& out, u32 v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}
void put64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

} // namespace

namespace bowl::net {

struct FuzzAccess {
    static Host::Peer* peer(Host& h, PeerId id) {
        auto it = h.peers_.find(id);
        return it == h.peers_.end() ? nullptr : it->second.get();
    }
    // What update() does with a packet its socket received.
    static void receive(Host& h, const Address& from, std::span<const u8> packet, f64 now) {
        if (h.is_rendezvous(from))
            h.handle_rendezvous(from, packet, now);
        else
            h.handle(from, 0, packet, now);
    }

    static void run(fuzz::Input in) {
        Socket rendezvous; // stands in for the rendezvous server while connecting, to hear the client's join nonce
        Host server, client;
        if (!rendezvous.open() || !server.start({.max_peers = 3}) || !client.start()) return;
        const Address rendezvous_address = Address::loopback(rendezvous.port());
        const Address server_address = Address::loopback(server.port());
        const Address client_address = Address::loopback(client.port());
        const Address stranger_address = Address::loopback(9, true);
        server.host_room(rendezvous_address, "room");
        PeerId to_server = client.connect(server_address);
        PeerId joining = client.join_room(rendezvous_address, "room");

        f64 now = 0;
        PeerId to_client = 0;
        for (auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(2);
             !(to_client && client.peers().size() == 1) && std::chrono::steady_clock::now() < give_up; now += 0.01) {
            server.update(now);
            client.update(now);
            if (auto peers = server.peers(); peers.size() == 1) to_client = peers[0];
        }
        if (!to_client) return; // a machine too busy to finish the handshake in time; not the input's doing
        u64 join_nonce = 0;
        std::array<u8, 2048> buffer;
        Address from;
        while (auto got = rendezvous.receive(from, buffer))
            if (*got == 4 + 1 + 4 + 1 + 4 + 8 + 1 && buffer[4] == 2) // JoinRoom: magic, kind, protocol, "room", nonce
                for (int i = 0; i < 8; ++i) join_nonce |= u64{buffer[14 + i]} << (8 * i);
        server.socket_.close();
        client.socket_.close();
        while (server.receive()) continue; // the events the handshake made
        while (client.receive()) continue;

        const Address* addresses[] = {&server_address, &client_address, &rendezvous_address, &stranger_address};
        for (int steps = 0; steps < 256 && !in.empty(); ++steps) {
            u8 op = in.byte();
            const Address& sender = *addresses[(op >> 4) & 3];
            switch (op & 7) {
            case 0:
                receive(server, sender, in.chunk(), now);
                break;
            case 1:
                receive(client, sender, in.chunk(), now);
                break;
            case 2: // sealed with the connection's keys: Data, or Disconnect when the top bit is set
            case 3: {
                bool to_server_side = (op & 7) == 2;
                Host& from_host = to_server_side ? client : server;
                Host::Peer* p = peer(from_host, to_server_side ? to_server : to_client);
                auto body = in.chunk();
                if (p)
                    receive(to_server_side ? server : client, to_server_side ? client_address : server_address,
                            from_host.sealed(*p, (op & 0x80) ? 5 : 4, {body.begin(), body.end()}), now);
                break;
            }
            case 4: { // the rendezvous server answers the client's join: Introduce, or NoRoom when the top bit is set
                std::vector<u8> packet;
                put32(packet, 0x5244564E);
                packet.push_back((op & 0x80) ? 4 : 3);
                put64(packet, join_nonce);
                auto rest = in.chunk(); // session, then u8 1 + address or u8 0
                packet.insert(packet.end(), rest.begin(), rest.end());
                receive(client, rendezvous_address, packet, now);
                break;
            }
            case 5: // time passes; both hosts time peers out and send what they have
                now += in.byte() * 0.02;
                server.send_or_time_out(now);
                client.send_or_time_out(now);
                break;
            case 6: { // the game sends a message
                auto message = in.chunk();
                Delivery delivery = (op & 0x80) ? Delivery::Unreliable : Delivery::Reliable;
                if (op & 0x40)
                    server.broadcast(message, delivery);
                else
                    client.send(to_server, message, delivery);
                break;
            }
            case 7: // someone hangs up
                if (op & 0x80)
                    server.disconnect(to_client);
                else
                    client.disconnect(to_server);
                break;
            }
            while (server.receive()) continue;
            while (client.receive()) continue;
        }
        server.stats(to_client);
        client.stats(to_server);
        client.stats(joining);
    }
};

} // namespace bowl::net

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzAccess::run(fuzz::Input{{data, size}});
    return 0;
}
