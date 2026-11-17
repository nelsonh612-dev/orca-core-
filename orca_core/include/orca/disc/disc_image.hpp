// Orca Core — GameCube disc images (raw GCM/ISO, 1:1 sector dumps).
//
// Disc header layout (public GameCube documentation, YAGCD):
//   0x000 game code (4) + maker code (2)   0x006 disc number   0x007 version
//   0x018 Wii magic 0x5D1C9EA3              0x01C GameCube magic 0xC2339F3D
//   0x020 game name (up to 0x3E0 bytes, NUL-terminated)
//   0x420 main DOL offset   0x424 FST offset   0x428 FST size   0x42C FST max size
// Apploader at 0x2440: 0x00 build date (16 bytes), 0x10 entry point,
//   0x14 code size, 0x18 trailer size; code starts at 0x2460 and is loaded
//   at 0x81200000.
//
// Wii discs (encrypted partitions) are detected and rejected explicitly.
// Compressed formats (CISO, GCZ, RVZ, WBFS) are not supported.
#pragma once
#include "orca/core/bits.hpp"

#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orca::disc {

inline constexpr u32 kGameCubeMagic = 0xC2339F3DU;
inline constexpr u32 kWiiMagic = 0x5D1C9EA3U;
inline constexpr u32 kApploaderOffset = 0x2440U;
inline constexpr u32 kApploaderLoadAddress = 0x81200000U;

class DiscReader {
public:
    virtual ~DiscReader() = default;
    virtual u64 size() const = 0;
    // Reads exactly out.size() bytes; false if the range is outside the image.
    virtual bool read(u64 offset, std::span<u8> out) = 0;
};

class MemoryDisc final : public DiscReader {
public:
    explicit MemoryDisc(std::vector<u8> data) : data_(std::move(data)) {}
    u64 size() const override { return data_.size(); }
    bool read(u64 offset, std::span<u8> out) override;
private:
    std::vector<u8> data_;
};

class FileDisc final : public DiscReader {
public:
    static std::unique_ptr<FileDisc> open(const std::string& path, std::string& error);
    u64 size() const override { return size_; }
    bool read(u64 offset, std::span<u8> out) override;
private:
    std::ifstream file_;
    u64 size_{0};
};

struct DiscHeader {
    std::string game_id;   // 6 characters, e.g. "GALE01"
    std::string name;
    u8 disc_number{0};
    u8 version{0};
    bool gamecube{false};
    bool wii{false};
    u32 dol_offset{0};
    u32 fst_offset{0};
    u32 fst_size{0};
    u32 fst_max_size{0};
};

struct ApploaderHeader {
    std::string date;
    u32 entry{0};
    u32 size{0};
    u32 trailer_size{0};
};

// Wii disc partition table (all on-disc offsets are stored in 4-byte words).
inline constexpr u64 kWiiPartitionInfoOffset = 0x40000U;
inline constexpr u32 kWiiPartitionGroupCount = 4U;
struct WiiPartition {
    u64 offset{0};
    u32 type{0}; // 0 = data, 1 = update, other values are reserved
};
std::optional<std::vector<WiiPartition>> parse_wii_partition_table(DiscReader& disc, std::string& error);

struct WiiPartitionHeader {
    u32 tmd_size{0};
    u64 tmd_offset{0};
    u32 cert_size{0};
    u64 cert_offset{0};
    u64 h3_offset{0};
    u64 data_offset{0};
    u64 data_size{0};
};
// Parses the 0x1C-byte metadata block at partition +0x2A4 and validates
// every referenced range against the raw disc image.
std::optional<WiiPartitionHeader> parse_wii_partition_header(
    DiscReader& disc, const WiiPartition& partition, std::string& error);

std::optional<DiscHeader> parse_header(DiscReader& disc, std::string& error);
std::optional<ApploaderHeader> parse_apploader_header(DiscReader& disc, std::string& error);

} // namespace orca::disc
