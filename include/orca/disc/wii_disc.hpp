// Orca Core — Wii discs: partition table, tickets, title keys and the decrypted,
// hash-verified view of a partition's data.
//
// Layout (public documentation: WiiBrew "Wii disc", "Ticket", "Title metadata";
// Dolphin DiscIO/VolumeWii.cpp read as information):
//   0x060: 0 = the partitions carry hashes; 0x061: 0 = they are encrypted (retail
//          discs: both 0; Dolphin reads discs without hashes or encryption too).
//   0x40000: four partition groups {count, table offset >> 2}; each table entry
//            {partition offset >> 2, type (0 = game, 1 = update, 2 = channel)}.
//   Partition: ticket (0x2A4 bytes) | TMD size, TMD offset >> 2, cert chain size,
//            cert chain offset >> 2, H3 table offset >> 2 (0x18000 bytes), data
//            offset >> 2, data size >> 2 (at 0x2A4-0x2BC).
//   Ticket:  encrypted title key at 0x1BF, title ID at 0x1DC (IV: title ID then 8
//            zero bytes), common key index at 0x1F1 (0 normal, 1 Korean, 2 vWii).
//   Data:    0x8000-byte clusters = 0x400 bytes of hashes (AES-CBC with the title
//            key, IV 0) + 0x7C00 bytes of data (IV = encrypted bytes 0x3D0-0x3DF).
//            Hashes: H0 at 0x000 (31 SHA-1 of the 0x400-byte data chunks), H1 at
//            0x280 (8 SHA-1 of the H0 tables of the cluster's subgroup), H2 at
//            0x340 (8 SHA-1 of the H1 tables of the group); a group is 64
//            clusters. The H3 table holds SHA-1 of each group's H2 table, and the
//            TMD's first content hash is SHA-1 of the whole H3 table.
//            Without hashes the data is contiguous (no 0x400-byte hash areas);
//            without encryption the clusters are stored in the clear.
//
// Keys. Dolphin carries Nintendo's common keys inside its source (IOSC) and uses
// them through IOS; Orca does not include them. Only encrypted partitions need
// one: the user provides it (see load_wii_key_file). Unencrypted discs need none.
#pragma once
#include "orca/core/bits.hpp"
#include "orca/disc/disc_image.hpp"
#include "orca/disc/wii_meta.hpp"

#include <array>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace orca::disc {

using AesKey = std::array<u8, 16>;

// The common keys the user supplied for this session (never the title keys: each
// WiiPartition derives and keeps its own). Overwritten when destroyed.
struct WiiKeys {
    std::array<std::optional<AesKey>, 3> common; // retail keys: index 0 normal, 1 Korean, 2 vWii
    std::optional<AesKey> dev_common;            // development consoles (tickets from Root-CA00000002-XS00000006)
    std::optional<crypto::RsaPublicKey> root;    // optional: lets the CA certificate be checked too
    bool any() const noexcept { return common[0] || common[1] || common[2] || dev_common; }
    WiiKeys() = default;
    WiiKeys(const WiiKeys&) = default;
    WiiKeys& operator=(const WiiKeys&) = default;
    ~WiiKeys() {
        for (auto& k : common)
            if (k) crypto::secure_zero(k->data(), k->size());
        if (dev_common) crypto::secure_zero(dev_common->data(), dev_common->size());
    }
};

// Reads key material the user extracted from their own console:
//   - exactly 16 bytes: one common key, stored at `index` (0-2; 3 = development);
//   - 0x400 bytes: a BootMii keys.bin ("BackupMii v1" text header of 0x100 bytes, then
//     the console's OTP; only the common key, OTP 0x14 = file 0x114, is read: the
//     console's private keys in it are neither used nor kept);
//   - text: lines "name = 32 hex digits" with the names common_key, korean_common_key,
//     vwii_common_key, dev_common_key, and optionally root_key_file = path (a root
//     key in a form parse_root_key accepts); '#' starts a comment.
// Merges into `keys`. Several files can be loaded one after another.
bool load_wii_key_file(const std::string& path, WiiKeys& keys, unsigned index, std::string& error);
bool parse_wii_keys(std::span<const u8> data, WiiKeys& keys, unsigned index, std::string& error);

struct WiiPartitionEntry {
    u64 offset{0};
    u32 type{0};
    unsigned group{0};
};
std::optional<std::vector<WiiPartitionEntry>> list_wii_partitions(DiscReader& disc, std::string& error);
const char* wii_partition_type_name(u32 type) noexcept;

// Disc-header flags at 0x60 / 0x61.
struct WiiDiscFlags {
    bool hashes{true};
    bool encryption{true};
};
std::optional<WiiDiscFlags> read_wii_disc_flags(DiscReader& disc, std::string& error);

// Compatibility view of a ticket's fields (parse_ticket_checked validates).
struct WiiTicket {
    std::array<u8, 16> encrypted_title_key{};
    u64 title_id{0};
    u8 common_key_index{0};
    std::string issuer;
};
WiiTicket parse_ticket(std::span<const u8> ticket_0x2a4);
// Title key = AES-CBC-decrypt(common key, IV = title ID || 0^8, encrypted key).
// The key is chosen by the ticket's index, or the development key for tickets
// issued by Root-CA00000002-XS00000006.
std::optional<AesKey> decrypt_title_key(const WiiTicket& ticket, const WiiKeys& keys, std::string& error);

inline constexpr u32 kWiiClusterSize = 0x8000, kWiiClusterData = 0x7C00, kWiiClusterHeader = 0x400, kWiiH3Size = 0x18000;
inline constexpr unsigned kWiiClustersPerGroup = 64, kWiiCacheClusters = 8;

// The data of one partition, decrypted, as a DiscReader (offset 0 = first data
// byte of the first cluster). Every cluster is checked against H0, H1, H2 and the
// H3 table before any of its bytes are handed out, and the H3 table against the
// TMD when opening; a mismatch makes read() fail and sets last_error() (the
// console's DI also refuses such data). Opening also decrypts and checks the
// first cluster, so a wrong common key is reported then.
class WiiPartition final : public DiscReader {
public:
    static std::unique_ptr<WiiPartition> open(DiscReader& disc, u64 partition_offset, const WiiKeys& keys, std::string& error);
    ~WiiPartition() override;
    WiiPartition(const WiiPartition&) = delete;
    WiiPartition& operator=(const WiiPartition&) = delete;

    u64 size() const override { return size_; }
    bool read(u64 offset, std::span<u8> out) override;

    const Ticket& ticket_info() const noexcept { return ticket_; }
    const Tmd& tmd_info() const noexcept { return tmd_; }
    const std::vector<Certificate>& certificates() const noexcept { return certs_; }
    WiiTicket ticket() const;
    const std::vector<u8>& ticket_bytes() const noexcept { return ticket_.bytes; } // 0x2A4 bytes
    const AesKey& title_key() const noexcept { return title_key_; }
    const std::vector<u8>& tmd() const noexcept { return tmd_.bytes; }
    const std::vector<u8>& cert_chain() const noexcept { return cert_bytes_; }
    const std::vector<u8>& h3_table() const noexcept { return h3_; }
    u64 partition_offset() const noexcept { return partition_offset_; }
    u64 data_offset() const noexcept { return data_offset_; } // absolute, on the disc
    u64 data_size() const noexcept { return data_size_; }     // on the disc, hashes included
    WiiDiscFlags flags() const noexcept { return flags_; }
    const std::string& last_error() const noexcept { return error_; }
    std::string failure_reason() const override { return error_; }
    ChainReport verify_signatures(const crypto::RsaPublicKey* root_key) const;
    // Decrypts and verifies every cluster; false at the first failure (last_error()).
    bool verify_all();

    // cache statistics (tests, diagnostics)
    u64 clusters_decrypted() const noexcept { return decrypted_; }
    u64 cache_hits() const noexcept { return hits_; }

private:
    explicit WiiPartition(DiscReader& disc) : disc_(disc) {}
    const u8* cluster(u64 index); // decrypted, verified data (0x7C00 bytes) or nullptr

    struct Slot {
        u64 index{~u64{0}};
        u64 last_use{0};
        std::vector<u8> data;
    };

    DiscReader& disc_;
    WiiDiscFlags flags_{};
    Ticket ticket_;
    Tmd tmd_;
    std::vector<Certificate> certs_;
    AesKey title_key_{};
    std::vector<u8> cert_bytes_, h3_;
    u64 partition_offset_{0}, data_offset_{0}, data_size_{0}, clusters_{0}, size_{0};
    std::array<Slot, kWiiCacheClusters> cache_{};
    u64 clock_{0}, decrypted_{0}, hits_{0};
    std::vector<u8> raw_; // scratch for one encrypted cluster
    std::string error_;
    std::mutex mutex_;    // read() and verify_all() may be called from several threads
};

} // namespace orca::disc
