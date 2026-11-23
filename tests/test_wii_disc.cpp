// Wii discs: partition table, title key, decryption and the H0-H3 hash tree, on a
// disc image built here with a test common key (the real keys are not in Orca).
#include "orca/crypto/aes.hpp"
#include "orca/crypto/sha1.hpp"
#include "orca/disc/wii_disc.hpp"
#include "wii_disc_builder.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace orca;
using namespace orca::disc;

using namespace orca::test_wii;

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
