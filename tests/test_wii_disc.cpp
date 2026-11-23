// Wii discs: partition table, title key, decryption and the H0-H3 hash tree, on a
// disc image built here with a test common key (the real keys are not in Orca).
#include "orca/crypto/aes.hpp"
#include "orca/crypto/sha1.hpp"
#include "orca/disc/wii_disc.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace orca;
using namespace orca::disc;

namespace {

void put32(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); }
void put16(std::vector<u8>& d, std::size_t at, u16 v) { d[at] = static_cast<u8>(v >> 8); d[at + 1] = static_cast<u8>(v); }

constexpr AesKey kTestCommon{0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr AesKey kTitleKey{0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3, 0xD0, 0xD1, 0xD2, 0xD3};
constexpr u64 kTitleId = 0x0001000052534245ULL; // "RSBE"-like game title
constexpr u64 kPart = 0x50000;
constexpr u32 kTmdOff = 0x2C0, kTmdSize = 0x208, kCertOff = 0x4C8, kCertSize = 0x40, kH3Off = 0x8000, kDataOff = 0x20000;

// Plain partition data (deterministic), `clusters` x 0x7C00 bytes.
std::vector<u8> plain_data(u64 clusters) {
    std::vector<u8> d(clusters * kWiiClusterData);
    u32 x = 12345;
    for (auto& b : d) { x = x * 1103515245U + 12345U; b = static_cast<u8>(x >> 16); }
    const char* id = "RSBE01";
    std::memcpy(d.data(), id, 6);
    put32(d, 0x18, kWiiMagic);
    return d;
}

std::vector<u8> build_disc(const std::vector<u8>& plain, unsigned common_index = 0) {
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
    for (unsigned k = 0; k < 8; ++k) tmd[0x18C + k] = static_cast<u8>(kTitleId >> (56U - 8U * k));
    put16(tmd, 0x1DE, 1);
    put16(tmd, 0x1E4 + 6, 3);
    const auto hh = crypto::sha1(h3);
    std::memcpy(&tmd[0x1E4 + 0x10], hh.data(), 20);
    std::memcpy(&disc[kPart + kTmdOff], tmd.data(), kTmdSize);
    return disc;
}

WiiKeys test_keys() { WiiKeys k; k.common[0] = kTestCommon; return k; }

} // namespace

TEST_CASE(wii_partition_table_and_title_key) {
    auto img = build_disc(plain_data(3));
    MemoryDisc disc(img);
    std::string err;
    const auto parts = list_wii_partitions(disc, err);
    CHECK(parts.has_value());
    CHECK_EQ(parts->size(), 1U);
    CHECK_EQ((*parts)[0].offset, kPart);
    CHECK_EQ((*parts)[0].type, 0U);
    std::vector<u8> head(0x2A4);
    CHECK(disc.read(kPart, head));
    const WiiTicket t = parse_ticket(head);
    CHECK_EQ(t.title_id, kTitleId);
    const auto key = decrypt_title_key(t, test_keys(), err);
    CHECK(key.has_value());
    CHECK(*key == kTitleKey);
    // without the common key: a clear error
    WiiKeys none;
    CHECK(!decrypt_title_key(t, none, err).has_value());
    CHECK(err.find("not loaded") != std::string::npos);
}

TEST_CASE(wii_partition_decrypts_and_verifies_two_groups) {
    const auto plain = plain_data(70); // 64 clusters of group 0 + 6 of group 1
    auto img = build_disc(plain);
    MemoryDisc disc(img);
    std::string err;
    auto p = WiiPartition::open(disc, kPart, test_keys(), err);
    CHECK(p != nullptr);
    if (!p) { std::printf("    %s\n", err.c_str()); return; }
    CHECK_EQ(p->size(), u64{70} * kWiiClusterData);
    std::vector<u8> all(p->size());
    CHECK(p->read(0, all));
    CHECK(all == plain);
    // unaligned reads across cluster and group boundaries
    std::vector<u8> piece(0x9000);
    const u64 at = 63U * kWiiClusterData - 0x123U;
    CHECK(p->read(at, piece));
    CHECK(std::equal(piece.begin(), piece.end(), plain.begin() + static_cast<std::ptrdiff_t>(at)));
    std::vector<u8> past(16);
    CHECK(!p->read(p->size() - 8U, past));
}

TEST_CASE(wii_partition_rejects_tampering_and_wrong_keys) {
    const auto plain = plain_data(9);
    const auto good = build_disc(plain);
    std::string err;
    struct Case { std::size_t at; const char* expect; };
    const Case cases[] = {
        {kPart + kDataOff + 0x400 + 0x1234, "H0"},                        // data of cluster 0
        {kPart + kDataOff + 8 * kWiiClusterSize + 0x10, "H0"},            // hashes of cluster 8 (H0 area)
        {kPart + kDataOff + 0x290, "H1"},                                 // H1 list in cluster 0
        {kPart + kDataOff + 0x350, "H2"},                                 // H2 list in cluster 0
    };
    for (const Case& c : cases) {
        auto img = good;
        img[c.at] ^= 0x01;
        MemoryDisc disc(img);
        auto p = WiiPartition::open(disc, kPart, test_keys(), err);
        CHECK(p != nullptr);
        if (!p) continue;
        std::vector<u8> all(p->size());
        CHECK(!p->read(0, all));
        CHECK(p->last_error().find(c.expect) != std::string::npos);
        if (p->last_error().find(c.expect) == std::string::npos) std::printf("    %s\n", p->last_error().c_str());
    }
    { // H3 table no longer matches the TMD
        auto img = good;
        img[kPart + kH3Off + 3] ^= 0x80;
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(err.find("H3") != std::string::npos);
    }
    { // another common key: the title key is wrong, so H0 fails
        WiiKeys k;
        AesKey other = kTestCommon;
        other[0] ^= 1;
        k.common[0] = other;
        MemoryDisc disc(good);
        auto p = WiiPartition::open(disc, kPart, k, err);
        CHECK(p != nullptr);
        std::vector<u8> b(16);
        if (p) CHECK(!p->read(0, b));
    }
    { // Korean key index selects common[1]
        MemoryDisc disc(build_disc(plain, 1));
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        WiiKeys k;
        k.common[1] = kTestCommon;
        auto p = WiiPartition::open(disc, kPart, k, err);
        CHECK(p != nullptr);
        std::vector<u8> b(0x7C00);
        if (p) CHECK(p->read(0, b));
    }
}

TEST_CASE(wii_key_files) {
    const std::string raw = "/tmp/orca_test_common.key", bootmii = "/tmp/orca_test_keys.bin", bad = "/tmp/orca_test_bad.key";
    { std::ofstream f(raw, std::ios::binary); f.write(reinterpret_cast<const char*>(kTestCommon.data()), 16); }
    {
        std::vector<char> kb(0x400, 0);
        std::memcpy(&kb[0x114], kTestCommon.data(), 16);
        std::ofstream f(bootmii, std::ios::binary);
        f.write(kb.data(), static_cast<std::streamsize>(kb.size()));
    }
    { std::ofstream f(bad, std::ios::binary); f << "short"; }
    WiiKeys k;
    std::string err;
    CHECK(load_wii_key_file(raw, k, 1, err));
    CHECK(k.common[1].has_value() && *k.common[1] == kTestCommon);
    CHECK(load_wii_key_file(bootmii, k, 0, err));
    CHECK(k.common[0].has_value() && *k.common[0] == kTestCommon);
    CHECK(!load_wii_key_file(bad, k, 0, err));
    CHECK(!load_wii_key_file("/tmp/does/not/exist", k, 0, err));
}

TEST_MAIN("wii_disc")
