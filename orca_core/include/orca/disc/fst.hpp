// Orca Core — GameCube disc file system table (fst.bin).
//
// Layout (wiki.re.virtualworld.fr "GCM (File format)"; nod src/disc/fst.rs):
//   disc header 0x424 = FST offset, 0x428 = FST size (GameCube: bytes).
//   12-byte big-endian entries, entry 0 is the root directory:
//     byte 0      type: 0 = file, 1 = directory
//     bytes 1-3   name offset in the string table
//     bytes 4-7   file: data offset on the disc   / directory: parent index
//     bytes 8-11  file: length in bytes            / directory: index of the
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
    u32 offset{0};      // file: disc offset; directory: parent index
    u32 size{0};        // file: length;      directory: next index
};

std::optional<std::vector<FstEntry>> parse_fst(DiscReader& disc, const DiscHeader& header, std::string& error);
// Parses an FST already in memory (for the copy the apploader loads, tests, tools).
std::optional<std::vector<FstEntry>> parse_fst_bytes(std::span<const u8> fst, u64 disc_size, std::string& error);
// Finds a file by its full path ("/a/b.bin"); nullptr if absent or a directory.
const FstEntry* find_file(const std::vector<FstEntry>& fst, const std::string& path);

} // namespace orca::disc
