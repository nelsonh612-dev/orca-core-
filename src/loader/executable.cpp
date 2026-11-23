#include "orca/loader/executable.hpp"

#include <algorithm>

namespace orca::loader {
namespace {

u32 be32(std::span<const u8> f, std::size_t off) {
    return (static_cast<u32>(f[off]) << 24U) | (static_cast<u32>(f[off + 1]) << 16U) |
           (static_cast<u32>(f[off + 2]) << 8U) | f[off + 3];
}
u16 be16(std::span<const u8> f, std::size_t off) { return static_cast<u16>((f[off] << 8U) | f[off + 1]); }
void put32(std::vector<u8>& v, std::size_t off, u32 x) {
    v[off] = static_cast<u8>(x >> 24U); v[off + 1] = static_cast<u8>(x >> 16U);
    v[off + 2] = static_cast<u8>(x >> 8U); v[off + 3] = static_cast<u8>(x);
}
bool range_ok(std::size_t file_size, u64 off, u64 len) { return off <= file_size && len <= file_size - off; }

} // namespace

std::optional<Image> parse_dol(std::span<const u8> f, std::string& error) {
    if (f.size() < 0x100) { error = "DOL: file smaller than the 0x100-byte header"; return std::nullopt; }
    Image img;
    for (unsigned i = 0; i < 18; ++i) {
        const bool text = i < 7;
        const unsigned k = text ? i : i - 7;
        const u32 off = be32(f, (text ? 0x00 : 0x1C) + k * 4);
        const u32 addr = be32(f, (text ? 0x48 : 0x64) + k * 4);
        const u32 size = be32(f, (text ? 0x90 : 0xAC) + k * 4);
        if (size == 0) continue;
        if (!range_ok(f.size(), off, size)) { error = "DOL: section " + std::to_string(i) + " lies outside the file"; return std::nullopt; }
        if (static_cast<u64>(addr) + size > 0x100000000ULL) { error = "DOL: section address wraps"; return std::nullopt; }
        Segment s;
        s.address = addr;
        s.data.assign(f.begin() + off, f.begin() + off + size);
        s.memory_size = size;
        s.executable = text;
        img.segments.push_back(std::move(s));
    }
    img.bss_address = be32(f, 0xD8);
    img.bss_size = be32(f, 0xDC);
    img.entry = be32(f, 0xE0);
    if (img.segments.empty()) { error = "DOL: no sections"; return std::nullopt; }
    return img;
}

std::optional<Image> parse_elf(std::span<const u8> f, std::string& error) {
    if (f.size() < 52 || f[0] != 0x7F || f[1] != 'E' || f[2] != 'L' || f[3] != 'F') { error = "ELF: bad magic"; return std::nullopt; }
    if (f[4] != 1) { error = "ELF: not 32-bit"; return std::nullopt; }
    if (f[5] != 2) { error = "ELF: not big-endian"; return std::nullopt; }
    if (be16(f, 0x12) != 20) { error = "ELF: machine is not PowerPC (EM_PPC)"; return std::nullopt; }
    Image img;
    img.entry = be32(f, 0x18);
    const u32 phoff = be32(f, 0x1C);
    const u16 phentsize = be16(f, 0x2A), phnum = be16(f, 0x2C);
    if (phentsize < 32 || !range_ok(f.size(), phoff, static_cast<u64>(phentsize) * phnum)) { error = "ELF: bad program header table"; return std::nullopt; }
    for (u16 i = 0; i < phnum; ++i) {
        const std::size_t p = phoff + static_cast<std::size_t>(i) * phentsize;
        if (be32(f, p) != 1) continue; // PT_LOAD only
        const u32 off = be32(f, p + 4), vaddr = be32(f, p + 8), filesz = be32(f, p + 16), memsz = be32(f, p + 20), flags = be32(f, p + 24);
        if (memsz == 0) continue;
        if (filesz > memsz || !range_ok(f.size(), off, filesz)) { error = "ELF: segment lies outside the file"; return std::nullopt; }
        if (static_cast<u64>(vaddr) + memsz > 0x100000000ULL) { error = "ELF: segment address wraps"; return std::nullopt; }
        Segment s;
        s.address = vaddr;
        s.data.assign(f.begin() + off, f.begin() + off + filesz);
        s.memory_size = memsz;
        s.executable = (flags & 1U) != 0U;
        img.segments.push_back(std::move(s));
    }
    if (img.segments.empty()) { error = "ELF: no PT_LOAD segments"; return std::nullopt; }
    return img;
}

std::optional<Image> parse_executable(std::span<const u8> f, std::string& error) {
    if (f.size() >= 4 && f[0] == 0x7F && f[1] == 'E' && f[2] == 'L' && f[3] == 'F') return parse_elf(f, error);
    return parse_dol(f, error);
}

std::optional<std::vector<u8>> make_dol(const Image& img, std::string& error) {
    std::vector<const Segment*> text, data;
    u64 bss_lo = ~0ULL, bss_hi = 0;
    for (const Segment& s : img.segments) {
        if (!s.data.empty()) (s.executable ? text : data).push_back(&s);
        if (s.memory_size > s.data.size()) {
            bss_lo = std::min<u64>(bss_lo, static_cast<u64>(s.address) + s.data.size());
            bss_hi = std::max<u64>(bss_hi, static_cast<u64>(s.address) + s.memory_size);
        }
    }
    if (text.size() > 7 || data.size() > 11) { error = "DOL: too many sections (max 7 text, 11 data)"; return std::nullopt; }
    if (img.bss_size != 0) {
        bss_lo = std::min<u64>(bss_lo, img.bss_address);
        bss_hi = std::max<u64>(bss_hi, static_cast<u64>(img.bss_address) + img.bss_size);
    }
    if (bss_hi > bss_lo) {
        for (const Segment& s : img.segments) {
            const u64 lo = s.address, hi = lo + s.data.size();
            if (lo < bss_hi && bss_lo < hi) { error = "DOL: merged BSS range overlaps initialised data"; return std::nullopt; }
        }
    }
    std::vector<u8> out(0x100, 0);
    auto add = [&](const Segment& s, std::size_t slot_off, std::size_t slot_addr, std::size_t slot_size) {
        const std::size_t pos = (out.size() + 31U) & ~std::size_t{31};
        out.resize(pos, 0);
        out.insert(out.end(), s.data.begin(), s.data.end());
        put32(out, slot_off, static_cast<u32>(pos));
        put32(out, slot_addr, s.address);
        put32(out, slot_size, static_cast<u32>(s.data.size()));
    };
    for (std::size_t i = 0; i < text.size(); ++i) add(*text[i], 0x00 + i * 4, 0x48 + i * 4, 0x90 + i * 4);
    for (std::size_t i = 0; i < data.size(); ++i) add(*data[i], 0x1C + i * 4, 0x64 + i * 4, 0xAC + i * 4);
    if (bss_hi > bss_lo) { put32(out, 0xD8, static_cast<u32>(bss_lo)); put32(out, 0xDC, static_cast<u32>(bss_hi - bss_lo)); }
    put32(out, 0xE0, img.entry);
    return out;
}

bool load_into(mem::Bus& bus, const Image& img, std::string& error) {
    auto check = [&](u32 vaddr, u32 size) {
        if (size == 0) return true;
        const u32 pa = virtual_to_physical(vaddr);
        if (static_cast<u64>(pa) + size > 0x100000000ULL || !bus.is_ram(pa, size)) {
            error = "load: range at virtual " + std::to_string(vaddr) + " (+" + std::to_string(size) + ") is not RAM";
            return false;
        }
        return true;
    };
    if (!check(img.bss_address, img.bss_size)) return false;
    for (const Segment& s : img.segments) if (!check(s.address, s.memory_size)) return false;
    if (img.bss_size != 0) {
        const std::vector<u8> zeros(img.bss_size, 0);
        (void)bus.dma_write(virtual_to_physical(img.bss_address), zeros);
    }
    for (const Segment& s : img.segments) {
        (void)bus.dma_write(virtual_to_physical(s.address), s.data);
        if (s.memory_size > s.data.size()) {
            const std::vector<u8> zeros(s.memory_size - s.data.size(), 0);
            (void)bus.dma_write(virtual_to_physical(s.address) + static_cast<u32>(s.data.size()), zeros);
        }
    }
    return true;
}

} // namespace orca::loader
