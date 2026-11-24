// Writes Wii test images and what Orca's reader makes of them, for
// tests/dolphin_wii_reference.py, which reads the same images following
// Dolphin's procedure independently (OpenSSL for AES, hashlib for SHA-1) and
// compares: title ID, title key (by its SHA-1), H3 check, the H0-H3 verdict of
// every block, and the decrypted data (by its SHA-1).
//
// Usage: orca_wii_reference_fixture DIR
#include "orca/crypto/sha1.hpp"
#include "orca/disc/wii_disc.hpp"
#include "wii_disc_builder.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::disc;
using namespace orca::test_wii;

namespace {

std::string hex(std::span<const u8> b) {
    std::string s;
    char t[3];
    for (const u8 x : b) { std::snprintf(t, sizeof t, "%02x", x); s += t; }
    return s;
}

void write(const std::string& path, const std::vector<u8>& d) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}

// One JSON object per partition: what Orca reads.
std::string describe(const std::string& image, const std::vector<u8>& img, const WiiKeys& keys) {
    MemoryDisc disc(img);
    std::string err, out;
    const auto parts = list_wii_partitions(disc, err);
    if (!parts) return "";
    for (std::size_t i = 0; i < parts->size(); ++i) {
        const u64 off = (*parts)[i].offset;
        out += "{\"image\": \"" + image + "\", \"partition\": " + std::to_string(i) + ", ";
        auto p = WiiPartition::open(disc, off, keys, err);
        if (!p) {
            // what failed when opening: the H3 table against the TMD, or block 0
            const char* kind = err.find("H3 table does not match") != std::string::npos ? "h3" : err.find("cluster 0") != std::string::npos ? "block0" : "other";
            out += "\"open_error\": \"" + std::string(kind) + "\"}\n";
            continue;
        }
        const auto key_hash = crypto::sha1(p->title_key());
        char tid[24];
        std::snprintf(tid, sizeof tid, "%016llx", static_cast<unsigned long long>(p->ticket_info().title_id));
        out += "\"title_id\": \"" + std::string(tid) + "\", \"title_key_sha1\": \"" + hex(key_hash) + "\", \"h3_ok\": true, \"blocks\": [";
        const u64 blocks = p->data_size() / kWiiClusterSize;
        bool all = true;
        for (u64 b = 0; b < blocks; ++b) {
            u8 one[1];
            const bool ok = p->read(b * kWiiClusterData, one);
            all = all && ok;
            out += std::string(b == 0 ? "" : ", ") + (ok ? "true" : "false");
        }
        out += "]";
        if (all) {
            std::vector<u8> data(p->size());
            (void)p->read(0, data);
            out += ", \"data_sha1\": \"" + hex(crypto::sha1(data)) + "\"";
        }
        out += "}\n";
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { std::fprintf(stderr, "usage: %s DIR\n", argv[0]); return 2; }
    const std::string dir = argv[1];
    // a disc with three partitions, each with its own title key; the third uses the Korean index
    DiscSpec spec;
    PartitionSpec a, b, c;
    a.plain = plain_data(70, 1); // two hash groups
    b.plain = plain_data(3, 2);
    b.type = 1;
    b.title_id = 0x0000000100000002ULL;
    b.title_key = AesKey{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    c.plain = plain_data(4, 3);
    c.type = 2;
    c.group = 1;
    c.title_id = 0x0001000148414A45ULL;
    c.common_index = 1;
    c.common = AesKey{0x5A, 0x5A, 0x5A, 0x5A, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    c.title_key = AesKey{0xF0, 0xE1, 0xD2, 0xC3, 0xB4, 0xA5, 0x96, 0x87, 0x78, 0x69, 0x5A, 0x4B, 0x3C, 0x2D, 0x1E, 0x0F};
    spec.partitions = {a, b, c};
    const auto good = build_disc(spec);
    WiiKeys keys = test_keys();
    keys.common[1] = c.common;
    struct Image { std::string name; std::vector<u8> data; };
    std::vector<Image> images;
    images.push_back({"good.iso", good});
    const u64 data0 = kPart + kDataOff;
    auto flip = [&](const char* name, u64 at) { auto d = good; d[at] ^= 0x01; images.push_back({name, std::move(d)}); };
    flip("data_block5.iso", data0 + 5U * kWiiClusterSize + 0x1000);       // user data of block 5
    flip("h0_block9.iso", data0 + 9U * kWiiClusterSize + 0x40);           // H0 table of block 9
    flip("h1_block17.iso", data0 + 17U * kWiiClusterSize + 0x2A0);        // H1 list of block 17
    flip("h2_block65.iso", data0 + 65U * kWiiClusterSize + 0x360);        // H2 list of block 65 (group 1)
    flip("h3_table.iso", kPart + kH3Off + 25);                            // H3 table (entry 1)
    flip("block0.iso", data0 + 0x500);                                    // block 0
    for (const Image& im : images) write(dir + "/" + im.name, im.data);
    // the test common keys (not Nintendo's) as the reference script reads them
    std::ofstream k(dir + "/keys.txt");
    k << "common_key = " << hex(kTestCommon) << "\nkorean_common_key = " << hex(c.common) << "\n";
    std::ofstream out(dir + "/orca.jsonl");
    for (const Image& im : images) out << describe(im.name, im.data, keys);
    std::printf("wrote %zu images and orca.jsonl to %s\n", images.size(), dir.c_str());
    return 0;
}
