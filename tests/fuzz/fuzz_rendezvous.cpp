// net::Rendezvous, the server players meet through: it takes packets from anyone. The input is a list of packets, each
// from one of a few addresses, with time passing in between.
//
// Nothing goes on the network: the server isn't started, so its answers are dropped.

#include "fuzz.hpp"

#include "bowl/net/rendezvous.hpp"

using namespace bowl;
using namespace bowl::net;

namespace bowl::net {

struct FuzzAccess {
    static void run(fuzz::Input in) {
        Rendezvous server;
        static const Address from[] = {Address::loopback(9), Address::loopback(10), Address::loopback(10, true),
                                       *Address::resolve("192.0.2.1", 27015)};
        f64 now = 0;
        for (int steps = 0; steps < 256 && !in.empty(); ++steps) {
            u8 op = in.byte();
            now += (op >> 2) * 0.1; // up to 6 seconds: rooms and sessions time out
            server.handle(from[op & 3], in.chunk(), now);
        }
        server.stats();
    }
};

} // namespace bowl::net

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    FuzzAccess::run(fuzz::Input{{data, size}});
    return 0;
}
