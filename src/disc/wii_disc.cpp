// Orca Core — Wii discs. See wii_disc.hpp.
#include "orca/disc/wii_disc.hpp"

#include "orca/crypto/aes.hpp"
#include "orca/crypto/sha1.hpp"
#include "orca/ios/iosc.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

namespace orca::disc {

namespace {
u32 be32(const u8* p) noexcept { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }

std::string hex64(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

// [offset, offset + length) lies inside [0, limit), without overflow
bool inside(u64 offset, u64 length, u64 limit) noexcept { return offset <= limit && length <= limit - offset; }

std::string trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a])) != 0) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])) != 0) --b;
    return s.substr(a, b - a);
}

bool parse_hex_key(const std::string& text, AesKey& out) {
    if (text.size() != 32U) return false;
    for (std::size_t i = 0; i < 16; ++i) {
        unsigned v = 0;
        for (std::size_t j = 0; j < 2; ++j) {
            const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text[2 * i + j])));
            if (c >= '0' && c <= '9') v = v * 16U + static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v = v * 16U + static_cast<unsigned>(c - 'a' + 10);
            else return false;
        }
        out[i] = static_cast<u8>(v);
    }
    return true;
}

std::optional<std::vector<u8>> read_file(const std::string& path, std::string& error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open " + path; return std::nullopt; }
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

} // namespace

bool parse_wii_keys(std::span<const u8> data, WiiKeys& keys, unsigned index, std::string& error) {
    AesKey k{};
    if (data.size() == 16U) {
        if (index > 3U) { error = "common key index " + std::to_string(index) + " out of range (0-2, 3 = development)"; return false; }
        std::copy(data.begin(), data.end(), k.begin());
        (index == 3U ? keys.dev_common : keys.common[index]) = k;
        crypto::secure_zero(k.data(), k.size());
        return true;
    }
    if (data.size() == 0x400U) {
        // BootMii's packed dump layout (same documented offsets as Dolphin's
        // BootMiiKeyDump): common key at 0x114 and Korean key at 0x274.
        // Only these two title-decryption keys are imported here. Console
        // private material, NAND keys, IDs and signatures are deliberately
        // neither copied into WiiKeys nor used by the disc reader.
        if (std::memcmp(data.data(), "BackupMii", 9) != 0) {
            error = "a 1024-byte key file must be a BootMii keys.bin (it starts with \\"BackupMii\\")";
            return false;
        }
        AesKey common{}, korean{};
        std::copy_n(data.begin() + 0x114, common.size(), common.begin());
        std::copy_n(data.begin() + 0x274, korean.size(), korean.begin());
        const bool has_common = !std::all_of(common.begin(), common.end(), [](u8 b) { return b == 0U; });
        const bool has_korean = !std::all_of(korean.begin(), korean.end(), [](u8 b) { return b == 0U; });
        if (!has_common && !has_korean) {
            crypto::secure_zero(common.data(), common.size());
            crypto::secure_zero(korean.data(), korean.size());
            error = "keys.bin: both common-key fields are empty";
            return false;
        }
        if (has_common) keys.common[0] = common;
        if (has_korean) keys.common[1] = korean;
        crypto::secure_zero(common.data(), common.size());
        crypto::secure_zero(korean.data(), korean.size());
        crypto::secure_zero(k.data(), k.size());
        return true;
    }
    // text form
    const std::string text(data.begin(), data.end());
    if (text.find('\0') != std::string::npos) {
        error = "key file must be 16 bytes (a common key), 1024 bytes (BootMii keys.bin) or text lines \"name = hex\"";
        return false;
    }
    std::size_t pos = 0, line_no = 0, found = 0;
    while (pos <= text.size()) {
        const std::size_t end = std::min(text.find('\n', pos), text.size());
        std::string line = text.substr(pos, end - pos);
        pos = end + 1U;
        ++line_no;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        line = trim(line);
        if (line.empty()) { if (end == text.size()) break; continue; }
        const auto eq = line.find('=');
        if (eq == std::string::npos) { error = "key file line " + std::to_string(line_no) + ": expected name = value"; return false; }
        const std::string name = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
        if (name == "root_key_file") {
            const auto bytes = read_file(value, error);
            if (!bytes) return false;
            auto root = parse_root_key(*bytes, error);
            if (!root) return false;
            keys.root = std::move(*root);
            ++found;
            continue;
        }
        std::optional<AesKey>* slot = nullptr;
        if (name == "common_key") slot = &keys.common[0];
        else if (name == "korean_common_key") slot = &keys.common[1];
        else if (name == "vwii_common_key") slot = &keys.common[2];
        else if (name == "dev_common_key") slot = &keys.dev_common;
        else { error = "key file line " + std::to_string(line_no) + ": unknown name \"" + name + "\""; return false; }
        if (!parse_hex_key(value, k)) { error = "key file line " + std::to_string(line_no) + ": " + name + " needs 32 hex digits"; return false; }
        *slot = k;
        ++found;
        if (end == text.size()) break;
    }
    crypto::secure_zero(k.data(), k.size());
    if (found == 0U) { error = "key file holds no keys"; return false; }
    return true;
}

bool load_wii_key_file(const std::string& path, WiiKeys& keys, unsigned index, std::string& error) {
    auto data = read_file(path, error);
    if (!data) { error = "cannot open key file " + path; return false; }
    const bool ok = parse_wii_keys(*data, keys, index, error);
    crypto::secure_zero(data->data(), data->size());
    if (!ok) error = path + ": " + error;
    return ok;
}

const char* wii_partition_type_name(u32 type) noexcept {
    switch (type) {
    case 0: return "game";
    case 1: return "update";
    case 2: return "channel";
    default: return "other";
    }
}

std::optional<std::vector<WiiPartitionEntry>> list_wii_partitions(DiscReader& disc, std::string& error) {
    u8 hdr[0x20];
    if (!disc.read(0x40000U, hdr)) { error = "disc too small for the Wii partition table at 0x40000"; return std::nullopt; }
    std::vector<WiiPartitionEntry> out;
    for (unsigned g = 0; g < 4; ++g) {
        const u32 count = be32(hdr + 8 * g);
        const u64 table = u64{be32(hdr + 8 * g + 4)} << 2;
        if (count > 64U) { error = "partition group " + std::to_string(g) + ": implausible partition count " + std::to_string(count); return std::nullopt; }
        if (count == 0U) continue;
        if (!inside(table, u64{count} * 8U, disc.size())) {
            error = "partition group " + std::to_string(g) + ": table at " + hex64(table) + " is outside the image";
            return std::nullopt;
        }
        for (u32 k = 0; k < count; ++k) {
            u8 e[8];
            if (!disc.read(table + 8U * k, e)) { error = "partition table outside the disc"; return std::nullopt; }
            const u64 off = u64{be32(e)} << 2;
            if (off < 0x440U || off >= disc.size()) {
                error = "partition " + std::to_string(k) + " of group " + std::to_string(g) + " at " + hex64(off) + " is outside the image";
                return std::nullopt;
            }
            out.push_back(WiiPartitionEntry{off, be32(e + 4), g});
        }
    }
    if (out.empty()) { error = "the Wii partition table lists no partitions"; return std::nullopt; }
    return out;
}

std::optional<WiiDiscFlags> read_wii_disc_flags(DiscReader& disc, std::string& error) {
    u8 f[2];
    if (!disc.read(0x60, f)) { error = "disc header truncated before 0x62"; return std::nullopt; }
    WiiDiscFlags r;
    r.hashes = f[0] == 0U;
    r.encryption = f[1] == 0U;
    return r;
}

WiiTicket parse_ticket(std::span<const u8> t) {
    WiiTicket r;
    if (t.size() < kTicketSize) return r;
    std::copy_n(t.begin() + 0x1BF, 16, r.encrypted_title_key.begin());
    for (unsigned k = 0; k < 8; ++k) r.title_id = r.title_id << 8 | t[0x1DC + k];
    r.common_key_index = t[0x1F1];
    std::size_t len = 0;
    while (len < 0x40U && t[0x140 + len] != 0U) ++len;
    r.issuer.assign(reinterpret_cast<const char*>(&t[0x140]), len);
    return r;
}

std::optional<AesKey> decrypt_title_key(const WiiTicket& ticket, const WiiKeys& keys, std::string& error) {
    // ES asks IOSC (OrcaIOSC) to decrypt the ticket's key with the common key its
    // index selects, in the key set of the console type that issued the ticket.
    const ios::Iosc iosc(ios::Iosc::console_type_of(ticket.issuer), keys);
    auto key = iosc.title_key(ticket.title_id, ticket.encrypted_title_key, ticket.common_key_index, error);
    if (key) return key;
    const auto handle = iosc.common_key_handle(ticket.common_key_index, error);
    if (!handle) return std::nullopt; // unsupported index: the error says so
    const ios::Iosc::KeyEntry* e = iosc.entry(*handle);
    if (e != nullptr && !e->provided) {
        error = std::string("this encrypted partition needs the ") + e->name + " (IOSC handle " + std::to_string(*handle) +
                "), which is not provided: Orca does not include Nintendo's keys. Import the one from your own console once "
                "(app: \"Import Wii keys\"; tools: orca_disc import-keys FILE) and it is loaded automatically from then on "
                "(see docs/DISCOS.md)";
    }
    return std::nullopt;
}

WiiPartition::~WiiPartition() {
    crypto::secure_zero(title_key_.data(), title_key_.size());
    for (Slot& s : cache_) crypto::secure_zero(s.data.data(), s.data.size());
    crypto::secure_zero(raw_.data(), raw_.size());
}

WiiTicket WiiPartition::ticket() const { return parse_ticket(ticket_.bytes); }

std::unique_ptr<WiiPartition> WiiPartition::open(DiscReader& disc, u64 po, const WiiKeys& keys, std::string& error) {
    std::unique_ptr<WiiPartition> p(new WiiPartition(disc));
    const auto flags = read_wii_disc_flags(disc, error);
    if (!flags) return nullptr;
    p->flags_ = *flags;
    if (flags->encryption && !flags->hashes) {
        error = "the disc header says the partitions are encrypted but carry no hashes; such an image cannot be decrypted";
        return nullptr;
    }
    const u64 disc_size = disc.size();
    std::vector<u8> head(0x2C0);
    if (!inside(po, head.size(), disc_size) || !disc.read(po, head)) { error = "partition header at " + hex64(po) + " is outside the image"; return nullptr; }
    auto ticket = parse_ticket_checked(std::span<const u8>(head.data(), kTicketSize), error);
    if (!ticket) return nullptr;
    p->ticket_ = std::move(*ticket);
    const u32 tmd_size = be32(&head[0x2A4]), cert_size = be32(&head[0x2AC]);
    const u64 tmd_off = u64{be32(&head[0x2A8])} << 2, cert_off = u64{be32(&head[0x2B0])} << 2;
    const u64 h3_off = u64{be32(&head[0x2B4])} << 2, data_off = u64{be32(&head[0x2B8])} << 2;
    const u64 data_size = u64{be32(&head[0x2BC])} << 2;
    const u64 room = disc_size - po;
    if (tmd_size < kTmdHeaderSize || tmd_size > kTmdMaxSize) { error = "TMD size " + hex64(tmd_size) + " outside 0x1E4..0x49E4"; return nullptr; }
    if (cert_size == 0U || cert_size > 0x10000U) { error = "certificate chain size " + hex64(cert_size) + " is implausible"; return nullptr; }
    if (tmd_off < 0x2C0U || !inside(tmd_off, tmd_size, room)) { error = "TMD at partition offset " + hex64(tmd_off) + " is outside the image"; return nullptr; }
    if (cert_off < 0x2C0U || !inside(cert_off, cert_size, room)) { error = "certificate chain at partition offset " + hex64(cert_off) + " is outside the image"; return nullptr; }
    if (data_off < 0x2C0U || data_size == 0U) { error = "partition data offset/size invalid"; return nullptr; }
    // As in Dolphin, the data may extend past the end of a trimmed image: only the
    // clusters actually read must be present (a missing one is reported then).
    if (data_off >= room) {
        error = "partition data starts at " + hex64(po + data_off) + ", past the end of the image (" + hex64(disc_size) +
                " bytes): truncated image";
        return nullptr;
    }
    std::vector<u8> tmd_bytes(tmd_size);
    p->cert_bytes_.resize(cert_size);
    if (!disc.read(po + tmd_off, tmd_bytes) || !disc.read(po + cert_off, p->cert_bytes_)) { error = "cannot read the TMD or certificate chain"; return nullptr; }
    auto tmd = parse_tmd_checked(tmd_bytes, error);
    if (!tmd) return nullptr;
    p->tmd_ = std::move(*tmd);
    if (p->tmd_.title_id != p->ticket_.title_id) {
        error = "the TMD's title ID " + hex64(p->tmd_.title_id) + " differs from the ticket's " + hex64(p->ticket_.title_id);
        return nullptr;
    }
    auto certs = parse_cert_chain(p->cert_bytes_, error);
    if (!certs) return nullptr;
    p->certs_ = std::move(*certs);

    p->partition_offset_ = po;
    p->data_offset_ = po + data_off;
    p->data_size_ = data_size;
    if (flags->hashes) {
        if (h3_off < 0x2C0U || !inside(h3_off, kWiiH3Size, room)) { error = "H3 table at partition offset " + hex64(h3_off) + " is outside the image"; return nullptr; }
        p->h3_.resize(kWiiH3Size);
        if (!disc.read(po + h3_off, p->h3_)) { error = "cannot read the H3 table"; return nullptr; }
        // The TMD of a disc partition has one content, the H3 table (as Dolphin's
        // CheckH3TableIntegrity requires).
        if (p->tmd_.contents.size() != 1U) {
            error = "the partition's TMD lists " + std::to_string(p->tmd_.contents.size()) + " contents instead of one (the H3 table)";
            return nullptr;
        }
        const auto h3_hash = crypto::sha1(p->h3_);
        if (!std::equal(h3_hash.begin(), h3_hash.end(), p->tmd_.contents[0].sha1.begin())) {
            error = "H3 table does not match the TMD's content hash";
            return nullptr;
        }
        if (data_size % kWiiClusterSize != 0U) { error = "partition data size " + hex64(data_size) + " is not a whole number of 0x8000-byte clusters"; return nullptr; }
        p->clusters_ = data_size / kWiiClusterSize;
        if (p->clusters_ > u64{kWiiH3Size / 20U} * kWiiClustersPerGroup) { error = "partition larger than its H3 table covers"; return nullptr; }
        p->size_ = p->clusters_ * kWiiClusterData;
    } else {
        p->size_ = data_size;
    }
    if (flags->encryption) {
        auto key = decrypt_title_key(p->ticket(), keys, error);
        if (!key) return nullptr;
        p->title_key_ = *key;
        crypto::secure_zero(key->data(), key->size());
        // a wrong common key yields a wrong title key: cluster 0 then fails H0
        u8 probe[1];
        if (!p->read(0, probe)) {
            error = "cluster 0 does not verify after decryption (" + p->error_ +
                    "): the common key is wrong for this disc, or the image is corrupt";
            return nullptr;
        }
    }
    return p;
}

const u8* WiiPartition::cluster(u64 index) {
    ++clock_;
    for (Slot& s : cache_) {
        if (s.index == index) {
            s.last_use = clock_;
            ++hits_;
            return s.data.data();
        }
    }
    Slot* slot = &cache_[0];
    for (Slot& s : cache_) {
        if (s.last_use < slot->last_use) slot = &s;
    }
    slot->index = ~u64{0}; // invalid until verified
    slot->data.resize(kWiiClusterData);
    raw_.resize(kWiiClusterSize);
    if (!disc_.read(data_offset_ + index * kWiiClusterSize, raw_)) {
        error_ = "cluster " + std::to_string(index) + " lies past the end of the image or cannot be read (truncated or trimmed image)";
        const std::string why = disc_.failure_reason();
        if (!why.empty()) error_ += ": " + why;
        return nullptr;
    }
    std::array<u8, kWiiClusterHeader> hashes{};
    std::memcpy(hashes.data(), raw_.data(), hashes.size());
    std::memcpy(slot->data.data(), raw_.data() + kWiiClusterHeader, kWiiClusterData);
    if (flags_.encryption) {
        crypto::Aes128 aes(title_key_);
        crypto::Aes128::Block iv{};
        std::memcpy(iv.data(), raw_.data() + 0x3D0, 16); // taken before the hashes are decrypted
        crypto::Aes128::Block zero{};
        (void)aes.cbc_decrypt(hashes, zero);
        (void)aes.cbc_decrypt(slot->data, iv);
    }
    ++decrypted_;
    const u8* data = slot->data.data();
    const auto fail = [&](const char* what) -> const u8* {
        error_ = "cluster " + std::to_string(index) + ": " + what;
        return nullptr;
    };
    for (unsigned c = 0; c < 31; ++c) { // H0: each 0x400-byte chunk
        const auto h = crypto::sha1(std::span<const u8>(data + 0x400U * c, 0x400U));
        if (!std::equal(h.begin(), h.end(), hashes.begin() + 20 * c)) return fail("data failed to verify against H0");
    }
    const auto h0 = crypto::sha1(std::span<const u8>(hashes.data(), 0x26C)); // this cluster's H0 table, in its subgroup's H1 list
    if (!std::equal(h0.begin(), h0.end(), hashes.begin() + 0x280 + 20 * (index % 8U))) return fail("H0 table failed to verify against H1");
    const auto h1 = crypto::sha1(std::span<const u8>(hashes.data() + 0x280, 0xA0));
    if (!std::equal(h1.begin(), h1.end(), hashes.begin() + 0x340 + 20 * ((index / 8U) % 8U))) return fail("H1 table failed to verify against H2");
    const auto h2 = crypto::sha1(std::span<const u8>(hashes.data() + 0x340, 0xA0));
    if (!std::equal(h2.begin(), h2.end(), h3_.begin() + static_cast<std::ptrdiff_t>(20U * (index / kWiiClustersPerGroup))))
        return fail("H2 table failed to verify against H3");
    slot->index = index;
    slot->last_use = clock_;
    return data;
}

bool WiiPartition::read(u64 offset, std::span<u8> out) {
    if (!inside(offset, out.size(), size_)) {
        error_ = "read of " + std::to_string(out.size()) + " bytes at " + hex64(offset) + " is outside the partition (" + hex64(size_) + " bytes)";
        return false;
    }
    if (!flags_.hashes) { // contiguous, unencrypted data (see open())
        if (!disc_.read(data_offset_ + offset, out)) { error_ = "partition data past the end of the image (truncated or trimmed image)"; return false; }
        return true;
    }
    std::size_t done = 0;
    while (done < out.size()) {
        const u64 at = offset + done;
        const u64 index = at / kWiiClusterData, in = at % kWiiClusterData;
        const u8* data = cluster(index);
        if (data == nullptr) return false;
        const std::size_t n = static_cast<std::size_t>(std::min<u64>(out.size() - done, kWiiClusterData - in));
        std::memcpy(out.data() + done, data + in, n);
        done += n;
    }
    return true;
}

bool WiiPartition::verify_all() {
    if (!flags_.hashes) { error_ = "this disc carries no hashes to verify"; return false; }
    for (u64 i = 0; i < clusters_; ++i)
        if (cluster(i) == nullptr) return false;
    return true;
}

ChainReport WiiPartition::verify_signatures(const crypto::RsaPublicKey* root_key) const {
    return verify_chain(ticket_, tmd_, certs_, root_key);
}

} // namespace orca::disc
