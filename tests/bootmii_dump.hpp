// Test helper: a synthetic BootMii keys.bin (1024 bytes) with the real layout and
// made-up values (no real console's or Nintendo's keys).
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <optional>
#include <vector>

namespace orca::test_bootmii {

struct DumpSpec {
    std::array<u8, 16> common{};
    std::optional<std::array<u8, 16>> korean;
    u32 console_id{0x0badc0de};
    u32 ms_id{2}, ca_id{1};
    u8 fill{0x40}; // base of the made-up console keys
};

inline std::vector<u8> make_dump(const DumpSpec& s) {
    std::vector<u8> d(0x400, 0);
    char header[64];
    const int n = std::snprintf(header, sizeof header, "BackupMii v1, ConsoleID: %08x\n", s.console_id);
    std::memcpy(d.data(), header, static_cast<std::size_t>(n));
    const auto put32 = [&](std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); };
    for (std::size_t i = 0; i < 20; ++i) d[0x100 + i] = static_cast<u8>(0xB0 + i);             // boot1 hash
    std::memcpy(&d[0x114], s.common.data(), 16);                                                 // common key
    put32(0x124, s.console_id);                                                                  // NG ID
    for (std::size_t i = 0; i < 0x30; ++i) d[0x128 + i] = static_cast<u8>(s.fill + i);           // NG private key + NAND HMAC
    for (std::size_t i = 0; i < 16; ++i) d[0x158 + i] = static_cast<u8>(s.fill + 0x60 + i);      // NAND key
    for (std::size_t i = 0; i < 16; ++i) d[0x168 + i] = static_cast<u8>(s.fill + 0x80 + i);      // backup key
    put32(0x200, s.ms_id);
    put32(0x204, s.ca_id);
    put32(0x208, 0x12345678U);                                                                   // NG key ID
    d[0x248] = 7;                                                                                // boot2 version
    if (s.korean) std::memcpy(&d[0x274], s.korean->data(), 16);                                  // Korean key
    return d;
}

} // namespace orca::test_bootmii
