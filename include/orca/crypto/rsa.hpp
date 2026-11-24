// Orca Core — RSA signature verification (PKCS#1 v1.5 with SHA-1), for the Wii
// certificate chain, tickets and TMDs. Written from RFC 8017 (sections 8.2.2 and
// 9.2): EM = 0x00 0x01 0xFF..0xFF 0x00 || DigestInfo(SHA-1) || hash, and the
// signature s is valid when s^e mod n == EM. Moduli of 2048 and 4096 bits (the
// Wii's two RSA sizes); arithmetic with Montgomery multiplication on 32-bit limbs.
//
// `rsa_raw` (x^d mod n with any exponent) is exposed so tests can make signatures
// with their own test keys; nothing in Orca signs anything.
#pragma once
#include "orca/core/bits.hpp"
#include "orca/crypto/secure.hpp"
#include "orca/crypto/sha1.hpp"

#include <span>
#include <vector>

namespace orca::crypto {

// Big-endian byte strings, as stored on the disc. The modulus length (256 or 512
// bytes) fixes the signature length.
struct RsaPublicKey {
    std::vector<u8> modulus;
    u32 exponent{65537};
};

// out = base^exponent mod modulus (all big-endian; out has the modulus' length).
// false if the modulus is even, zero, or base >= modulus.
bool rsa_raw(std::span<const u8> base, std::span<const u8> exponent, std::span<const u8> modulus, std::vector<u8>& out);

// PKCS#1 v1.5 encoding of a SHA-1 digest for a `length`-byte modulus.
std::vector<u8> pkcs1_sha1_encoding(const Sha1Digest& digest, std::size_t length);

// true only for a signature of exactly the modulus' length that decodes to the
// PKCS#1 v1.5 SHA-1 encoding of `digest`.
bool rsa_verify_sha1(const RsaPublicKey& key, std::span<const u8> signature, const Sha1Digest& digest);

} // namespace orca::crypto
