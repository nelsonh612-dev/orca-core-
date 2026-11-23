// Orca Core — Wii discs. See wii_disc.hpp.
#include "orca/disc/wii_disc.hpp"

#include "orca/crypto/aes.hpp"
#include "orca/crypto/sha1.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

namespace orca::disc {

namespace {
u32 be32(const u8* p) noexcept { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
u64 be64(const u8* p) noexcept { return u64{be32(p)} << 32 | be32(p + 4); }
u16 be16(const u8* p) noexcept { return static_cast<u16>(p[0] << 8 | p[1]); }
bool read_at(DiscReader& d, u64 off, std::span<u8> out) { return d.read(off, out); }
} // namespace

bool load_wii_key_file(const std::string& path, WiiKeys& keys, unsigned index, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open key file " + path; return false; }
    const std::vector<u8> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    AesKey k{};
    if (data.size() == 16U) {
        if (index >= keys.common.size()) { error = "common key index out of range"; return false; }
        std::copy(data.begin(), data.end(), k.begin());
        keys.common[index] = k;
        return true;
    }
    if (data.size() == 0x400U) { // BootMii keys.bin: header (0x100) + OTP (common key at OTP 0x14)
        std::copy_n(data.begin() + 0x114, 16, k.begin());
        keys.common[0] = k;
        return true;
    }
    error = "key file must be 16 bytes (a common key) or 1024 bytes (BootMii keys.bin)";
    return false;
}

std::optional<std::vector<WiiPartitionEntry>> list_wii_partitions(DiscReader& disc, std::string& error) {
    u8 hdr[0x20];
    if (!read_at(disc, 0x40000U, hdr)) { error = "disc too small for the Wii partition table"; return std::nullopt; }
    std::vector<WiiPartitionEntry> out;
    for (unsigned g = 0; g < 4; ++g) {
        const u32 count = be32(hdr + 8 * g);
        const u64 table = u64{be32(hdr + 8 * g + 4)} << 2;
        if (count > 64U) { error = "implausible partition count"; return std::nullopt; }
        for (u32 k = 0; k < count; ++k) {
            u8 e[8];
            if (!read_at(disc, table + 8U * k, e)) { error = "partition table outside the disc"; return std::nullopt; }
            out.push_back(WiiPartitionEntry{u64{be32(e)} << 2, be32(e + 4), g});
        }
    }
    return out;
}

WiiTicket parse_ticket(std::span<const u8> t) {
    WiiTicket r;
    std::copy_n(t.begin() + 0x1BF, 16, r.encrypted_title_key.begin());
    r.title_id = be64(t.data() + 0x1DC);
    r.common_key_index = t[0x1F1];
    return r;
}

std::optional<AesKey> decrypt_title_key(const WiiTicket& ticket, const WiiKeys& keys, std::string& error) {
    if (ticket.common_key_index >= keys.common.size() || !keys.common[ticket.common_key_index]) {
        error = "the common key with index " + std::to_string(ticket.common_key_index) +
                " is not loaded (it must be provided by the user)";
        return std::nullopt;
    }
    crypto::Aes128 aes(*keys.common[ticket.common_key_index]);
    crypto::Aes128::Block iv{};
    for (unsigned k = 0; k < 8; ++k) iv[k] = static_cast<u8>(ticket.title_id >> (56U - 8U * k));
    AesKey key = ticket.encrypted_title_key;
    (void)aes.cbc_decrypt(key, iv);
    return key;
}

std::unique_ptr<WiiPartition> WiiPartition::open(DiscReader& disc, u64 po, const WiiKeys& keys, std::string& error) {
    std::unique_ptr<WiiPartition> p(new WiiPartition(disc));
    std::vector<u8> head(0x2C0);
    if (!read_at(disc, po, head)) { error = "partition header outside the disc"; return nullptr; }
    p->ticket_ = parse_ticket(head);
    const auto key = decrypt_title_key(p->ticket_, keys, error);
    if (!key) return nullptr;
    p->title_key_ = *key;
    const u32 tmd_size = be32(&head[0x2A4]), cert_size = be32(&head[0x2AC]);
    const u64 tmd_off = u64{be32(&head[0x2A8])} << 2, cert_off = u64{be32(&head[0x2B0])} << 2;
    const u64 h3_off = u64{be32(&head[0x2B4])} << 2, data_off = u64{be32(&head[0x2B8])} << 2;
    const u64 data_size = u64{be32(&head[0x2BC])} << 2;
    if (tmd_size < 0x1E4U + 0x24U || tmd_size > 0x10000U || cert_size > 0x10000U) { error = "implausible TMD or certificate size"; return nullptr; }
    p->tmd_.resize(tmd_size);
    p->certs_.resize(cert_size);
    p->h3_.resize(kWiiH3Size);
    if (!read_at(disc, po + tmd_off, p->tmd_) || !read_at(disc, po + cert_off, p->certs_) || !read_at(disc, po + h3_off, p->h3_)) {
        error = "TMD, certificates or H3 table outside the disc";
        return nullptr;
    }
    // The TMD's first content describes the H3 table (signatures are not checked:
    // that would need Nintendo's public keys and is not needed to read the data).
    if (be16(&p->tmd_[0x1DE]) == 0U) { error = "TMD without contents"; return nullptr; }
    const auto h3_hash = crypto::sha1(p->h3_);
    if (!std::equal(h3_hash.begin(), h3_hash.end(), p->tmd_.begin() + 0x1E4 + 0x10)) {
        error = "H3 table does not match the TMD content hash";
        return nullptr;
    }
    if (data_size % kWiiClusterSize != 0U) { error = "partition data size is not a whole number of clusters"; return nullptr; }
    p->partition_offset_ = po;
    p->data_offset_ = po + data_off;
    p->clusters_ = data_size / kWiiClusterSize;
    if (p->clusters_ > u64{kWiiH3Size / 20U} * 64U) { error = "partition larger than its H3 table covers"; return nullptr; }
    return p;
}

bool WiiPartition::load_cluster(u64 index) {
    if (index == cached_) return true;
    std::vector<u8> raw(kWiiClusterSize);
    if (!read_at(disc_, data_offset_ + index * kWiiClusterSize, raw)) { error_ = "cluster outside the disc"; return false; }
    crypto::Aes128 aes(title_key_);
    crypto::Aes128::Block iv{};
    for (unsigned k = 0; k < 16; ++k) iv[k] = raw[0x3D0 + k]; // taken before the hashes are decrypted
    std::vector<u8> hashes(raw.begin(), raw.begin() + 0x400);
    crypto::Aes128::Block zero{};
    (void)aes.cbc_decrypt(hashes, zero);
    plain_.assign(raw.begin() + 0x400, raw.end());
    (void)aes.cbc_decrypt(plain_, iv);
    // H0: each 0x400-byte chunk
    for (unsigned c = 0; c < 31; ++c) {
        const auto h = crypto::sha1(std::span<const u8>(plain_.data() + 0x400U * c, 0x400U));
        if (!std::equal(h.begin(), h.end(), hashes.begin() + 20 * c)) {
            cached_ = ~u64{0};
            error_ = "cluster " + std::to_string(index) + ": data failed to verify against H0";
            return false;
        }
    }
    // H1: this cluster's H0 table, in its subgroup's H1 list
    const auto h0 = crypto::sha1(std::span<const u8>(hashes.data(), 0x26C));
    if (!std::equal(h0.begin(), h0.end(), hashes.begin() + 0x280 + 20 * (index % 8U))) {
        cached_ = ~u64{0};
        error_ = "cluster " + std::to_string(index) + ": H0 table failed to verify against H1";
        return false;
    }
    const auto h1 = crypto::sha1(std::span<const u8>(hashes.data() + 0x280, 0xA0));
    if (!std::equal(h1.begin(), h1.end(), hashes.begin() + 0x340 + 20 * ((index / 8U) % 8U))) {
        cached_ = ~u64{0};
        error_ = "cluster " + std::to_string(index) + ": H1 table failed to verify against H2";
        return false;
    }
    const auto h2 = crypto::sha1(std::span<const u8>(hashes.data() + 0x340, 0xA0));
    if (!std::equal(h2.begin(), h2.end(), h3_.begin() + static_cast<std::ptrdiff_t>(20U * (index / 64U)))) {
        cached_ = ~u64{0};
        error_ = "cluster " + std::to_string(index) + ": H2 table failed to verify against H3";
        return false;
    }
    cached_ = index;
    return true;
}

bool WiiPartition::read(u64 offset, std::span<u8> out) {
    if (offset > size() || out.size() > size() - offset) { error_ = "read outside the partition"; return false; }
    std::size_t done = 0;
    while (done < out.size()) {
        const u64 at = offset + done;
        const u64 cluster = at / kWiiClusterData, in = at % kWiiClusterData;
        if (!load_cluster(cluster)) return false;
        const std::size_t n = static_cast<std::size_t>(std::min<u64>(out.size() - done, kWiiClusterData - in));
        std::memcpy(out.data() + done, plain_.data() + in, n);
        done += n;
    }
    return true;
}

} // namespace orca::disc
