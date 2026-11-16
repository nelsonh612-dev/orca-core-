#include "orca/disc/fst.hpp"

#include <cstdio>

namespace orca::disc {

namespace {
u32 be32(std::span<const u8> b, std::size_t o) {
    return (static_cast<u32>(b[o]) << 24) | (static_cast<u32>(b[o + 1]) << 16) | (static_cast<u32>(b[o + 2]) << 8) | b[o + 3];
}
} // namespace

std::optional<std::vector<FstEntry>> parse_fst_bytes(std::span<const u8> fst, u64 disc_size, std::string& error) {
    char buf[160];
    if (fst.size() < 12U) { error = "FST: smaller than the root entry"; return std::nullopt; }
    if (fst[0] != 1U) { error = "FST: entry 0 is not a directory"; return std::nullopt; }
    const u32 count = be32(fst, 8);
    if (count == 0U || static_cast<u64>(count) * 12U > fst.size()) {
        std::snprintf(buf, sizeof buf, "FST: %u entries do not fit in %zu bytes", count, fst.size());
        error = buf;
        return std::nullopt;
    }
    const std::span<const u8> strings = fst.subspan(static_cast<std::size_t>(count) * 12U);
    std::vector<FstEntry> out(count);
    out[0] = {"/", true, 0, count};
    struct Open { u32 end; std::string path; };
    std::vector<Open> stack{{count, ""}};
    for (u32 i = 1; i < count; ++i) {
        while (stack.size() > 1U && i >= stack.back().end) stack.pop_back();
        const std::size_t o = static_cast<std::size_t>(i) * 12U;
        const u8 type = fst[o];
        const u32 name_off = be32(fst, o) & 0x00FFFFFFU;
        const u32 a = be32(fst, o + 4), b = be32(fst, o + 8);
        if (type > 1U) {
            std::snprintf(buf, sizeof buf, "FST: entry %u has unknown type %u", i, type);
            error = buf;
            return std::nullopt;
        }
        std::string name;
        std::size_t p = name_off;
        while (p < strings.size() && strings[p] != 0U) name.push_back(static_cast<char>(strings[p++]));
        if (p >= strings.size() || name.empty()) {
            std::snprintf(buf, sizeof buf, "FST: entry %u name (offset 0x%X) is empty or not terminated", i, name_off);
            error = buf;
            return std::nullopt;
        }
        FstEntry e;
        e.path = stack.back().path + "/" + name;
        e.directory = type == 1U;
        e.offset = a;
        e.size = b;
        if (e.directory) {
            if (a >= i || b <= i || b > stack.back().end) {
                std::snprintf(buf, sizeof buf, "FST: directory %u has parent %u / next %u out of range", i, a, b);
                error = buf;
                return std::nullopt;
            }
            stack.push_back({b, e.path});
        } else if (static_cast<u64>(a) + b > disc_size) {
            std::snprintf(buf, sizeof buf, "FST: file %s (0x%X + 0x%X) is outside the disc", e.path.c_str(), a, b);
            error = buf;
            return std::nullopt;
        }
        out[i] = std::move(e);
    }
    return out;
}

std::optional<std::vector<FstEntry>> parse_fst(DiscReader& disc, const DiscHeader& header, std::string& error) {
    if (!header.gamecube && !header.wii) { error = "FST: header is neither GameCube nor decrypted Wii partition"; return std::nullopt; }
    std::vector<u8> data(header.fst_size);
    if (header.fst_size == 0U || !disc.read(header.fst_offset, data)) {
        error = "FST: table is empty or outside the disc";
        return std::nullopt;
    }
    return parse_fst_bytes(data, disc.size(), error);
}

const FstEntry* find_file(const std::vector<FstEntry>& fst, const std::string& path) {
    for (const FstEntry& e : fst)
        if (!e.directory && e.path == path) return &e;
    return nullptr;
}

} // namespace orca::disc
