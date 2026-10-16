#include "crypto.hpp"

#include "bowl/core/assert.hpp"

#include <cstring>
#include <monocypher.h>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#include <stdlib.h> // arc4random_buf
#else
#include <cerrno>
#include <sys/random.h>
#endif

namespace bowl::net::crypto {

void random_bytes(std::span<u8> out) {
#if defined(_WIN32)
    // Keys made without good randomness can be guessed, so failing here stops the program in every build.
    if (BCryptGenRandom(nullptr, out.data(), static_cast<ULONG>(out.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        ::bowl::detail::assert_fail("BCryptGenRandom", __FILE__, __LINE__, "no secure random numbers");
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    arc4random_buf(out.data(), out.size());
#else
    for (usize at = 0; at < out.size();) {
        ssize_t got = getrandom(out.data() + at, out.size() - at, 0);
        if (got < 0) {
            // Keys made without good randomness can be guessed, so this stops the program in every build.
            if (errno != EINTR) ::bowl::detail::assert_fail("getrandom", __FILE__, __LINE__, "no secure random numbers");
            continue;
        }
        at += static_cast<usize>(got);
    }
#endif
}

u64 random_u64() {
    u64 v = 0;
    random_bytes({reinterpret_cast<u8*>(&v), sizeof(v)});
    return v;
}

KeyPair::KeyPair() {
    random_bytes(secret);
    crypto_x25519_public_key(public_key.data(), secret.data());
}

KeyPair::~KeyPair() { crypto_wipe(secret.data(), secret.size()); }

SessionKeys::~SessionKeys() {
    crypto_wipe(client_to_server.data(), client_to_server.size());
    crypto_wipe(server_to_client.data(), server_to_client.size());
}

namespace {

void put_u64(std::vector<u8>& out, u64 v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<u8>(v >> (8 * i)));
}

std::array<u8, 24> nonce_for(u64 counter) {
    std::array<u8, 24> nonce{};
    for (int i = 0; i < 8; ++i) nonce[static_cast<usize>(i)] = static_cast<u8>(counter >> (8 * i));
    return nonce;
}

} // namespace

bool derive(SessionKeys& out, const KeyPair& mine, const Key& theirs, bool client, u64 client_salt, u64 server_salt,
            std::string_view secret) {
    Key shared{};
    crypto_x25519(shared.data(), mine.secret.data(), theirs.data());
    const Key zero{};
    if (crypto_verify32(shared.data(), zero.data()) == 0) return false; // a low-order point: anyone could know it

    // The secret keys the hash, so the result depends on it; without one, a fixed label does.
    Key hash_key{};
    if (secret.empty()) {
        static constexpr char label[] = "bowl net v1";
        std::memcpy(hash_key.data(), label, sizeof(label) - 1);
    } else {
        crypto_blake2b(hash_key.data(), hash_key.size(), reinterpret_cast<const u8*>(secret.data()), secret.size());
    }
    const Key& client_public = client ? mine.public_key : theirs;
    const Key& server_public = client ? theirs : mine.public_key;
    std::vector<u8> transcript(shared.begin(), shared.end());
    transcript.insert(transcript.end(), client_public.begin(), client_public.end());
    transcript.insert(transcript.end(), server_public.begin(), server_public.end());
    put_u64(transcript, client_salt);
    put_u64(transcript, server_salt);

    std::array<u8, 64> keys{};
    crypto_blake2b_keyed(keys.data(), keys.size(), hash_key.data(), hash_key.size(), transcript.data(), transcript.size());
    std::memcpy(out.client_to_server.data(), keys.data(), key_size);
    std::memcpy(out.server_to_client.data(), keys.data() + key_size, key_size);
    crypto_wipe(shared.data(), shared.size());
    crypto_wipe(hash_key.data(), hash_key.size());
    crypto_wipe(transcript.data(), transcript.size());
    crypto_wipe(keys.data(), keys.size());
    return true;
}

void seal(std::span<u8> text, Mac& mac, const Key& key, u64 counter, std::span<const u8> extra) {
    auto nonce = nonce_for(counter);
    crypto_aead_lock(text.data(), mac.data(), key.data(), nonce.data(), extra.data(), extra.size(), text.data(),
                     text.size());
}

bool open(std::span<u8> text, const Mac& mac, const Key& key, u64 counter, std::span<const u8> extra) {
    auto nonce = nonce_for(counter);
    return crypto_aead_unlock(text.data(), mac.data(), key.data(), nonce.data(), extra.data(), extra.size(),
                              text.data(), text.size()) == 0;
}

} // namespace bowl::net::crypto
