// Orca Core — GameCube/Wii executable formats.
//
// DOL: 0x100-byte big-endian header with 7 text and 11 data sections
//   0x00 text offsets[7]   0x1C data offsets[11]
//   0x48 text addresses[7] 0x64 data addresses[11]
//   0x90 text sizes[7]     0xAC data sizes[11]
//   0xD8 BSS address       0xDC BSS size        0xE0 entry point
// ELF: 32-bit, big-endian, EM_PPC (20); PT_LOAD program headers are loaded.
//
// Parsing never trusts the file: every offset/size is bounds-checked.
#pragma once
#include "orca/memory/bus.hpp"

#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orca::loader {

struct Segment {
    u32 address{0};        // virtual (effective) address
    std::vector<u8> data;  // initialised bytes
    u32 memory_size{0};    // >= data.size(); the rest is zero-filled
    bool executable{false};
};

struct Image {
    u32 entry{0};
    std::vector<Segment> segments;
    u32 bss_address{0};
    u32 bss_size{0};
};

std::optional<Image> parse_dol(std::span<const u8> file, std::string& error);
std::optional<Image> parse_elf(std::span<const u8> file, std::string& error);
// Detects the format (ELF magic, otherwise DOL).
std::optional<Image> parse_executable(std::span<const u8> file, std::string& error);

// Converts an image to DOL. Requires at most 7 executable and 11 other
// segments; all zero-fill parts are merged into the single DOL BSS range,
// which must not overlap initialised data.
std::optional<std::vector<u8>> make_dol(const Image& image, std::string& error);

// Maps the standard GameCube/Wii virtual windows to physical addresses:
// 0x80000000-0xBFFFFFFF (cached) and 0xC0000000-0xFFFFFFFF (uncached) both
// map to physical (address & 0x3FFFFFFF). Lower addresses are physical.
constexpr u32 virtual_to_physical(u32 address) noexcept {
    return address >= 0x80000000U ? (address & 0x3FFFFFFFU) : address;
}

// Writes the image into RAM (BSS cleared first, then sections copied, so
// section data wins if a foreign DOL's BSS overlaps it). Fails without partial
// writes if any byte would land outside RAM.
bool load_into(mem::Bus& bus, const Image& image, std::string& error);

} // namespace orca::loader
