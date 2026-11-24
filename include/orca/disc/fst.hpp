// Orca Core — GameCube and Wii disc file system table (fst.bin) and file access.
//
// Layout (wiki.re.virtualworld.fr "GCM (File format)"; nod src/disc/fst.rs;
// Dolphin DiscIO/FileSystemGCWii.cpp read as information):
//   disc header 0x424 = FST offset, 0x428 = FST size (GameCube: bytes; inside a
//   Wii partition both are stored >> 2).
//   12-byte big-endian entries, entry 0 is the root directory:
//     byte 0      type: 0 = file, 1 = directory
//     bytes 1-3   name offset in the string table
//     bytes 4-7   file: data offset (Wii: >> 2)       / directory: parent index
//     bytes 8-11  file: length in bytes                / directory: index of the
//                 first entry after its contents (root: total entry count)
//   The string table starts right after the last entry (count * 12).
// Every field is validated; a malformed table is rejected with a reason.
#pragma once
#include "orca/disc/disc_image.hpp"

#include <optional>
#include <string>
#include <vector>

namespace orca::disc {

struct FstEntry {
    std::string path;   // "/dir/file.bin"; the root is "/"
    bool directory{false};
    u64 offset{0};      // file: offset in the volume (already shifted); directory: parent index
    u64 size{0};        // file: length;                                   directory: next index
};

std::optional<std::vector<FstEntry>> parse_fst(DiscReader& disc, const DiscHeader& header, std::string& error);
// Parses an FST already in memory (for the copy the apploader loads, tests,
// tools). `offset_shift` is 2 for Wii file systems.
std::optional<std::vector<FstEntry>> parse_fst_bytes(std::span<const u8> fst, u64 disc_size, std::string& error, unsigned offset_shift = 0);
// Finds a file by its full path ("/a/b.bin"); nullptr if absent or a directory.
const FstEntry* find_file(const std::vector<FstEntry>& fst, const std::string& path);

// The file system of a volume read directly: a GameCube disc or a decrypted Wii
// partition (WiiPartition). Reads are bounded by each file's size.
class FileSystem {
public:
    static std::optional<FileSystem> open(DiscReader& volume, bool wii, std::string& error);

    const DiscHeader& header() const noexcept { return header_; }
    const std::vector<FstEntry>& entries() const noexcept { return entries_; }
    const FstEntry* find(const std::string& path) const; // file or directory
    std::vector<const FstEntry*> list(const std::string& directory) const; // direct children
    bool read(const FstEntry& file, u64 offset, std::span<u8> out, std::string& error) const;
    std::optional<std::vector<u8>> read_file(const std::string& path, std::string& error) const;

    // The main executable (DOL at 0x420) and the apploader (0x2440), whole.
    std::optional<std::vector<u8>> main_dol(std::string& error) const;
    std::optional<std::vector<u8>> apploader(std::string& error) const;

private:
    DiscReader* volume_{nullptr};
    DiscHeader header_;
    std::vector<FstEntry> entries_;
};

// Size of a DOL from its header (end of the last section); 0 if malformed.
u64 dol_size(std::span<const u8, 0x100> header);

} // namespace orca::disc
