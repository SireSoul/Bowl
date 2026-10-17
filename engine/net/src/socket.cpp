#include "bowl/net/socket.hpp"

#include "bowl/core/log.hpp"

#include <cstring>
#include <format>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen = int;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using socklen = socklen_t;
#endif

namespace bowl::net {

namespace {

void start_sockets() {
#if defined(_WIN32)
    static std::once_flag once;
    std::call_once(once, [] {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) error("net: WSAStartup failed");
    });
#endif
}

#if defined(_WIN32)
using Handle = SOCKET;
constexpr Handle no_socket = INVALID_SOCKET;
void close_handle(Handle h) { closesocket(h); }
#else
using Handle = int;
constexpr Handle no_socket = -1;
void close_handle(Handle h) { ::close(h); }
#endif

Handle as_handle(std::intptr_t h) { return static_cast<Handle>(h); }

} // namespace

// Address

std::optional<Address> Address::resolve(std::string_view host, u16 port) {
    start_sockets();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* found = nullptr;
    std::string name(host), service = std::to_string(port);
    if (getaddrinfo(name.c_str(), service.c_str(), &hints, &found) != 0 || !found) return std::nullopt;
    Address a;
    std::memcpy(a.storage_.data(), found->ai_addr, found->ai_addrlen);
    a.size_ = static_cast<u32>(found->ai_addrlen);
    freeaddrinfo(found);
    return a;
}

Address Address::loopback(u16 port, bool ipv6) {
    Address a;
    if (ipv6) {
        sockaddr_in6 s{};
        s.sin6_family = AF_INET6;
        s.sin6_port = htons(port);
        s.sin6_addr = in6addr_loopback;
        std::memcpy(a.storage_.data(), &s, sizeof s);
        a.size_ = sizeof s;
    } else {
        sockaddr_in s{};
        s.sin_family = AF_INET;
        s.sin_port = htons(port);
        s.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        std::memcpy(a.storage_.data(), &s, sizeof s);
        a.size_ = sizeof s;
    }
    return a;
}

Address Address::any(u16 port, bool ipv6) {
    Address a;
    if (ipv6) {
        sockaddr_in6 s{};
        s.sin6_family = AF_INET6;
        s.sin6_port = htons(port);
        s.sin6_addr = in6addr_any;
        std::memcpy(a.storage_.data(), &s, sizeof s);
        a.size_ = sizeof s;
    } else {
        sockaddr_in s{};
        s.sin_family = AF_INET;
        s.sin_port = htons(port);
        s.sin_addr.s_addr = htonl(INADDR_ANY);
        std::memcpy(a.storage_.data(), &s, sizeof s);
        a.size_ = sizeof s;
    }
    return a;
}

bool Address::ipv6() const { return size_ > 0 && reinterpret_cast<const sockaddr*>(storage_.data())->sa_family == AF_INET6; }

u16 Address::port() const {
    if (!size_) return 0;
    if (ipv6()) return ntohs(reinterpret_cast<const sockaddr_in6*>(storage_.data())->sin6_port);
    return ntohs(reinterpret_cast<const sockaddr_in*>(storage_.data())->sin_port);
}

std::string Address::to_string() const {
    if (!size_) return "(none)";
    char text[INET6_ADDRSTRLEN] = {};
    if (ipv6()) {
        inet_ntop(AF_INET6, &reinterpret_cast<const sockaddr_in6*>(storage_.data())->sin6_addr, text, sizeof text);
        return std::format("[{}]:{}", text, port());
    }
    inet_ntop(AF_INET, &reinterpret_cast<const sockaddr_in*>(storage_.data())->sin_addr, text, sizeof text);
    return std::format("{}:{}", text, port());
}

void Address::write(std::vector<u8>& out) const {
    auto put = [&](const void* bytes, usize n) {
        auto* b = static_cast<const u8*>(bytes);
        out.insert(out.end(), b, b + n);
    };
    u16 p = port();
    if (ipv6()) {
        out.push_back(6);
        put(&reinterpret_cast<const sockaddr_in6*>(storage_.data())->sin6_addr, 16);
    } else {
        out.push_back(4);
        put(&reinterpret_cast<const sockaddr_in*>(storage_.data())->sin_addr, 4);
    }
    out.push_back(static_cast<u8>(p >> 8));
    out.push_back(static_cast<u8>(p));
}

std::optional<Address> Address::read(std::span<const u8> in, usize& at) {
    if (at >= in.size()) return std::nullopt;
    u8 family = in[at];
    usize n = family == 6 ? 16 : family == 4 ? 4 : 0;
    if (n == 0 || at + 1 + n + 2 > in.size()) return std::nullopt;
    const u8* ip = in.data() + at + 1;
    u16 p = static_cast<u16>((in[at + 1 + n] << 8) | in[at + 2 + n]);
    at += 1 + n + 2;
    Address a = any(p, family == 6);
    if (family == 6) std::memcpy(&reinterpret_cast<sockaddr_in6*>(a.storage_.data())->sin6_addr, ip, 16);
    else std::memcpy(&reinterpret_cast<sockaddr_in*>(a.storage_.data())->sin_addr, ip, 4);
    return a;
}

bool Address::operator==(const Address& o) const {
    if (size_ != o.size_ || ipv6() != o.ipv6() || port() != o.port()) return false;
    if (!size_) return true;
    if (ipv6())
        return std::memcmp(&reinterpret_cast<const sockaddr_in6*>(storage_.data())->sin6_addr,
                           &reinterpret_cast<const sockaddr_in6*>(o.storage_.data())->sin6_addr, sizeof(in6_addr)) == 0;
    return reinterpret_cast<const sockaddr_in*>(storage_.data())->sin_addr.s_addr ==
           reinterpret_cast<const sockaddr_in*>(o.storage_.data())->sin_addr.s_addr;
}

// Socket

Socket::~Socket() { close(); }

Socket::Socket(Socket&& o) noexcept : handle_(o.handle_), port_(o.port_) { o.handle_ = -1; }

Socket& Socket::operator=(Socket&& o) noexcept {
    if (this != &o) {
        close();
        handle_ = o.handle_;
        port_ = o.port_;
        o.handle_ = -1;
    }
    return *this;
}

bool Socket::is_open() const { return handle_ != -1; }

bool Socket::open(u16 port, bool ipv6, bool loopback) {
    start_sockets();
    close();
    Handle h = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h == no_socket) {
        error("net: can't make a UDP socket");
        return false;
    }
    if (ipv6) { // take IPv4 too
        int off = 0;
        setsockopt(h, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&off), sizeof off);
    }
    Address local = loopback ? Address::loopback(port, ipv6) : Address::any(port, ipv6);
    if (::bind(h, static_cast<const sockaddr*>(local.data()), static_cast<socklen>(local.size())) != 0) {
        error("net: can't use port {} (in use?)", port);
        close_handle(h);
        return false;
    }
#if defined(_WIN32)
    u_long nonblocking = 1;
    ioctlsocket(h, FIONBIO, &nonblocking);
#else
    fcntl(h, F_SETFL, fcntl(h, F_GETFL, 0) | O_NONBLOCK);
#endif
    Address bound;
    socklen len = sizeof(sockaddr_storage);
    getsockname(h, static_cast<sockaddr*>(bound.data()), &len);
    bound.set_size(static_cast<u32>(len));
    handle_ = static_cast<std::intptr_t>(h);
    port_ = bound.port();
    return true;
}

void Socket::close() {
    if (handle_ != -1) close_handle(as_handle(handle_));
    handle_ = -1;
    port_ = 0;
}

bool Socket::send(const Address& to, std::span<const u8> data) {
    if (!is_open() || !to.valid()) return false;
    auto sent = ::sendto(as_handle(handle_), reinterpret_cast<const char*>(data.data()), static_cast<int>(data.size()), 0,
                         static_cast<const sockaddr*>(to.data()), static_cast<socklen>(to.size()));
    return sent == static_cast<decltype(sent)>(data.size());
}

std::optional<usize> Socket::receive(Address& from, std::span<u8> buffer) {
    if (!is_open()) return std::nullopt;
    socklen len = sizeof(sockaddr_storage);
    auto got = ::recvfrom(as_handle(handle_), reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
                          static_cast<sockaddr*>(from.data()), &len);
    if (got < 0) return std::nullopt; // nothing waiting (or an error the next packet will tell about)
    from.set_size(static_cast<u32>(len));
    return static_cast<usize>(got);
}

} // namespace bowl::net
