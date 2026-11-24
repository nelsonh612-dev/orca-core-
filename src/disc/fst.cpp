#include "orca/disc/fst.hpp"

#include <algorithm>
#include <array>
#include <cstdio>

namespace orca::disc {

namespace {
u32 be32(std::span<const u8> b, std::size_t o) {
    return (static_cast<u32>(b[o]) << 24) | (static_cast<u32>(b[o + 1]) << 16) | (static_cast<u32>(b[o + 2]) << 8) | b[o + 3];
}
constexpr u64 kFstLimit = 128U << 20; // Dolphin refuses larger tables too (ARBITRARY_FILE_SYSTEM_SIZE_LIMIT)
} // namespace

std::optional<std::vector<FstEntry>> parse_fst_bytes(std::span<const u8> fst, u64 disc_size, std::string& error, unsigned shift) {
    char buf[200];
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
        if (name == "." || name == ".." || name.find('/') != std::string::npos) {
            std::snprintf(buf, sizeof buf, "FST: entry %u has an invalid name", i);
            error = buf;
            return std::nullopt;
        }
        FstEntry e;
        e.path = stack.back().path + "/" + name;
        e.directory = type == 1U;
        e.offset = e.directory ? a : u64{a} << shift;
        e.size = b;
        if (e.directory) {
            if (a >= i || b <= i || b > stack.back().end) {
                std::snprintf(buf, sizeof buf, "FST: directory %u has parent %u / next %u out of range", i, a, b);
                error = buf;
                return std::nullopt;
            }
            stack.push_back({b, e.path});
        } else if (e.offset > disc_size || e.size > disc_size - e.offset) {
            std::snprintf(buf, sizeof buf, "FST: file %s (0x%llX + 0x%llX) is outside the disc", e.path.c_str(),
                          static_cast<unsigned long long>(e.offset), static_cast<unsigned long long>(e.size));
            error = buf;
            return std::nullopt;
        }
        out[i] = std::move(e);
    }
    return out;
}

std::optional<std::vector<FstEntry>> parse_fst(DiscReader& disc, const DiscHeader& header, std::string& error) {
    if (!header.gamecube && header.offset_shift == 0U) { error = "FST: a Wii disc's FST is inside its game partition (use FileSystem on the partition)"; return std::nullopt; }
    if (header.fst_size == 0U || header.fst_size > kFstLimit) { error = "FST: table is empty or implausibly large"; return std::nullopt; }
    std::vector<u8> data(static_cast<std::size_t>(header.fst_size));
    if (!disc.read(header.fst_offset, data)) {
        error = "FST: table is outside the disc";
        return std::nullopt;
    }
    return parse_fst_bytes(data, disc.size(), error, header.offset_shift);
}

const FstEntry* find_file(const std::vector<FstEntry>& fst, const std::string& path) {
    for (const FstEntry& e : fst)
        if (!e.directory && e.path == path) return &e;
    return nullptr;
}

std::optional<FileSystem> FileSystem::open(DiscReader& volume, bool wii, std::string& error) {
    auto h = parse_volume_header(volume, wii, error);
    if (!h) return std::nullopt;
    FileSystem fs;
    fs.volume_ = &volume;
    fs.header_ = *h;
    auto e = parse_fst(volume, *h, error);
    if (!e) return std::nullopt;
    fs.entries_ = std::move(*e);
    return fs;
}

const FstEntry* FileSystem::find(const std::string& path) const {
    const std::string p = path.size() > 1U && path.back() == '/' ? path.substr(0, path.size() - 1U) : path;
    for (const FstEntry& e : entries_)
        if (e.path == p) return &e;
    return nullptr;
}

std::vector<const FstEntry*> FileSystem::list(const std::string& directory) const {
    std::vector<const FstEntry*> out;
    const FstEntry* d = find(directory);
    if (d == nullptr || !d->directory) return out;
    const std::size_t first = static_cast<std::size_t>(d - entries_.data()) + 1U;
    const std::size_t end = static_cast<std::size_t>(std::min<u64>(d->size, entries_.size()));
    for (std::size_t i = first; i < end;) {
        out.push_back(&entries_[i]);
        i = entries_[i].directory ? static_cast<std::size_t>(entries_[i].size) : i + 1U;
    }
    return out;
}

bool FileSystem::read(const FstEntry& file, u64 offset, std::span<u8> out, std::string& error) const {
    if (file.directory) { error = file.path + " is a directory"; return false; }
    if (offset > file.size || out.size() > file.size - offset) {
        error = "read past the end of " + file.path;
        return false;
    }
    if (!volume_->read(file.offset + offset, out)) {
        error = "cannot read " + file.path + " from the disc";
        return false;
    }
    return true;
}

std::optional<std::vector<u8>> FileSystem::read_file(const std::string& path, std::string& error) const {
    const FstEntry* f = find(path);
    if (f == nullptr || f->directory) { error = "no file " + path + " on the disc"; return std::nullopt; }
    std::vector<u8> data(static_cast<std::size_t>(f->size));
    if (!read(*f, 0, data, error)) return std::nullopt;
    return data;
}

u64 dol_size(std::span<const u8, 0x100> h) {
    u64 end = 0x100;
    for (std::size_t i = 0; i < 18; ++i) {
        const u64 off = be32(h, 4 * i), size = be32(h, 0x90 + 4 * i);
        if (size == 0U) continue;
        if (off < 0x100U) return 0;
        end = std::max(end, off + size);
    }
    return end;
}

std::optional<std::vector<u8>> FileSystem::main_dol(std::string& error) const {
    std::array<u8, 0x100> h{};
    if (!volume_->read(header_.dol_offset, h)) { error = "main DOL header outside the volume"; return std::nullopt; }
    const u64 size = dol_size(h);
    if (size == 0U || size > (32U << 20) || header_.dol_offset + size > volume_->size()) {
        error = "main DOL header is malformed or the DOL extends past the volume";
        return std::nullopt;
    }
    std::vector<u8> dol(static_cast<std::size_t>(size));
    if (!volume_->read(header_.dol_offset, dol)) { error = "cannot read the main DOL"; return std::nullopt; }
    return dol;
}

std::optional<std::vector<u8>> FileSystem::apploader(std::string& error) const {
    const auto a = parse_apploader_header(*volume_, error);
    if (!a) return std::nullopt;
    std::vector<u8> data(0x20U + a->size + a->trailer_size);
    if (!volume_->read(kApploaderOffset, data)) { error = "cannot read the apploader"; return std::nullopt; }
    return data;
}

} // namespace orca::disc
