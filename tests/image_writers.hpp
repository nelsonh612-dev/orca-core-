// Test helpers: write a raw disc image as CISO, WBFS or GCZ (the layouts the
// readers in src/disc/blob.cpp expect; see include/orca/disc/blob.hpp).
#pragma once
#include "orca/core/bits.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#if defined(ORCA_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace orca::test_images {

inline void le32(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (8U * k)); }
inline void le64(std::vector<u8>& d, std::size_t at, u64 v) { le32(d, at, static_cast<u32>(v)); le32(d, at + 4, static_cast<u32>(v >> 32)); }
inline void be32w(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); }

inline bool all_zero(const std::vector<u8>& raw, std::size_t from, std::size_t n) {
    return std::all_of(raw.begin() + static_cast<std::ptrdiff_t>(from), raw.begin() + static_cast<std::ptrdiff_t>(std::min(raw.size(), from + n)), [](u8 b) { return b == 0U; });
}

// CISO: all-zero blocks are left out of the file (map 0).
inline std::vector<u8> make_ciso(const std::vector<u8>& raw, u32 block) {
    std::vector<u8> out(0x8000, 0);
    std::memcpy(out.data(), "CISO", 4);
    le32(out, 4, block);
    const std::size_t blocks = (raw.size() + block - 1U) / block;
    for (std::size_t i = 0; i < blocks; ++i) {
        if (i > 0U && all_zero(raw, i * block, block)) continue;
        out[8 + i] = 1;
        const std::size_t at = out.size();
        out.resize(at + block, 0);
        std::memcpy(&out[at], &raw[i * block], std::min<std::size_t>(block, raw.size() - i * block));
    }
    return out;
}

// WBFS with 512-byte HD sectors and 2 MiB WBFS sectors; all-zero blocks are left out.
inline std::vector<u8> make_wbfs(const std::vector<u8>& raw) {
    const u64 hd = 512, ws = u64{1} << 21;
    const u64 table_entries = (143432ULL * 2U * 0x8000U + ws - 1U) / ws;
    std::vector<u8> out(ws, 0); // block 0: header and disc info
    std::memcpy(out.data(), "WBFS", 4);
    out[8] = 9;
    out[9] = 21;
    out[12] = 1; // disc slot 0 used
    std::memcpy(&out[hd], raw.data(), 0x100);
    u16 next = 1;
    for (u64 i = 0; i < table_entries && i * ws < raw.size(); ++i) {
        if (i > 0U && all_zero(raw, static_cast<std::size_t>(i * ws), static_cast<std::size_t>(ws))) continue;
        const std::size_t at = out.size();
        out.resize(at + ws, 0);
        std::memcpy(&out[at], &raw[i * ws], static_cast<std::size_t>(std::min<u64>(ws, raw.size() - i * ws)));
        out[hd + 0x100 + 2 * i] = static_cast<u8>(next >> 8);
        out[hd + 0x100 + 2 * i + 1] = static_cast<u8>(next);
        ++next;
    }
    be32w(out, 4, static_cast<u32>(out.size() / hd));
    return out;
}

#if defined(ORCA_HAVE_ZLIB)
// GCZ: blocks that do not shrink are stored uncompressed (bit 63 of the pointer).
inline std::vector<u8> make_gcz(const std::vector<u8>& raw, u32 block) {
    const u32 blocks = static_cast<u32>((raw.size() + block - 1U) / block);
    std::vector<u8> data;
    std::vector<u64> pointers;
    std::vector<u32> hashes;
    for (u32 i = 0; i < blocks; ++i) {
        std::vector<u8> plain(block, 0);
        std::memcpy(plain.data(), &raw[std::size_t{i} * block], std::min<std::size_t>(block, raw.size() - std::size_t{i} * block));
        uLongf len = compressBound(block);
        std::vector<u8> z(len);
        compress2(z.data(), &len, plain.data(), block, 6);
        u64 ptr = data.size();
        const u8* stored = z.data();
        std::size_t n = len;
        if (len >= block) { ptr |= 1ULL << 63; stored = plain.data(); n = block; }
        pointers.push_back(ptr);
        hashes.push_back(static_cast<u32>(adler32(adler32(0L, Z_NULL, 0), stored, static_cast<uInt>(n))));
        data.insert(data.end(), stored, stored + n);
    }
    std::vector<u8> out(32 + std::size_t{blocks} * 12U, 0);
    le32(out, 0, 0xB10BB10BU);
    le32(out, 4, 0);
    le64(out, 8, data.size());
    le64(out, 16, raw.size());
    le32(out, 24, block);
    le32(out, 28, blocks);
    for (u32 i = 0; i < blocks; ++i) {
        le64(out, 32 + 8U * i, pointers[i]);
        le32(out, 32 + 8U * blocks + 4U * i, hashes[i]);
    }
    out.insert(out.end(), data.begin(), data.end());
    return out;
}
#endif

} // namespace orca::test_images
