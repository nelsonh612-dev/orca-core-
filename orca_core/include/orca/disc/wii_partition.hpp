// Wii raw-disc partition table and partition metadata parser.
// This deliberately does not decrypt partition data; callers must supply a
// user-owned common key through a separate crypto/key-provider layer.
#pragma once
#include "orca/disc/disc_image.hpp"

#include <array>
#include <vector>

namespace orca::disc {

inline constexpr u64 kWiiPartitionTableOffset = 0x40000ULL;
inline constexpr u32 kWiiPartitionGroups = 4U;
inline constexpr u32 kWiiPartitionEntrySize = 8U;
inline constexpr u32 kWiiPartitionHeaderSize = 0x2C0U;

struct WiiPartitionInfo {
    u64 offset{0};
    u32 type{0};
    u64 tmd_offset{0};
    u32 tmd_size{0};
    u64 cert_offset{0};
    u32 cert_size{0};
    u64 h3_offset{0};
    u64 data_offset{0};
    u64 data_size{0};
    std::array<u8, 8> title_id{};
    u8 common_key_index{0};
};

struct WiiPartitionTable {
    std::vector<WiiPartitionInfo> partitions;
};

// Parses the four group descriptors at 0x40000 and each partition's
// 0x2C0-byte metadata header. All on-disc offsets/sizes are big-endian
// 32-bit quantities measured in 4-byte words. Data offsets are relative
// to the start of the containing partition.
std::optional<WiiPartitionTable> parse_wii_partition_table(DiscReader& disc, std::string& error);

// Parses the ordinary disc header embedded at offset 0 of a decrypted Wii
// partition. Wii DOL/FST offsets in this header are stored in 4-byte words.
std::optional<DiscHeader> parse_wii_partition_header(DiscReader& partition, std::string& error);

} // namespace orca::disc
