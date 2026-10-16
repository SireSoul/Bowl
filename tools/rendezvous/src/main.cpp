// bowl-rendezvous: runs a net::Rendezvous server, which players meet through to play in rooms (Host::host_room,
// Host::join_room): it introduces them so they can connect straight to each other, and relays their (encrypted)
// packets when their routers won't let them. Run it on a machine everyone can reach, with its UDP port open.
//
//     bowl-rendezvous [--port 27016] [--relay-kbps 256] [--ipv6]

#include "bowl/net/rendezvous.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace {
std::atomic<bool> stopping{false};
}

int main(int argc, char** argv) {
    bowl::net::RendezvousSettings settings;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            settings.port = static_cast<bowl::u16>(std::atoi(argv[++i]));
        } else if (arg == "--relay-kbps" && i + 1 < argc) {
            settings.relay_bytes_per_second = static_cast<bowl::u32>(std::atoi(argv[++i])) * 1024;
        } else if (arg == "--ipv6") {
            settings.ipv6 = true;
        } else {
            std::fprintf(stderr, "usage: bowl-rendezvous [--port 27016] [--relay-kbps 256] [--ipv6]\n");
            return 2;
        }
    }
    bowl::net::Rendezvous server;
    if (!server.start(settings)) {
        std::fprintf(stderr, "bowl-rendezvous: couldn't open UDP port %u\n", static_cast<unsigned>(settings.port));
        return 1;
    }
    std::signal(SIGINT, [](int) { stopping = true; });
    std::signal(SIGTERM, [](int) { stopping = true; });
    std::printf("bowl-rendezvous: listening on UDP port %u (relay %u KB/s per connection each way)\n",
                static_cast<unsigned>(server.port()), static_cast<unsigned>(settings.relay_bytes_per_second / 1024));
    std::fflush(stdout);

    auto next_report = std::chrono::steady_clock::now() + std::chrono::minutes(1);
    while (!stopping) {
        server.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (std::chrono::steady_clock::now() >= next_report) {
            auto s = server.stats();
            std::printf("rooms %zu, introductions %llu, relayed %llu packets (%llu KB), dropped %llu\n", s.rooms,
                        static_cast<unsigned long long>(s.introductions),
                        static_cast<unsigned long long>(s.relayed_packets),
                        static_cast<unsigned long long>(s.relayed_bytes / 1024),
                        static_cast<unsigned long long>(s.dropped_packets));
            std::fflush(stdout);
            next_report += std::chrono::minutes(1);
        }
    }
    std::printf("bowl-rendezvous: stopped\n");
    return 0;
}
