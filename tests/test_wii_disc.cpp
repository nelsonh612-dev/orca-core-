// Wii discs: partition table, tickets, TMDs, certificate chains and their RSA
// signatures, title keys, decryption, the H0-H3 hash tree and the cluster cache,
// on disc images built here with a test common key and test RSA keys (the real
// keys are not in Orca).
#include "orca/crypto/aes.hpp"
#include "orca/crypto/rsa.hpp"
#include "orca/crypto/sha1.hpp"
#include "orca/disc/wii_disc.hpp"
#include "orca/disc/wii_meta.hpp"
#include "wii_disc_builder.hpp"
#include "wii_test_keys.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

using namespace orca;
using namespace orca::disc;
using namespace orca::test_wii;

namespace {
bool contains(const std::string& s, const char* what) {
    const bool ok = s.find(what) != std::string::npos;
    if (!ok) std::printf("    expected \"%s\" in: %s\n", what, s.c_str());
    return ok;
}
std::vector<u8> ticket_of(const std::vector<u8>& img, u64 part = kPart) {
    return std::vector<u8>(img.begin() + static_cast<std::ptrdiff_t>(part), img.begin() + static_cast<std::ptrdiff_t>(part + kTicketSize));
}
std::vector<u8> tmd_of(const std::vector<u8>& img, u64 part = kPart) {
    return std::vector<u8>(img.begin() + static_cast<std::ptrdiff_t>(part + kTmdOff), img.begin() + static_cast<std::ptrdiff_t>(part + kTmdOff + kTmdSize));
}
} // namespace

TEST_CASE(rsa_known_answers_from_openssl) {
    // signatures made by `openssl dgst -sha1 -sign` with the test keys
    const auto digest = crypto::sha1(std::span<const u8>(reinterpret_cast<const u8*>(rsa_test_keys::kRsaKatMessage), sizeof rsa_test_keys::kRsaKatMessage - 1U));
    crypto::RsaPublicKey xs{std::vector<u8>(rsa_test_keys::kXsN.begin(), rsa_test_keys::kXsN.end()), 65537};
    crypto::RsaPublicKey root{std::vector<u8>(rsa_test_keys::kRootN.begin(), rsa_test_keys::kRootN.end()), 65537};
    CHECK(crypto::rsa_verify_sha1(xs, rsa_test_keys::kKatXsSig, digest));
    CHECK(crypto::rsa_verify_sha1(root, rsa_test_keys::kKatRootSig, digest));
    auto bad = std::vector<u8>(rsa_test_keys::kKatXsSig.begin(), rsa_test_keys::kKatXsSig.end());
    bad[100] ^= 0x01;
    CHECK(!crypto::rsa_verify_sha1(xs, bad, digest));
    auto other = digest;
    other[0] ^= 0x80;
    CHECK(!crypto::rsa_verify_sha1(xs, rsa_test_keys::kKatXsSig, other));
    CHECK(!crypto::rsa_verify_sha1(root, rsa_test_keys::kKatXsSig, digest)); // wrong length for the key
    // signing with the private exponent and verifying round-trips
    const auto em = crypto::pkcs1_sha1_encoding(digest, 256);
    std::vector<u8> sig;
    CHECK(crypto::rsa_raw(em, rsa_test_keys::kXsD, rsa_test_keys::kXsN, sig));
    CHECK(std::equal(sig.begin(), sig.end(), rsa_test_keys::kKatXsSig.begin())); // PKCS#1 v1.5 is deterministic
}

TEST_CASE(wii_partition_table_groups_and_types) {
    DiscSpec spec;
    PartitionSpec game;
    game.plain = plain_data(2);
    PartitionSpec update = game;
    update.type = 1;
    update.group = 1;
    update.plain = plain_data(1, 777);
    PartitionSpec channel = game;
    channel.type = 2;
    channel.group = 3;
    channel.plain = plain_data(1, 999);
    channel.title_id = 0x0001000148414A45ULL;
    spec.partitions = {update, game, channel}; // the game partition is not first
    const auto img = build_disc(spec);
    MemoryDisc disc(img);
    std::string err;
    const auto parts = list_wii_partitions(disc, err);
    CHECK(parts.has_value());
    if (!parts) { std::printf("    %s\n", err.c_str()); return; }
    CHECK_EQ(parts->size(), 3U);
    CHECK_EQ((*parts)[0].type, 0U); // group 0 is listed first
    CHECK_EQ((*parts)[0].group, 0U);
    CHECK_EQ((*parts)[1].type, 1U);
    CHECK_EQ((*parts)[1].group, 1U);
    CHECK_EQ((*parts)[2].type, 2U);
    CHECK_EQ((*parts)[2].group, 3U);
    CHECK(std::string(wii_partition_type_name(2)) == "channel");
    // every partition opens and reads its own data; reads stop at its end
    const std::vector<u8>* plains[] = {&game.plain, &update.plain, &channel.plain};
    for (std::size_t i = 0; i < 3; ++i) {
        auto p = WiiPartition::open(disc, (*parts)[i].offset, test_keys(), err);
        CHECK(p != nullptr);
        if (!p) { std::printf("    %s\n", err.c_str()); continue; }
        std::vector<u8> all(p->size());
        CHECK(p->read(0, all));
        CHECK(all == *plains[i]);
        std::vector<u8> across(0x100);
        CHECK(!p->read(p->size() - 0x80U, across)); // past the partition's end, not into the next one
        CHECK(contains(p->last_error(), "outside the partition"));
    }
    // malformed tables
    {
        auto bad = img;
        put32(bad, 0x40004, 0x7FFFFFFFU); // group 0 table far outside the image
        MemoryDisc d(bad);
        CHECK(!list_wii_partitions(d, err).has_value());
        CHECK(contains(err, "outside the image"));
    }
    {
        auto bad = img;
        put32(bad, 0x40000, 1000);
        MemoryDisc d(bad);
        CHECK(!list_wii_partitions(d, err).has_value());
        CHECK(contains(err, "implausible"));
    }
    {
        auto bad = img;
        put32(bad, 0x40020, 0x3FFFFFFFU); // partition offset (>> 2) past the end
        MemoryDisc d(bad);
        CHECK(!list_wii_partitions(d, err).has_value());
    }
    {
        std::vector<u8> tiny(0x40010, 0);
        MemoryDisc d(tiny);
        CHECK(!list_wii_partitions(d, err).has_value());
    }
}

TEST_CASE(wii_tickets_tmds_and_certificates) {
    const auto img = build_disc(plain_data(1));
    std::string err;
    const auto tbytes = ticket_of(img);
    const auto ticket = parse_ticket_checked(tbytes, err);
    CHECK(ticket.has_value());
    if (!ticket) { std::printf("    %s\n", err.c_str()); return; }
    CHECK_EQ(ticket->title_id, kTitleId);
    CHECK(ticket->sig.issuer == kXsIssuer);
    CHECK_EQ(ticket->common_key_index, 0U);
    CHECK_EQ(ticket->ticket_id, 0x0001020304050607ULL);
    // truncated, wrong type, wrong version, bad index, issuer
    CHECK(!parse_ticket_checked(std::span<const u8>(tbytes.data(), 0x2A0), err).has_value());
    CHECK(contains(err, "instead of 0x2A4"));
    { auto t = tbytes; put32(t, 0, kSigRsa4096); CHECK(!parse_ticket_checked(t, err).has_value()); CHECK(contains(err, "not RSA-2048")); }
    { auto t = tbytes; put32(t, 0, 0x12345678U); CHECK(!parse_ticket_checked(t, err).has_value()); CHECK(contains(err, "unknown signature type")); }
    { auto t = tbytes; t[0x1BC] = 1; CHECK(!parse_ticket_checked(t, err).has_value()); CHECK(contains(err, "version 1")); }
    { auto t = tbytes; t[0x1F1] = 3; CHECK(!parse_ticket_checked(t, err).has_value()); CHECK(contains(err, "unsupported common key index 3")); }
    { auto t = tbytes; t[0x140] = 'X'; CHECK(!parse_ticket_checked(t, err).has_value()); CHECK(contains(err, "issuer")); }

    const auto mbytes = tmd_of(img);
    const auto tmd = parse_tmd_checked(mbytes, err);
    CHECK(tmd.has_value());
    if (!tmd) { std::printf("    %s\n", err.c_str()); return; }
    CHECK_EQ(tmd->title_id, kTitleId);
    CHECK_EQ(tmd->ios_id, 0x0000000100000024ULL);
    CHECK_EQ(tmd->group_id, 0x3031U);
    CHECK_EQ(tmd->contents.size(), 1U);
    CHECK_EQ(tmd->contents[0].size, u64{kWiiH3Size});
    CHECK(!parse_tmd_checked(std::span<const u8>(mbytes.data(), 0x1E0), err).has_value());
    CHECK(!parse_tmd_checked(std::span<const u8>(mbytes.data(), 0x207), err).has_value()); // last content truncated
    CHECK(contains(err, "contents need"));
    { auto m = mbytes; put16(m, 0x1DE, 2); CHECK(!parse_tmd_checked(m, err).has_value()); }
    { auto m = mbytes; put16(m, 0x1DE, 0); CHECK(!parse_tmd_checked(m, err).has_value()); CHECK(contains(err, "no contents")); }
    { std::vector<u8> big(kTmdMaxSize + 4, 0); CHECK(!parse_tmd_checked(big, err).has_value()); }

    const auto chain = parse_cert_chain(test_cert_chain(), err);
    CHECK(chain.has_value());
    if (!chain) { std::printf("    %s\n", err.c_str()); return; }
    CHECK_EQ(chain->size(), 3U);
    CHECK((*chain)[0].name == "CA00000001" && (*chain)[0].sig.issuer == "Root" && (*chain)[0].bytes.size() == 0x400U);
    CHECK((*chain)[1].name == "CP00000004" && (*chain)[1].sig.issuer == "Root-CA00000001");
    CHECK((*chain)[2].name == "XS00000003" && (*chain)[2].key.modulus.size() == 0x100U && (*chain)[2].key.exponent == 65537U);
    auto cut = test_cert_chain();
    cut.resize(cut.size() - 0x10);
    CHECK(!parse_cert_chain(cut, err).has_value());
    CHECK(contains(err, "truncated"));
    auto junk = test_cert_chain();
    junk.insert(junk.end(), 0x20, 0xEE);
    CHECK(!parse_cert_chain(junk, err).has_value());
    CHECK(!parse_cert_chain(std::span<const u8>(), err).has_value());
}

TEST_CASE(wii_signature_chain) {
    DiscSpec spec;
    PartitionSpec p;
    p.plain = plain_data(1);
    p.sign = true;
    spec.partitions = {p};
    const auto signed_img = build_disc(spec);
    std::string err;
    {
        MemoryDisc disc(signed_img);
        auto part = WiiPartition::open(disc, kPart, test_keys(), err);
        CHECK(part != nullptr);
        if (!part) { std::printf("    %s\n", err.c_str()); return; }
        const auto root = test_root_key();
        const ChainReport full = part->verify_signatures(&root);
        CHECK(full.ticket == SignatureStatus::Valid);
        CHECK(full.tmd == SignatureStatus::Valid);
        CHECK(full.ticket_signer == SignatureStatus::Valid);
        CHECK(full.tmd_signer == SignatureStatus::Valid);
        CHECK(full.ca == SignatureStatus::Valid);
        CHECK(full.disc_chain_valid());
        const ChainReport no_root = part->verify_signatures(nullptr);
        CHECK(no_root.disc_chain_valid());
        CHECK(no_root.ca == SignatureStatus::NoRootKey);
        auto other_root = root;
        other_root.modulus[10] ^= 0x40;
        CHECK(part->verify_signatures(&other_root).ca == SignatureStatus::Invalid);
    }
    { // a modified TMD field breaks the TMD's signature only (the H3 hash is untouched)
        auto img = signed_img;
        img[kPart + kTmdOff + 0x19D] ^= 0x01; // region
        MemoryDisc disc(img);
        auto part = WiiPartition::open(disc, kPart, test_keys(), err);
        CHECK(part != nullptr);
        if (part) {
            const ChainReport r = part->verify_signatures(nullptr);
            CHECK(r.tmd == SignatureStatus::Invalid);
            CHECK(r.ticket == SignatureStatus::Valid);
            CHECK(!r.disc_chain_valid());
            CHECK(contains(r.detail, "INVALID"));
        }
    }
    { // fakesigned (zero signatures) images are reported as such and still readable
        MemoryDisc disc(build_disc(plain_data(1)));
        auto part = WiiPartition::open(disc, kPart, test_keys(), err);
        CHECK(part != nullptr);
        if (part) {
            const ChainReport r = part->verify_signatures(nullptr);
            CHECK(r.ticket == SignatureStatus::Fakesigned);
            CHECK(r.tmd == SignatureStatus::Fakesigned);
            CHECK(r.ticket_signer == SignatureStatus::Valid);
        }
    }
    { // root key file forms
        std::vector<u8> raw(rsa_test_keys::kRootN.begin(), rsa_test_keys::kRootN.end());
        CHECK(parse_root_key(raw, err).has_value());
        raw.insert(raw.end(), {0, 1, 0, 1});
        CHECK(parse_root_key(raw, err).has_value());
        CHECK(!parse_root_key(std::vector<u8>(100, 1), err).has_value());
    }
}

TEST_CASE(wii_each_partition_uses_its_own_ticket_and_title_key) {
    // three partitions: different title IDs, title keys and common key indexes
    DiscSpec spec;
    PartitionSpec a, b, c;
    a.plain = plain_data(2, 1);
    b.plain = plain_data(2, 2);
    b.type = 1;
    b.title_id = 0x0000000100000002ULL;
    b.title_key = AesKey{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    c.plain = plain_data(2, 3);
    c.type = 2;
    c.title_id = 0x0001000148414A45ULL;
    c.common_index = 1;
    c.common = AesKey{0x5A, 0x5A, 0x5A, 0x5A, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    c.title_key = AesKey{0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F};
    spec.partitions = {a, b, c};
    const auto img = build_disc(spec);
    MemoryDisc disc(img);
    std::string err;
    const auto parts = list_wii_partitions(disc, err);
    CHECK(parts.has_value() && parts->size() == 3U);
    if (!parts || parts->size() != 3U) return;
    WiiKeys keys = test_keys();
    keys.common[1] = c.common;
    const PartitionSpec* specs[] = {&a, &b, &c};
    std::vector<std::unique_ptr<WiiPartition>> open;
    for (std::size_t i = 0; i < 3; ++i) {
        auto p = WiiPartition::open(disc, (*parts)[i].offset, keys, err);
        CHECK(p != nullptr);
        if (!p) { std::printf("    %s\n", err.c_str()); return; }
        CHECK_EQ(p->ticket_info().title_id, specs[i]->title_id);
        CHECK_EQ(p->ticket_info().common_key_index, specs[i]->common_index);
        CHECK(p->title_key() == specs[i]->title_key);
        open.push_back(std::move(p));
    }
    // all three open at once, reads interleaved: each one decrypts with its own key
    for (int round = 0; round < 3; ++round)
        for (std::size_t i = 0; i < 3; ++i) {
            std::vector<u8> piece(0x9000);
            const u64 at = 0x1234U * static_cast<u64>(round);
            CHECK(open[i]->read(at, piece));
            CHECK(std::equal(piece.begin(), piece.end(), specs[i]->plain.begin() + static_cast<std::ptrdiff_t>(at)));
        }
    // partition c without the Korean key: only c fails, naming the key
    WiiKeys normal_only = test_keys();
    CHECK(WiiPartition::open(disc, (*parts)[0].offset, normal_only, err) != nullptr);
    CHECK(WiiPartition::open(disc, (*parts)[2].offset, normal_only, err) == nullptr);
    CHECK(contains(err, "Korean"));
    // b's title key cannot read a's clusters: a's ticket with b's encrypted key fails the hash check
    {
        auto swapped = img;
        std::memcpy(&swapped[(*parts)[0].offset + 0x1BF], &img[(*parts)[1].offset + 0x1BF], 16);
        std::memcpy(&swapped[(*parts)[0].offset + 0x1DC], &img[(*parts)[1].offset + 0x1DC], 8); // and its title ID (IV)
        std::memcpy(&swapped[(*parts)[0].offset + kTmdOff + 0x18C], &img[(*parts)[1].offset + 0x1DC], 8);
        MemoryDisc d(swapped);
        CHECK(WiiPartition::open(d, (*parts)[0].offset, keys, err) == nullptr);
        CHECK(contains(err, "H0"));
    }
}

TEST_CASE(wii_title_key_iv_and_keys) {
    auto img = build_disc(plain_data(3));
    std::string err;
    const WiiTicket t = parse_ticket(ticket_of(img));
    CHECK_EQ(t.title_id, kTitleId);
    CHECK(t.issuer == kXsIssuer);
    const auto key = decrypt_title_key(t, test_keys(), err);
    CHECK(key.has_value());
    CHECK(key.has_value() && *key == kTitleKey);
    // the IV is the title ID: with another title ID the same bytes give another key
    WiiTicket moved = t;
    moved.title_id ^= 1U;
    const auto wrong = decrypt_title_key(moved, test_keys(), err);
    CHECK(wrong.has_value() && *wrong != kTitleKey);
    // missing keys: explicit errors that name the key
    WiiKeys none;
    CHECK(!decrypt_title_key(t, none, err).has_value());
    CHECK(contains(err, "the common key (index 0)"));
    CHECK(contains(err, "not provided"));
    WiiTicket korean = t;
    korean.common_key_index = 1;
    CHECK(!decrypt_title_key(korean, test_keys(), err).has_value());
    CHECK(contains(err, "Korean"));
    WiiTicket bad = t;
    bad.common_key_index = 7;
    CHECK(!decrypt_title_key(bad, test_keys(), err).has_value());
    CHECK(contains(err, "unsupported common key index 7"));
    { // development tickets use the development key
        DiscSpec spec;
        PartitionSpec p;
        p.plain = plain_data(1);
        p.ticket_issuer = "Root-CA00000002-XS00000006";
        p.common = AesKey{9, 8, 7, 6, 5, 4, 3, 2, 1, 0, 1, 2, 3, 4, 5, 6};
        spec.partitions = {p};
        MemoryDisc disc(build_disc(spec));
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "development common key"));
        WiiKeys dev;
        dev.dev_common = p.common;
        CHECK(WiiPartition::open(disc, kPart, dev, err) != nullptr);
    }
    { // Korean key index selects common[1]
        MemoryDisc disc(build_disc(plain_data(1), 1));
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        WiiKeys k;
        k.common[1] = kTestCommon;
        auto p = WiiPartition::open(disc, kPart, k, err);
        CHECK(p != nullptr);
        std::vector<u8> b(0x7C00);
        if (p) CHECK(p->read(0, b));
    }
    { // a wrong common key is caught when opening, with the reason
        WiiKeys k;
        AesKey other = kTestCommon;
        other[0] ^= 1;
        k.common[0] = other;
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, k, err) == nullptr);
        CHECK(contains(err, "common key is wrong"));
    }
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
    CHECK_EQ(p->data_offset(), kPart + kDataOff);
    CHECK_EQ(p->data_size(), u64{70} * kWiiClusterSize);
    CHECK(p->flags().hashes && p->flags().encryption);
    std::vector<u8> all(p->size());
    CHECK(p->read(0, all));
    CHECK(all == plain);
    CHECK(p->verify_all());
    // unaligned reads across cluster and group boundaries, odd sizes
    for (const u64 at : {u64{63} * kWiiClusterData - 0x123U, u64{kWiiClusterData} - 1U, u64{0x3FF}, u64{64} * kWiiClusterData}) {
        for (const std::size_t n : {std::size_t{1}, std::size_t{0x401}, std::size_t{0x9000}}) {
            std::vector<u8> piece(n);
            CHECK(p->read(at, piece));
            CHECK(std::equal(piece.begin(), piece.end(), plain.begin() + static_cast<std::ptrdiff_t>(at)));
        }
    }
    std::vector<u8> past(16);
    CHECK(!p->read(p->size() - 8U, past));
    CHECK(!p->read(~u64{0} - 4U, past)); // no overflow
    std::vector<u8> none;
    CHECK(p->read(p->size(), none));
}

TEST_CASE(wii_cluster_cache) {
    const auto plain = plain_data(12);
    MemoryDisc disc(build_disc(plain));
    std::string err;
    auto p = WiiPartition::open(disc, kPart, test_keys(), err);
    CHECK(p != nullptr);
    if (!p) return;
    const u64 base = p->clusters_decrypted(); // cluster 0 was checked when opening
    CHECK_EQ(base, 1U);
    std::vector<u8> b(0x100);
    for (int i = 0; i < 50; ++i) CHECK(p->read(0x200, b)); // repeated reads: no new decryption
    CHECK_EQ(p->clusters_decrypted(), base);
    CHECK(p->cache_hits() >= 50U);
    std::vector<u8> span2(2 * kWiiClusterData);
    CHECK(p->read(kWiiClusterData / 2U, span2)); // clusters 0, 1, 2
    CHECK_EQ(p->clusters_decrypted(), base + 2U);
    for (u64 c = 0; c < 12; ++c) CHECK(p->read(c * kWiiClusterData, b)); // more clusters than the cache holds
    CHECK_EQ(p->clusters_decrypted(), base + 2U + 9U);
    const u64 now = p->clusters_decrypted();
    CHECK(p->read(11U * kWiiClusterData, b)); // most recent: still cached
    CHECK_EQ(p->clusters_decrypted(), now);
    CHECK(p->read(0, b)); // evicted: decrypted again, same data
    CHECK_EQ(p->clusters_decrypted(), now + 1U);
    CHECK(std::equal(b.begin(), b.end(), plain.begin()));
}

TEST_CASE(wii_partition_rejects_tampering_at_every_level) {
    const auto plain = plain_data(9);
    const auto good = build_disc(plain);
    std::string err;
    struct Case { std::size_t at; const char* expect; };
    const Case cases[] = {
        {kPart + kDataOff + 8 * kWiiClusterSize + 0x400 + 0x1234, "H0"}, // data of cluster 8
        {kPart + kDataOff + 8 * kWiiClusterSize + 0x10, "H0"},           // H0 table of cluster 8
        {kPart + kDataOff + 8 * kWiiClusterSize + 0x290, "H1"},          // H1 list in cluster 8
        {kPart + kDataOff + 8 * kWiiClusterSize + 0x350, "H2"},          // H2 list in cluster 8
    };
    for (const Case& c : cases) {
        auto img = good;
        img[c.at] ^= 0x01;
        MemoryDisc disc(img);
        auto p = WiiPartition::open(disc, kPart, test_keys(), err);
        CHECK(p != nullptr);
        if (!p) continue;
        std::vector<u8> first(kWiiClusterData);
        CHECK(p->read(0, first)); // other clusters are fine
        std::vector<u8> all(p->size());
        CHECK(!p->read(0, all)); // nothing of a failing cluster is handed out
        CHECK(contains(p->last_error(), c.expect));
        CHECK(contains(p->last_error(), "cluster 8"));
        CHECK(!p->verify_all());
    }
    { // corruption in cluster 0 is found when opening
        auto img = good;
        img[kPart + kDataOff + 0x500] ^= 0x01;
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "H0"));
    }
    { // H3 table no longer matches the TMD
        auto img = good;
        img[kPart + kH3Off + 3] ^= 0x80;
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "H3"));
    }
    { // ... and an H3 entry that matches a modified TMD hash still fails H2 -> H3
        auto img = good;
        img[kPart + kH3Off + 3] ^= 0x80;
        std::vector<u8> h3(img.begin() + static_cast<std::ptrdiff_t>(kPart + kH3Off), img.begin() + static_cast<std::ptrdiff_t>(kPart + kH3Off + kWiiH3Size));
        const auto hh = crypto::sha1(h3);
        std::memcpy(&img[kPart + kTmdOff + 0x1E4 + 0x10], hh.data(), 20);
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "H3"));
    }
    { // a disc partition's TMD must list exactly one content (the H3 table), as Dolphin checks
        auto img = good;
        put16(img, kPart + kTmdOff + 0x1DE, 2);
        put32(img, kPart + 0x2A4, kTmdSize + 0x24);
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "instead of one"));
    }
    { // TMD and ticket title IDs differ
        auto img = good;
        img[kPart + kTmdOff + 0x193] ^= 1;
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "title ID"));
    }
}

TEST_CASE(wii_partition_header_offsets_are_checked) {
    const auto good = build_disc(plain_data(2));
    std::string err;
    struct Case { std::size_t field; u32 value; const char* expect; };
    const Case cases[] = {
        {0x2A4, 0x100, "TMD size"},                    // TMD smaller than its header
        {0x2A8, 0x7FFFFFFF, "TMD at"},                 // TMD offset outside
        {0x2AC, 0, "certificate chain size"},
        {0x2B0, 0x7FFFFFFF, "certificate chain at"},
        {0x2B4, 0x7FFFFFFF, "H3 table at"},
        {0x2B8, 0x7FFFFFF0, "past the end"},           // data offset outside
        {0x2BC, (kWiiClusterSize + 0x100) >> 2, "whole number"},
        {0x2B0, (kTmdOff + 0x10) >> 2, "certificate chain"}, // chain read from the wrong place
    };
    for (const Case& c : cases) {
        auto img = good;
        put32(img, kPart + c.field, c.value);
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, c.expect));
    }
    { // truncated image: the data area is cut short; the clusters still present read,
      // the missing one is reported (as in Dolphin, which opens trimmed images)
        auto img = good;
        img.resize(img.size() - 0x1000);
        MemoryDisc disc(img);
        auto p = WiiPartition::open(disc, kPart, test_keys(), err);
        CHECK(p != nullptr);
        if (p) {
            std::vector<u8> first(kWiiClusterData);
            CHECK(p->read(0, first));
            std::vector<u8> last(16);
            CHECK(!p->read(p->size() - 16U, last));
            CHECK(contains(p->last_error(), "cluster 1"));
            CHECK(contains(p->last_error(), "truncated or trimmed"));
        }
        img.resize(kPart + kDataOff); // no data at all
        MemoryDisc none(img);
        CHECK(WiiPartition::open(none, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "truncated image"));
    }
    { // a partition offset in the middle of nowhere
        MemoryDisc disc(good);
        CHECK(WiiPartition::open(disc, good.size() - 0x100U, test_keys(), err) == nullptr);
        CHECK(WiiPartition::open(disc, ~u64{0} - 0x10U, test_keys(), err) == nullptr);
    }
}

TEST_CASE(wii_discs_without_encryption_or_hashes) {
    const auto plain = plain_data(5);
    std::string err;
    { // hashed, unencrypted: no key needed, hashes still verified
        DiscSpec spec;
        spec.encryption = false;
        spec.partitions.push_back({});
        spec.partitions[0].plain = plain;
        auto img = build_disc(spec);
        MemoryDisc disc(img);
        WiiKeys none;
        auto p = WiiPartition::open(disc, kPart, none, err);
        CHECK(p != nullptr);
        if (p) {
            CHECK(!p->flags().encryption);
            std::vector<u8> all(p->size());
            CHECK(p->read(0, all));
            CHECK(all == plain);
        }
        img[kPart + kDataOff + 3U * kWiiClusterSize + 0x400 + 7] ^= 1;
        MemoryDisc bad(img);
        auto q = WiiPartition::open(bad, kPart, none, err);
        CHECK(q != nullptr);
        std::vector<u8> all(plain.size());
        if (q) { CHECK(!q->read(0, all)); CHECK(contains(q->last_error(), "H0")); }
    }
    { // no hashes, no encryption: contiguous data
        DiscSpec spec;
        spec.encryption = false;
        spec.hashes = false;
        spec.partitions.push_back({});
        spec.partitions[0].plain = plain;
        MemoryDisc disc(build_disc(spec));
        auto p = WiiPartition::open(disc, kPart, WiiKeys{}, err);
        CHECK(p != nullptr);
        if (p) {
            CHECK_EQ(p->size(), plain.size());
            std::vector<u8> piece(0x9000);
            CHECK(p->read(0x7BF0, piece));
            CHECK(std::equal(piece.begin(), piece.end(), plain.begin() + 0x7BF0));
            CHECK(!p->verify_all());
        }
    }
    { // encrypted without hashes: refused with the reason (as Dolphin warns)
        auto img = build_disc(plain);
        img[0x60] = 1;
        MemoryDisc disc(img);
        CHECK(WiiPartition::open(disc, kPart, test_keys(), err) == nullptr);
        CHECK(contains(err, "encrypted but carry no hashes"));
    }
}

TEST_CASE(wii_key_files) {
    const std::string raw = "/tmp/orca_test_common.key", bootmii = "/tmp/orca_test_keys.bin", bad = "/tmp/orca_test_bad.key";
    const std::string text = "/tmp/orca_test_keys.txt", root = "/tmp/orca_test_root.bin";
    { std::ofstream f(raw, std::ios::binary); f.write(reinterpret_cast<const char*>(kTestCommon.data()), 16); }
    {
        std::vector<char> kb(0x400, 0);
        const char header[] = "BackupMii v1, ConsoleID: 0badc0de\n";
        std::memcpy(kb.data(), header, sizeof header - 1U);
        std::memcpy(&kb[0x114], kTestCommon.data(), 16);
        std::ofstream f(bootmii, std::ios::binary);
        f.write(kb.data(), static_cast<std::streamsize>(kb.size()));
    }
    { std::ofstream f(bad, std::ios::binary); f << "short"; }
    { std::ofstream f(root, std::ios::binary); f.write(reinterpret_cast<const char*>(rsa_test_keys::kRootN.data()), 512); }
    {
        std::ofstream f(text);
        f << "# keys from my own console\n"
             "common_key = 1032547698BADCFE0123456789ABCDEF\n"
             "  korean_common_key=00112233445566778899aabbccddeeff  # comment\n"
             "dev_common_key = FFEEDDCCBBAA99887766554433221100\n"
             "root_key_file = " << root << "\n";
    }
    WiiKeys k;
    std::string err;
    CHECK(load_wii_key_file(raw, k, 1, err));
    CHECK(k.common[1].has_value() && *k.common[1] == kTestCommon);
    CHECK(load_wii_key_file(raw, k, 3, err));
    CHECK(k.dev_common.has_value() && *k.dev_common == kTestCommon);
    CHECK(!load_wii_key_file(raw, k, 4, err));
    CHECK(load_wii_key_file(bootmii, k, 0, err));
    CHECK(k.common[0].has_value() && *k.common[0] == kTestCommon);
    CHECK(!load_wii_key_file(bad, k, 0, err));
    CHECK(!load_wii_key_file("/tmp/does/not/exist", k, 0, err));
    WiiKeys t;
    CHECK(load_wii_key_file(text, t, 0, err));
    if (!err.empty() && !t.common[0]) std::printf("    %s\n", err.c_str());
    CHECK(t.common[0].has_value() && *t.common[0] == kTestCommon);
    CHECK(t.common[1].has_value() && (*t.common[1])[15] == 0xFF);
    CHECK(!t.common[2].has_value());
    CHECK(t.dev_common.has_value() && (*t.dev_common)[0] == 0xFF);
    CHECK(t.root.has_value() && t.root->modulus.size() == 512U);
    const auto parse = [&](const char* s) { WiiKeys x; return parse_wii_keys(std::span<const u8>(reinterpret_cast<const u8*>(s), std::strlen(s)), x, 0, err); };
    CHECK(!parse("common_key = 1234\n"));
    CHECK(contains(err, "32 hex digits"));
    CHECK(!parse("secret = 1032547698BADCFE0123456789ABCDEF\n"));
    CHECK(contains(err, "unknown name"));
    CHECK(!parse("# nothing here\n"));
    CHECK(contains(err, "no keys"));
    CHECK(!parse("common_key 1032547698BADCFE0123456789ABCDEF"));
    std::vector<u8> empty_bootmii(0x400, 0);
    std::memcpy(empty_bootmii.data(), "BackupMii v1", 12);
    WiiKeys x;
    CHECK(!parse_wii_keys(empty_bootmii, x, 0, err));
    CHECK(contains(err, "empty"));
    std::vector<u8> other_1k(0x400, 0x11); // 1024 bytes that are not a BootMii dump
    CHECK(!parse_wii_keys(other_1k, x, 0, err));
    CHECK(contains(err, "BackupMii"));
    CHECK(!x.common[0].has_value());
}

TEST_MAIN("wii_disc")
