// Orca Core — Wii discs: partition table, tickets, title keys and the decrypted,
// hash-verified view of a partition's data.
//
// Layout (public documentation: WiiBrew "Wii disc", "Ticket", "Title metadata"):
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
// The common keys are Nintendo's and are not part of Orca: the user provides
// them (a 16-byte key file or a BootMii keys.bin, common key at 0x114).
#pragma once
#include "orca/core/bits.hpp"
#include "orca/disc/disc_image.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orca::disc {

using AesKey = std::array<u8, 16>;

struct WiiKeys {
    std::array<std::optional<AesKey>, 3> common; // index 0 normal, 1 Korean, 2 vWii
};

// Reads a common key: a file of exactly 16 bytes (stored at `index`), or a BootMii
// keys.bin (0x400 bytes; the OTP's common key at 0x114, stored at index 0).
bool load_wii_key_file(const std::string& path, WiiKeys& keys, unsigned index, std::string& error);

struct WiiPartitionEntry {
    u64 offset{0};
    u32 type{0};
    unsigned group{0};
};
std::optional<std::vector<WiiPartitionEntry>> list_wii_partitions(DiscReader& disc, std::string& error);

struct WiiTicket {
    std::array<u8, 16> encrypted_title_key{};
    u64 title_id{0};
    u8 common_key_index{0};
};
WiiTicket parse_ticket(std::span<const u8> ticket_0x2a4);
// Title key = AES-CBC-decrypt(common key, IV = title ID || 0^8, encrypted key).
std::optional<AesKey> decrypt_title_key(const WiiTicket& ticket, const WiiKeys& keys, std::string& error);

inline constexpr u32 kWiiClusterSize = 0x8000, kWiiClusterData = 0x7C00, kWiiH3Size = 0x18000;

// The data of one partition, decrypted, as a DiscReader (offset 0 = first data
// byte of the first cluster). Every cluster read is checked against H0, H1, H2
// and the H3 table, and the H3 table against the TMD when opening; a mismatch
// makes read() fail and sets last_error() (the console's DI also refuses such data).
class WiiPartition final : public DiscReader {
public:
    static std::unique_ptr<WiiPartition> open(DiscReader& disc, u64 partition_offset, const WiiKeys& keys, std::string& error);

    u64 size() const override { return clusters_ * kWiiClusterData; }
    bool read(u64 offset, std::span<u8> out) override;

    const WiiTicket& ticket() const noexcept { return ticket_; }
    const std::vector<u8>& ticket_bytes() const noexcept { return ticket_bytes_; } // 0x2A4 bytes
    const AesKey& title_key() const noexcept { return title_key_; }
    const std::vector<u8>& tmd() const noexcept { return tmd_; }
    const std::vector<u8>& cert_chain() const noexcept { return certs_; }
    const std::vector<u8>& h3_table() const noexcept { return h3_; }
    u64 data_offset() const noexcept { return data_offset_; } // absolute, on the disc
    const std::string& last_error() const noexcept { return error_; }

private:
    WiiPartition(DiscReader& disc) : disc_(disc) {}
    bool load_cluster(u64 index);

    DiscReader& disc_;
    WiiTicket ticket_{};
    AesKey title_key_{};
    std::vector<u8> ticket_bytes_, tmd_, certs_, h3_;
    u64 partition_offset_{0}, data_offset_{0}, clusters_{0};
    // one-cluster cache
    u64 cached_{~u64{0}};
    std::vector<u8> plain_;
    std::string error_;
};

} // namespace orca::disc
