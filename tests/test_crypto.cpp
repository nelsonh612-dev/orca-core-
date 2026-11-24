// AES-128 and SHA-1 against the published test vectors:
//   FIPS-197 Appendix B (cipher example) and C.1 (AES-128), SP 800-38A F.2.1/F.2.2
//   (CBC-AES128), FIPS 180-4 / RFC 3174 examples for SHA-1.
#include "orca/crypto/aes.hpp"
#include "orca/crypto/sha1.hpp"
#include "test_framework.hpp"
#include <array>
#include <chrono>

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::crypto;

namespace {
std::vector<u8> hex(const std::string& s) {
    std::vector<u8> out;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) out.push_back(static_cast<u8>(std::stoul(s.substr(i, 2), nullptr, 16)));
    return out;
}
#define CHECK_HEX(a, b) do { const std::string x_ = (a), y_ = (b); if (x_ != y_) std::printf("    %s != %s\n", x_.c_str(), y_.c_str()); CHECK(x_ == y_); } while (0)
std::string hex_of(std::span<const u8> d) {
    static const char* k = "0123456789abcdef";
    std::string s;
    for (u8 b : d) { s += k[b >> 4]; s += k[b & 15]; }
    return s;
}
} // namespace

TEST_CASE(aes128_fips197_vectors) {
    {
        // Appendix B: key 2b7e1516..., plaintext 3243f6a8... -> 3925841d...
        const auto key = hex("2b7e151628aed2a6abf7158809cf4f3c");
        const auto pt = hex("3243f6a8885a308d313198a2e0370734");
        Aes128 a(std::span<const u8, 16>(key.data(), 16));
        u8 ct[16], back[16];
        a.encrypt_block(pt.data(), ct);
        CHECK_HEX(hex_of(ct), std::string("3925841d02dc09fbdc118597196a0b32"));
        a.decrypt_block(ct, back);
        CHECK_HEX(hex_of(back), hex_of(pt));
    }
    {
        // Appendix C.1: key 000102..0f, plaintext 00112233..ff -> 69c4e0d8...
        const auto key = hex("000102030405060708090a0b0c0d0e0f");
        const auto pt = hex("00112233445566778899aabbccddeeff");
        Aes128 a(std::span<const u8, 16>(key.data(), 16));
        u8 ct[16], back[16];
        a.encrypt_block(pt.data(), ct);
        CHECK_HEX(hex_of(ct), std::string("69c4e0d86a7b0430d8cdb78070b4c55a"));
        a.decrypt_block(ct, back);
        CHECK_HEX(hex_of(back), hex_of(pt));
    }
}

TEST_CASE(aes128_cbc_sp800_38a) {
    // F.2.1 CBC-AES128.Encrypt / F.2.2 Decrypt
    const auto key = hex("2b7e151628aed2a6abf7158809cf4f3c");
    const auto iv = hex("000102030405060708090a0b0c0d0e0f");
    const std::string pt = "6bc1bee22e409f96e93d7e117393172a" "ae2d8a571e03ac9c9eb76fac45af8e51"
                           "30c81c46a35ce411e5fbc1191a0a52ef" "f69f2445df4f9b17ad2b417be66c3710";
    const std::string ct = "7649abac8119b246cee98e9b12e9197d" "5086cb9b507219ee95db113a917678b2"
                           "73bed6b8e3c1743b7116e69e22229516" "3ff1caa1681fac09120eca307586e1a7";
    Aes128 a(std::span<const u8, 16>(key.data(), 16));
    auto data = hex(pt);
    Aes128::Block v{};
    for (unsigned i = 0; i < 16; ++i) v[i] = iv[i];
    CHECK(a.cbc_encrypt(data, v));
    CHECK_HEX(hex_of(data), ct);
    for (unsigned i = 0; i < 16; ++i) v[i] = iv[i];
    // in two calls: the IV carries over
    CHECK(a.cbc_decrypt(std::span<u8>(data.data(), 32), v));
    CHECK(a.cbc_decrypt(std::span<u8>(data.data() + 32, 32), v));
    CHECK_HEX(hex_of(data), pt);
    std::vector<u8> odd(15);
    CHECK(!a.cbc_decrypt(odd, v));
}

TEST_CASE(sha1_fips180_examples) {
    const std::string abc = "abc";
    CHECK_HEX(hex_of(sha1(std::span<const u8>(reinterpret_cast<const u8*>(abc.data()), abc.size()))),
             std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    const std::string two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK_HEX(hex_of(sha1(std::span<const u8>(reinterpret_cast<const u8*>(two.data()), two.size()))),
             std::string("84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    CHECK_HEX(hex_of(sha1({})), std::string("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
    // one million 'a', fed in uneven pieces
    Sha1 s;
    const std::vector<u8> chunk(997, 'a');
    std::size_t left = 1000000;
    while (left > 0) {
        const std::size_t n = std::min(left, chunk.size());
        s.update(std::span<const u8>(chunk.data(), n));
        left -= n;
    }
    CHECK_HEX(hex_of(s.finish()), std::string("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
}

TEST_CASE(aes_hardware_path_matches_the_portable_code) {
    // On AArch64 CPUs with the Cryptography Extension, CBC goes through AESE/AESD;
    // both paths must give the same bytes for random keys, IVs and lengths.
    std::printf("[crypto] ARMv8 AES instructions: %s\n", crypto::Aes128::hardware_available() ? "available, compared with the portable code" : "not available on this host (portable code only)");
    u32 x = 0x12345678U;
    const auto rnd = [&] { x ^= x << 13; x ^= x >> 17; x ^= x << 5; return static_cast<u8>(x); };
    for (int round = 0; round < 200; ++round) {
        std::array<u8, 16> key{};
        crypto::Aes128::Block iv{};
        for (auto& b : key) b = rnd();
        for (auto& b : iv) b = rnd();
        std::vector<u8> data(16U * (1U + static_cast<unsigned>(rnd()) % 64U));
        for (auto& b : data) b = rnd();
        const crypto::Aes128 aes(key);
        auto sw_enc = data, hw_enc = data, sw_dec = data, hw_dec = data;
        auto iv1 = iv, iv2 = iv, iv3 = iv, iv4 = iv;
        crypto::Aes128::set_hardware_enabled(false);
        CHECK(aes.cbc_encrypt(sw_enc, iv1));
        CHECK(aes.cbc_decrypt(sw_dec, iv3));
        crypto::Aes128::set_hardware_enabled(true);
        CHECK(aes.cbc_encrypt(hw_enc, iv2));
        CHECK(aes.cbc_decrypt(hw_dec, iv4));
        CHECK(sw_enc == hw_enc && iv1 == iv2);
        CHECK(sw_dec == hw_dec && iv3 == iv4);
        auto back = sw_enc;
        auto iv5 = iv;
        CHECK(aes.cbc_decrypt(back, iv5));
        CHECK(back == data);
    }
    // throughput of a Wii cluster's data (0x7C00 bytes), both paths
    const std::array<u8, 16> key{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const crypto::Aes128 aes(key);
    std::vector<u8> cluster(0x7C00, 0x5A);
    for (const bool hw : {false, true}) {
        if (hw && !crypto::Aes128::hardware_available()) break;
        crypto::Aes128::set_hardware_enabled(hw);
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 64; ++i) { crypto::Aes128::Block iv{}; (void)aes.cbc_decrypt(cluster, iv); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::printf("[crypto] CBC decryption, %s: %.1f MB/s (64 clusters in %.2f ms)\n", hw ? "ARMv8 instructions" : "portable code",
                    64.0 * 0x7C00 / 1e6 / (ms / 1e3), ms);
    }
    crypto::Aes128::set_hardware_enabled(true);
}

TEST_MAIN("crypto")
