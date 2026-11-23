// Synthetic Wii disc for tests: one game partition (type 0) at 0x50000, its
// ticket (title key encrypted with a test common key), TMD (IOS, group, the
// H3 table hash), H3 table and encrypted, hashed clusters. The real common
// keys are not in Orca.
#pragma once
#include "orca/crypto/aes.hpp"
#include "orca/crypto/sha1.hpp"
#include "orca/disc/wii_disc.hpp"

#include <cstring>
#include <vector>

namespace orca::test_wii {
using namespace orca::disc;

inline void put32(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); }
inline void put16(std::vector<u8>& d, std::size_t at, u16 v) { d[at] = static_cast<u8>(v >> 8); d[at + 1] = static_cast<u8>(v); }

constexpr AesKey kTestCommon{0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr AesKey kTitleKey{0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3, 0xD0, 0xD1, 0xD2, 0xD3};
constexpr u64 kTitleId = 0x0001000052534245ULL; // "RSBE"-like game title
constexpr u64 kPart = 0x50000;
constexpr u32 kTmdOff = 0x2C0, kTmdSize = 0x208, kCertOff = 0x4C8, kCertSize = 0x40, kH3Off = 0x8000, kDataOff = 0x20000;

// Plain partition data (deterministic), `clusters` x 0x7C00 bytes.
inline std::vector<u8> plain_data(u64 clusters) {
    std::vector<u8> d(clusters * kWiiClusterData);
    u32 x = 12345;
    for (auto& b : d) { x = x * 1103515245U + 12345U; b = static_cast<u8>(x >> 16); }
    const char* id = "RSBE01";
    std::memcpy(d.data(), id, 6);
    put32(d, 0x18, kWiiMagic);
    return d;
}

inline std::vector<u8> build_disc(const std::vector<u8>& plain, unsigned common_index = 0, u32 ios = 36, u16 group = 0x3031) {
    const u64 clusters = plain.size() / kWiiClusterData;
    std::vector<u8> disc(kPart + kDataOff + clusters * kWiiClusterSize, 0);
    const char* id = "RSBE01";
    std::memcpy(disc.data(), id, 6);
    put32(disc, 0x18, kWiiMagic);
    // partition table: group 0 has one partition (type 0) listed at 0x40020
    put32(disc, 0x40000, 1);
    put32(disc, 0x40004, 0x40020U >> 2);
    put32(disc, 0x40020, static_cast<u32>(kPart >> 2));
    put32(disc, 0x40024, 0);
    // ticket
    std::vector<u8> enc(kTitleKey.begin(), kTitleKey.end());
    crypto::Aes128 common(kTestCommon);
    crypto::Aes128::Block iv{};
    for (unsigned k = 0; k < 8; ++k) iv[k] = static_cast<u8>(kTitleId >> (56U - 8U * k));
    (void)common.cbc_encrypt(enc, iv);
    std::memcpy(&disc[kPart + 0x1BF], enc.data(), 16);
    for (unsigned k = 0; k < 8; ++k) disc[kPart + 0x1DC + k] = static_cast<u8>(kTitleId >> (56U - 8U * k));
    disc[kPart + 0x1F1] = static_cast<u8>(common_index);
    put32(disc, kPart + 0x2A4, kTmdSize);
    put32(disc, kPart + 0x2A8, kTmdOff >> 2);
    put32(disc, kPart + 0x2AC, kCertSize);
    put32(disc, kPart + 0x2B0, kCertOff >> 2);
    put32(disc, kPart + 0x2B4, kH3Off >> 2);
    put32(disc, kPart + 0x2B8, kDataOff >> 2);
    put32(disc, kPart + 0x2BC, static_cast<u32>((clusters * kWiiClusterSize) >> 2));
    // hash tree and encryption, group by group
    crypto::Aes128 tk(kTitleKey);
    std::vector<u8> h3(kWiiH3Size, 0);
    const u64 groups = (clusters + 63U) / 64U;
    for (u64 g = 0; g < groups; ++g) {
        std::vector<std::vector<u8>> hb(64, std::vector<u8>(0x400, 0));
        for (unsigned c = 0; c < 64; ++c) { // H0
            const u64 ci = g * 64U + c;
            if (ci >= clusters) continue;
            for (unsigned k = 0; k < 31; ++k) {
                const auto h = crypto::sha1(std::span<const u8>(&plain[ci * kWiiClusterData + 0x400U * k], 0x400));
                std::memcpy(&hb[c][20 * k], h.data(), 20);
            }
        }
        std::vector<u8> h2(0xA0, 0);
        for (unsigned s = 0; s < 8; ++s) { // H1 of subgroup s, copied to its 8 clusters
            std::vector<u8> h1(0xA0, 0);
            for (unsigned c = 0; c < 8; ++c) {
                const auto h = crypto::sha1(std::span<const u8>(hb[8 * s + c].data(), 0x26C));
                std::memcpy(&h1[20 * c], h.data(), 20);
            }
            for (unsigned c = 0; c < 8; ++c) std::memcpy(&hb[8 * s + c][0x280], h1.data(), 0xA0);
            const auto h = crypto::sha1(h1);
            std::memcpy(&h2[20 * s], h.data(), 20);
        }
        for (unsigned c = 0; c < 64; ++c) std::memcpy(&hb[c][0x340], h2.data(), 0xA0);
        const auto h = crypto::sha1(h2);
        std::memcpy(&h3[20 * g], h.data(), 20);
        for (unsigned c = 0; c < 64; ++c) {
            const u64 ci = g * 64U + c;
            if (ci >= clusters) continue;
            u8* out = &disc[kPart + kDataOff + ci * kWiiClusterSize];
            std::vector<u8> hashes = hb[c];
            crypto::Aes128::Block z{};
            (void)tk.cbc_encrypt(hashes, z);
            std::memcpy(out, hashes.data(), 0x400);
            crypto::Aes128::Block div{};
            std::memcpy(div.data(), &hashes[0x3D0], 16);
            std::vector<u8> data(plain.begin() + static_cast<std::ptrdiff_t>(ci * kWiiClusterData),
                                 plain.begin() + static_cast<std::ptrdiff_t>((ci + 1U) * kWiiClusterData));
            (void)tk.cbc_encrypt(data, div);
            std::memcpy(out + 0x400, data.data(), kWiiClusterData);
        }
    }
    std::memcpy(&disc[kPart + kH3Off], h3.data(), kWiiH3Size);
    // TMD: one content, the H3 table
    std::vector<u8> tmd(kTmdSize, 0);
    put32(tmd, 0, 0x00010001U);
    put32(tmd, 0x184, 1);  // IOS title 00000001-<ios>
    put32(tmd, 0x188, ios);
    put16(tmd, 0x198, group);
    for (unsigned k = 0; k < 8; ++k) tmd[0x18C + k] = static_cast<u8>(kTitleId >> (56U - 8U * k));
    put16(tmd, 0x1DE, 1);
    put16(tmd, 0x1E4 + 6, 3);
    const auto hh = crypto::sha1(h3);
    std::memcpy(&tmd[0x1E4 + 0x10], hh.data(), 20);
    std::memcpy(&disc[kPart + kTmdOff], tmd.data(), kTmdSize);
    return disc;
}

inline WiiKeys test_keys() { WiiKeys k; k.common[0] = kTestCommon; return k; }


} // namespace orca::test_wii
