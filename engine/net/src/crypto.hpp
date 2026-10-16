#pragma once

#include "bowl/core/types.hpp"

#include <array>
#include <span>
#include <string_view>

// The cryptography under net::Host, on Monocypher: X25519 to agree on keys, BLAKE2b to derive them, and
// XChaCha20-Poly1305 to encrypt and authenticate packets.
namespace bowl::net::crypto {

constexpr usize key_size = 32;
constexpr usize mac_size = 16;
using Key = std::array<u8, key_size>;
using Mac = std::array<u8, mac_size>;

// Random bytes from the operating system (getrandom, arc4random, BCryptGenRandom), good for keys.
void random_bytes(std::span<u8> out);
u64 random_u64();

// A one-time key pair for one connection. Its secret half is wiped when it goes.
struct KeyPair {
    Key secret{}, public_key{};
    KeyPair();
    ~KeyPair();
    KeyPair(const KeyPair&) = default;
    KeyPair& operator=(const KeyPair&) = default;
};

// One key per direction, the same on both ends.
struct SessionKeys {
    Key client_to_server{}, server_to_client{};
    ~SessionKeys();
};

// What both ends compute from their own key pair and the other's public key, bound to everything the handshake said.
// `secret` (HostSettings::secret) is mixed in, so ends with different secrets get different keys and can't talk.
// False when the other's public key is no good (it would make a predictable key).
bool derive(SessionKeys& out, const KeyPair& mine, const Key& theirs, bool client, u64 client_salt, u64 server_salt,
            std::string_view secret);

// Encrypts `text` in place and writes its tag. `counter` must never repeat with the same key; `extra` is
// authenticated (it can't be changed unnoticed) but not encrypted.
void seal(std::span<u8> text, Mac& mac, const Key& key, u64 counter, std::span<const u8> extra);
// Checks the tag and decrypts in place. False, with `text` untouched, if anything was changed or the key is wrong.
bool open(std::span<u8> text, const Mac& mac, const Key& key, u64 counter, std::span<const u8> extra);

} // namespace bowl::net::crypto
