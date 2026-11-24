// Synthetic Wii disc for tests: a game partition (type 0) at 0x50000 and,
// optionally, more partitions (other types, other groups); each with its ticket
// (title key encrypted with a test common key), TMD (IOS, group, the H3 table
// hash), certificate chain (CA, CP, XS made with the test keys of
// wii_test_keys.hpp), H3 table and encrypted, hashed clusters. The real common
// keys are not in Orca. Tickets and TMDs are fakesigned (zero signatures) unless
// `sign` is set, which signs them with the test XS/CP keys (slow under sanitizers).
#pragma once
#include "orca/crypto/aes.hpp"
#include "orca/crypto/rsa.hpp"
#include "orca/crypto/sha1.hpp"
#include "orca/disc/wii_disc.hpp"
#include "wii_test_keys.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace orca::test_wii {
using namespace orca::disc;

inline void put32(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); }
inline void put16(std::vector<u8>& d, std::size_t at, u16 v) { d[at] = static_cast<u8>(v >> 8); d[at + 1] = static_cast<u8>(v); }
inline void put64(std::vector<u8>& d, std::size_t at, u64 v) { put32(d, at, static_cast<u32>(v >> 32)); put32(d, at + 4, static_cast<u32>(v)); }
inline void put_str(std::vector<u8>& d, std::size_t at, const std::string& s) { std::memcpy(&d[at], s.data(), s.size()); }

constexpr AesKey kTestCommon{0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr AesKey kTitleKey{0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3, 0xD0, 0xD1, 0xD2, 0xD3};
constexpr u64 kTitleId = 0x0001000052534245ULL; // "RSBE"-like game title
constexpr u64 kPart = 0x50000;
constexpr u32 kTmdOff = 0x2C0, kTmdSize = 0x208, kCertOff = 0x4C8, kCertSize = 0xA00, kH3Off = 0x8000, kDataOff = 0x20000;
inline const std::string kXsIssuer = "Root-CA00000001-XS00000003", kCpIssuer = "Root-CA00000001-CP00000004";

// Plain partition data (deterministic), `clusters` x 0x7C00 bytes.
inline std::vector<u8> plain_data(u64 clusters, u32 seed = 12345) {
    std::vector<u8> d(clusters * kWiiClusterData);
    u32 x = seed;
    for (auto& b : d) { x = x * 1103515245U + 12345U; b = static_cast<u8>(x >> 16); }
    const char* id = "RSBE01";
    std::memcpy(d.data(), id, 6);
    put32(d, 0x18, kWiiMagic);
    return d;
}

// Signs blob[issuer_offset..] with a test key (PKCS#1 v1.5, SHA-1) into the signature field.
template <std::size_t N>
inline void sign_blob(std::vector<u8>& blob, std::size_t sig_len, std::size_t issuer_offset, const std::array<u8, N>& n, const std::array<u8, N>& d) {
    const auto digest = crypto::sha1(std::span<const u8>(blob.data() + issuer_offset, blob.size() - issuer_offset));
    const auto em = crypto::pkcs1_sha1_encoding(digest, N);
    std::vector<u8> s;
    (void)crypto::rsa_raw(em, d, n, s);
    (void)sig_len;
    std::memcpy(&blob[4], s.data(), s.size());
}

// One certificate with an RSA-2048 key, signed with RSA-4096 (by the root) or RSA-2048.
template <std::size_t NS>
inline std::vector<u8> make_cert(bool rsa4096, const std::string& issuer, const std::string& name, const std::array<u8, 256>& key,
                                 const std::array<u8, NS>& sn, const std::array<u8, NS>& sd) {
    const std::size_t io = rsa4096 ? 0x240 : 0x140;
    std::vector<u8> c(rsa4096 ? 0x400 : 0x300, 0);
    put32(c, 0, rsa4096 ? kSigRsa4096 : kSigRsa2048);
    put_str(c, io, issuer);
    put32(c, io + 0x40, 1); // RSA-2048 key
    put_str(c, io + 0x44, name);
    std::memcpy(&c[io + 0x88], key.data(), 0x100);
    put32(c, io + 0x188, 65537);
    sign_blob(c, rsa4096 ? 0x200 : 0x100, io, sn, sd);
    return c;
}

// CA (signed by the test root), CP and XS (signed by the CA): made once.
inline const std::vector<u8>& test_cert_chain() {
    static const std::vector<u8> chain = [] {
        using namespace orca::rsa_test_keys;
        std::vector<u8> out = make_cert(true, "Root", "CA00000001", kCaN, kRootN, kRootD);
        const auto cp = make_cert(false, "Root-CA00000001", "CP00000004", kCpN, kCaN, kCaD);
        const auto xs = make_cert(false, "Root-CA00000001", "XS00000003", kXsN, kCaN, kCaD);
        out.insert(out.end(), cp.begin(), cp.end());
        out.insert(out.end(), xs.begin(), xs.end());
        return out;
    }();
    return chain;
}

inline crypto::RsaPublicKey test_root_key() { return {std::vector<u8>(rsa_test_keys::kRootN.begin(), rsa_test_keys::kRootN.end()), 65537}; }

struct PartitionSpec {
    std::vector<u8> plain;          // whole clusters of 0x7C00 bytes
    u32 type{0};                    // 0 game, 1 update, 2 channel
    unsigned group{0};
    unsigned common_index{0};
    u64 title_id{kTitleId};
    u32 ios{36};
    u16 gid{0x3031};
    bool sign{false};
    std::string ticket_issuer{kXsIssuer};
    AesKey common{kTestCommon};
};

struct DiscSpec {
    std::vector<PartitionSpec> partitions;
    bool hashes{true};
    bool encryption{true};
};

// Each partition gets its own 0x20000 + data area, one after another from 0x50000.
inline std::vector<u8> build_disc(const DiscSpec& spec) {
    u64 total = kPart;
    std::vector<u64> where;
    for (const PartitionSpec& p : spec.partitions) {
        const u64 clusters = p.plain.size() / kWiiClusterData;
        where.push_back(total);
        total += kDataOff + (spec.hashes ? clusters * kWiiClusterSize : p.plain.size());
        total = (total + 0x7FFFU) & ~u64{0x7FFF};
    }
    std::vector<u8> disc(total, 0);
    const std::vector<u8>& first = spec.partitions.at(0).plain;
    std::memcpy(disc.data(), first.data(), 6);
    put32(disc, 0x18, kWiiMagic);
    disc[0x60] = spec.hashes ? 0 : 1;
    disc[0x61] = spec.encryption ? 0 : 1;
    // partition tables: group g's table at 0x40020 + 0x100 * g
    for (unsigned g = 0; g < 4; ++g) {
        u32 count = 0;
        for (std::size_t i = 0; i < spec.partitions.size(); ++i) {
            if (spec.partitions[i].group != g) continue;
            const std::size_t e = 0x40020U + 0x100U * g + 8U * count;
            put32(disc, e, static_cast<u32>(where[i] >> 2));
            put32(disc, e + 4, spec.partitions[i].type);
            ++count;
        }
        if (count == 0U) continue;
        put32(disc, 0x40000U + 8U * g, count);
        put32(disc, 0x40004U + 8U * g, (0x40020U + 0x100U * g) >> 2);
    }
    for (std::size_t pi = 0; pi < spec.partitions.size(); ++pi) {
        const PartitionSpec& ps = spec.partitions[pi];
        const u64 part = where[pi];
        const std::vector<u8>& plain = ps.plain;
        const u64 clusters = plain.size() / kWiiClusterData;
        // ticket
        std::vector<u8> ticket(kTicketSize, 0);
        put32(ticket, 0, kSigRsa2048);
        put_str(ticket, 0x140, ps.ticket_issuer);
        std::vector<u8> enc(kTitleKey.begin(), kTitleKey.end());
        crypto::Aes128 common(ps.common);
        crypto::Aes128::Block iv{};
        for (unsigned k = 0; k < 8; ++k) iv[k] = static_cast<u8>(ps.title_id >> (56U - 8U * k));
        (void)common.cbc_encrypt(enc, iv);
        std::memcpy(&ticket[0x1BF], enc.data(), 16);
        put64(ticket, 0x1D0, 0x0001020304050607ULL);
        put64(ticket, 0x1DC, ps.title_id);
        ticket[0x1F1] = static_cast<u8>(ps.common_index);
        if (ps.sign) sign_blob(ticket, 0x100, 0x140, rsa_test_keys::kXsN, rsa_test_keys::kXsD);
        std::memcpy(&disc[part], ticket.data(), ticket.size());
        put32(disc, part + 0x2A4, kTmdSize);
        put32(disc, part + 0x2A8, kTmdOff >> 2);
        put32(disc, part + 0x2AC, kCertSize);
        put32(disc, part + 0x2B0, kCertOff >> 2);
        put32(disc, part + 0x2B4, kH3Off >> 2);
        put32(disc, part + 0x2B8, kDataOff >> 2);
        put32(disc, part + 0x2BC, static_cast<u32>((spec.hashes ? clusters * kWiiClusterSize : plain.size()) >> 2));
        std::memcpy(&disc[part + kCertOff], test_cert_chain().data(), kCertSize);
        std::vector<u8> h3(kWiiH3Size, 0);
        if (!spec.hashes) {
            std::memcpy(&disc[part + kDataOff], plain.data(), plain.size());
        } else {
            crypto::Aes128 tk(kTitleKey);
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
                    u8* out = &disc[part + kDataOff + ci * kWiiClusterSize];
                    std::vector<u8> hashes = hb[c];
                    std::vector<u8> data(plain.begin() + static_cast<std::ptrdiff_t>(ci * kWiiClusterData),
                                         plain.begin() + static_cast<std::ptrdiff_t>((ci + 1U) * kWiiClusterData));
                    if (spec.encryption) {
                        crypto::Aes128::Block z{};
                        (void)tk.cbc_encrypt(hashes, z);
                        crypto::Aes128::Block div{};
                        std::memcpy(div.data(), &hashes[0x3D0], 16);
                        (void)tk.cbc_encrypt(data, div);
                    }
                    std::memcpy(out, hashes.data(), 0x400);
                    std::memcpy(out + 0x400, data.data(), kWiiClusterData);
                }
            }
            std::memcpy(&disc[part + kH3Off], h3.data(), kWiiH3Size);
        }
        // TMD: one content, the H3 table
        std::vector<u8> tmd(kTmdSize, 0);
        put32(tmd, 0, kSigRsa2048);
        put_str(tmd, 0x140, kCpIssuer);
        put32(tmd, 0x184, 1); // IOS title 00000001-<ios>
        put32(tmd, 0x188, ps.ios);
        put64(tmd, 0x18C, ps.title_id);
        put16(tmd, 0x198, ps.gid);
        put16(tmd, 0x1DE, 1);
        put16(tmd, 0x1E4 + 6, 3);
        put64(tmd, 0x1E4 + 8, kWiiH3Size);
        const auto hh = crypto::sha1(h3);
        std::memcpy(&tmd[0x1E4 + 0x10], hh.data(), 20);
        if (ps.sign) sign_blob(tmd, 0x100, 0x140, rsa_test_keys::kCpN, rsa_test_keys::kCpD);
        std::memcpy(&disc[part + kTmdOff], tmd.data(), kTmdSize);
    }
    return disc;
}

inline std::vector<u8> build_disc(const std::vector<u8>& plain, unsigned common_index = 0, u32 ios = 36, u16 group = 0x3031) {
    DiscSpec s;
    PartitionSpec p;
    p.plain = plain;
    p.common_index = common_index;
    p.ios = ios;
    p.gid = group;
    s.partitions.push_back(std::move(p));
    return build_disc(s);
}

inline WiiKeys test_keys() { WiiKeys k; k.common[0] = kTestCommon; return k; }

} // namespace orca::test_wii
