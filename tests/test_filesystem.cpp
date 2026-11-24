// File systems of GameCube discs and Wii game partitions: header, FST (Wii
// offsets stored >> 2), directories, bounded file reads, main DOL and apploader.
// The same volume is used raw (GameCube) and inside an encrypted Wii partition.
#include "orca/disc/fst.hpp"
#include "orca/disc/wii_disc.hpp"
#include "wii_disc_builder.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <cstring>

using namespace orca;
using namespace orca::disc;

namespace {
using test_wii::put32;

struct FileSpec { const char* name; u32 offset; std::vector<u8> data; };

std::vector<u8> pattern(std::size_t n, u32 seed) {
    std::vector<u8> d(n);
    for (auto& b : d) { seed = seed * 1664525U + 1013904223U; b = static_cast<u8>(seed >> 24); }
    return d;
}

// A volume: header, apploader, DOL, FST and files. Offsets are written >> 2 for Wii.
std::vector<u8> make_volume(bool wii) {
    const unsigned sh = wii ? 2 : 0;
    std::vector<u8> v(wii ? 8U * kWiiClusterData : 0x3B000U, 0);
    std::memcpy(v.data(), wii ? "RORC01" : "GORC01", 6);
    if (wii) put32(v, 0x18, kWiiMagic);
    else put32(v, 0x1C, kGameCubeMagic);
    std::memcpy(&v[0x20], "Orca file system test", 21);
    // apploader: 0x20 header + 0x20 bytes of code
    std::memcpy(&v[0x2440], "2026/11/24", 10);
    put32(v, 0x2450, 0x81200000U);
    put32(v, 0x2454, 0x20);
    put32(v, 0x2458, 0);
    // DOL at 0x10000: text0 (0x100, 0x200 bytes), data0 (0x300, 0x40 bytes)
    const u32 dol = 0x10000;
    put32(v, dol + 0x00, 0x100);
    put32(v, dol + 0x48, 0x80003100U);
    put32(v, dol + 0x90, 0x200);
    put32(v, dol + 0x1C, 0x300);
    put32(v, dol + 0x64, 0x80004000U);
    put32(v, dol + 0xAC, 0x40);
    put32(v, dol + 0xE0, 0x80003100U);
    const auto code = pattern(0x240, 5);
    std::memcpy(&v[dol + 0x100], code.data(), code.size());
    put32(v, 0x420, dol >> sh);
    // files
    const FileSpec files[] = {{"opening.bnr", 0x30000, pattern(0x100, 1)},
                              {"big.bin", 0x31000, pattern(0x9000, 2)},
                              {"z.txt", 0x3A000, {'h', 'e', 'l', 'l', 'o'}}};
    for (const FileSpec& f : files) std::memcpy(&v[f.offset], f.data.data(), f.data.size());
    // FST at 0x20000: / , opening.bnr, data/, data/big.bin, data/sub/, data/sub/z.txt
    const u32 fst = 0x20000;
    const char names[] = "opening.bnr\0data\0big.bin\0sub\0z.txt\0";
    const u32 name_off[] = {0, 12, 17, 25, 29};
    const auto entry = [&](u32 i, u8 type, u32 name, u32 a, u32 b) {
        put32(v, fst + 12 * i, u32{type} << 24 | name);
        put32(v, fst + 12 * i + 4, a);
        put32(v, fst + 12 * i + 8, b);
    };
    entry(0, 1, 0, 0, 6);
    entry(1, 0, name_off[0], 0x30000U >> sh, 0x100);
    entry(2, 1, name_off[1], 0, 6);
    entry(3, 0, name_off[2], 0x31000U >> sh, 0x9000);
    entry(4, 1, name_off[3], 2, 6);
    entry(5, 0, name_off[4], 0x3A000U >> sh, 5);
    std::memcpy(&v[fst + 6 * 12], names, sizeof names);
    const u32 fst_size = 6 * 12 + sizeof names + 3U; // padded to 4
    put32(v, 0x424, fst >> sh);
    put32(v, 0x428, fst_size >> sh);
    put32(v, 0x42C, fst_size >> sh);
    return v;
}

void check_file_system(DiscReader& volume, bool wii) {
    std::string err;
    const auto fs = FileSystem::open(volume, wii, err);
    CHECK(fs.has_value());
    if (!fs) { std::printf("    %s\n", err.c_str()); return; }
    CHECK_EQ(fs->header().offset_shift, wii ? 2U : 0U);
    CHECK(fs->header().game_id == (wii ? "RORC01" : "GORC01"));
    CHECK_EQ(fs->header().dol_offset, 0x10000U);
    CHECK_EQ(fs->entries().size(), 6U);
    const auto root = fs->list("/");
    CHECK_EQ(root.size(), 2U);
    if (root.size() == 2U) { CHECK(root[0]->path == "/opening.bnr"); CHECK(root[1]->path == "/data" && root[1]->directory); }
    const auto data = fs->list("/data/");
    CHECK_EQ(data.size(), 2U);
    if (data.size() == 2U) { CHECK(data[0]->path == "/data/big.bin"); CHECK(data[1]->path == "/data/sub"); }
    CHECK(fs->list("/opening.bnr").empty());
    const FstEntry* big = fs->find("/data/big.bin");
    CHECK(big != nullptr);
    if (big == nullptr) return;
    CHECK_EQ(big->offset, 0x31000U);
    const auto expect = pattern(0x9000, 2);
    const auto whole = fs->read_file("/data/big.bin", err);
    CHECK(whole.has_value() && *whole == expect);
    // partial reads, including one across a Wii cluster boundary (0x7C00 * 7 = 0x36400)
    std::vector<u8> part(0x800);
    CHECK(fs->read(*big, 0x36400U - 0x31000U - 0x400U, part, err));
    CHECK(std::equal(part.begin(), part.end(), expect.begin() + 0x5000));
    std::vector<u8> tail(16);
    CHECK(!fs->read(*big, 0x9000 - 8, tail, err)); // past the file's end
    CHECK(err.find("past the end") != std::string::npos);
    CHECK(!fs->read(*fs->find("/data"), 0, tail, err));
    const auto z = fs->read_file("/data/sub/z.txt", err);
    CHECK(z.has_value() && z->size() == 5U && (*z)[0] == 'h');
    CHECK(!fs->read_file("/data/nothing", err).has_value());
    const auto dol = fs->main_dol(err);
    CHECK(dol.has_value() && dol->size() == 0x340U);
    const auto app = fs->apploader(err);
    CHECK(app.has_value() && app->size() == 0x40U);
}
} // namespace

TEST_CASE(gamecube_file_system) {
    disc::MemoryDisc d(make_volume(false));
    check_file_system(d, false);
}

TEST_CASE(wii_partition_file_system) {
    const auto plain = make_volume(true);
    MemoryDisc disc(test_wii::build_disc(plain));
    std::string err;
    auto p = WiiPartition::open(disc, test_wii::kPart, test_wii::test_keys(), err);
    CHECK(p != nullptr);
    if (!p) { std::printf("    %s\n", err.c_str()); return; }
    check_file_system(*p, true);
    // the outer header is not a file system
    const auto outer = parse_header(disc, err);
    CHECK(outer.has_value() && outer->wii);
    if (outer) CHECK(!parse_fst(disc, *outer, err).has_value());
    // the partition read as a GameCube volume is refused; a GameCube volume read as Wii too
    CHECK(!FileSystem::open(*p, false, err).has_value());
    MemoryDisc gc(make_volume(false));
    CHECK(!FileSystem::open(gc, true, err).has_value());
    CHECK(err.find("Wii magic") != std::string::npos);
}

TEST_CASE(file_system_rejects_bad_tables) {
    std::string err;
    {
        auto v = make_volume(false);
        put32(v, 0x428, 0x7FFFFFF0U); // FST larger than the volume
        MemoryDisc d(v);
        CHECK(!FileSystem::open(d, false, err).has_value());
        CHECK(err.find("FST") != std::string::npos);
    }
    {
        auto v = make_volume(false);
        put32(v, 0x20000 + 12 * 3 + 4, 0x3AFFF); // big.bin past the volume
        MemoryDisc d(v);
        CHECK(!FileSystem::open(d, false, err).has_value());
        CHECK(err.find("/data/big.bin") != std::string::npos);
    }
    {
        auto v = make_volume(false);
        put32(v, 0x10000 + 0x90, 0x7FFFFFF0U); // DOL text section past the volume
        MemoryDisc d(v);
        const auto fs = FileSystem::open(d, false, err);
        CHECK(fs.has_value());
        if (fs) CHECK(!fs->main_dol(err).has_value());
    }
    {
        auto v = make_volume(false);
        put32(v, 0x420, 0x100); // DOL inside the disc header
        MemoryDisc d(v);
        CHECK(!FileSystem::open(d, false, err).has_value());
    }
    {
        auto v = make_volume(false);
        std::memcpy(&v[0x20000 + 6 * 12 + 17], "..\0", 3); // "big.bin" renamed ".."
        MemoryDisc d(v);
        CHECK(!FileSystem::open(d, false, err).has_value());
        CHECK(err.find("invalid name") != std::string::npos);
    }
}

TEST_MAIN("filesystem")
