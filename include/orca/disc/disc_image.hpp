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
// Wii discs: the outer header carries the Wii magic and the partition table
// (wii_disc.hpp); the game's own header, apploader, DOL and FST are inside the
// game partition, where 0x420/0x424/0x428 are stored >> 2 (offset_shift = 2,
// as Dolphin's GetOffsetShift for Wii volumes). Image formats: see blob.hpp.
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

#if defined(__unix__) || defined(__APPLE__)
// A disc image behind an open file descriptor (Android hands out descriptors
// for files the user picks). Takes ownership of `fd`.
class FdDisc final : public DiscReader {
public:
    static std::unique_ptr<FdDisc> open(int fd, std::string& error);
    ~FdDisc() override;
    FdDisc(const FdDisc&) = delete;
    FdDisc& operator=(const FdDisc&) = delete;
    u64 size() const override { return size_; }
    bool read(u64 offset, std::span<u8> out) override;
private:
    FdDisc(int fd, u64 size) : fd_(fd), size_(size) {}
    int fd_;
    u64 size_;
};
#endif

struct DiscHeader {
    std::string game_id;   // 6 characters, e.g. "GALE01"
    std::string name;
    u8 disc_number{0};
    u8 version{0};
    bool gamecube{false};
    bool wii{false};
    u8 offset_shift{0};    // 0 GameCube, 2 inside a Wii partition
    u64 dol_offset{0};     // already shifted
    u64 fst_offset{0};
    u64 fst_size{0};
    u64 fst_max_size{0};
};

struct ApploaderHeader {
    std::string date;
    u32 entry{0};
    u32 size{0};
    u32 trailer_size{0};
};

// The header at offset 0 of an image. GameCube: checks the magic and that the
// DOL is inside the image. Wii: returns the ID, name and wii = true; the game's
// DOL and FST are found with parse_volume_header on the game partition.
std::optional<DiscHeader> parse_header(DiscReader& disc, std::string& error);
// The header of a volume whose data is read directly: a GameCube disc, or a
// decrypted Wii partition (`wii`: offsets stored >> 2). Checks the DOL and FST
// ranges against the volume's size.
std::optional<DiscHeader> parse_volume_header(DiscReader& volume, bool wii, std::string& error);
std::optional<ApploaderHeader> parse_apploader_header(DiscReader& disc, std::string& error);

} // namespace orca::disc
