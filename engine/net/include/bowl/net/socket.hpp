#pragma once

#include "bowl/core/types.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace bowl::net {

// An IPv4 or IPv6 address and port.
class Address {
public:
    Address() = default;
    // Resolves a host name or an IP literal ("example.com", "192.168.1.20", "::1"). Blocks while looking it up.
    static std::optional<Address> resolve(std::string_view host, u16 port);
    static Address loopback(u16 port, bool ipv6 = false);
    static Address any(u16 port, bool ipv6 = false);

    u16 port() const;
    bool ipv6() const;
    bool valid() const { return size_ > 0; }
    std::string to_string() const; // "192.168.1.20:27015", "[::1]:27015"
    bool operator==(const Address& o) const;

    // The platform's sockaddr, for the socket code.
    const void* data() const { return storage_.data(); }
    void* data() { return storage_.data(); }
    u32 size() const { return size_; }
    void set_size(u32 size) { size_ = size; }

private:
    alignas(8) std::array<u8, 128> storage_{}; // big enough for sockaddr_storage
    u32 size_ = 0;
};

// A non-blocking UDP socket.
class Socket : NonCopyable {
public:
    Socket() = default;
    ~Socket();
    Socket(Socket&& o) noexcept;
    Socket& operator=(Socket&& o) noexcept;

    // Port 0 picks a free one (port() then says which).
    bool open(u16 port = 0, bool ipv6 = false);
    void close();
    bool is_open() const;
    u16 port() const { return port_; }

    bool send(const Address& to, std::span<const u8> data);
    // One waiting packet into `buffer`: its size, or nothing if none is waiting.
    std::optional<usize> receive(Address& from, std::span<u8> buffer);

private:
    std::intptr_t handle_ = -1;
    u16 port_ = 0;
};

} // namespace bowl::net
