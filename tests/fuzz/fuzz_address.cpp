// Addresses inside packets (Address::read), as rendezvous servers send them.

#include "fuzz.hpp"

#include "bowl/net/socket.hpp"

using namespace bowl;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::span<const u8> in(data, size);
    for (usize at = 0; at <= in.size();) {
        usize start = at;
        auto address = net::Address::read(in, at);
        if (!address) {
            BOWL_FUZZ_CHECK(at == start); // a failed read moves nothing
            ++at;
            continue;
        }
        // What reads, writes back as the same bytes.
        BOWL_FUZZ_CHECK(at > start && at <= in.size());
        std::vector<u8> out;
        address->write(out);
        BOWL_FUZZ_CHECK(std::equal(out.begin(), out.end(), in.begin() + static_cast<std::ptrdiff_t>(start),
                                   in.begin() + static_cast<std::ptrdiff_t>(at)));
        address->to_string();
    }
    return 0;
}
