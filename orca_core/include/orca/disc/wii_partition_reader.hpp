// Logical decrypted view of a Wii partition's 0x7C00-byte cluster payloads.
// The title key must be supplied by the caller after ticket/key-provider
// processing. This class never embeds or searches for console/common keys.
#pragma once
#include "orca/crypto/aes128.hpp"
#include "orca/disc/wii_partition.hpp"
#include <array>
#include <string>

namespace orca::disc {
inline constexpr u32 kWiiClusterRawSize = 0x8000U;
inline constexpr u32 kWiiClusterHashSize = 0x0400U;
inline constexpr u32 kWiiClusterDataSize = 0x7C00U;

class WiiPartitionReader final : public DiscReader {
public:
    WiiPartitionReader(DiscReader& raw, const WiiPartitionInfo& partition,
                       const crypto::Aes128Key& title_key);
    ~WiiPartitionReader() override;
    WiiPartitionReader(const WiiPartitionReader&) = delete;
    WiiPartitionReader& operator=(const WiiPartitionReader&) = delete;

    u64 size() const override { return logical_size_; }
    bool read(u64 offset, std::span<u8> out) override;
    bool valid() const noexcept { return valid_; }
    const std::string& error() const noexcept { return error_; }

private:
    bool verify_tmd_h3_hash(u64 tmd_offset, u32 tmd_size);
    bool verify_tmd_h3_hash(u64 tmd_offset, u32 tmd_size);
    bool read_hash_header(u64 index, std::array<u8, kWiiClusterHashSize>& out);
    bool verify_hash_group(u64 index);
    bool read_cluster(u64 index, std::array<u8, kWiiClusterDataSize>& out);
    DiscReader& raw_;
    u64 data_offset_{0};
    u64 h3_offset_{0};
    u64 verified_group_{~u64{0}};
    u64 cluster_count_{0};
    u64 logical_size_{0};
    crypto::Aes128Key title_key_{};
    bool valid_{false};
    std::string error_;
};
} // namespace orca::disc
