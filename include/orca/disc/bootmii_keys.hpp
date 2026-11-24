// Orca Core — the BootMii key dump ("keys.bin", 1024 bytes) a user makes of their
// own Wii with BootMii's NAND backup.
//
// Layout (mini's crypto support, as documented in Dolphin's IOSC.cpp
// BootMiiKeyDump; checked field by field):
//   0x000-0x0FF  text header "BackupMii v1, ConsoleID: XXXXXXXX\n", rest zero
//   OTP (the console's one-time-programmable memory), 0x100-0x17F:
//     0x100  boot1 hash (20)          0x114  common key (16)
//     0x124  NG ID = console ID (4)   0x128  NG private key, ECC-233 (30)
//     0x144  NAND HMAC key (20)       0x158  NAND AES key (16)
//     0x168  backup (PRNG) key (16)   0x178  two unknown words
//   SEEPROM part, 0x180-0x2FF:
//     0x180  0x80 bytes not used here
//     0x200  MS ID (4)  0x204  CA ID (4)  0x208  NG key ID (4)  0x20C  NG signature (60)
//     0x248  two counters of 10 bytes (boot2 version first)    0x25C  fill (24)
//     0x274  Korean common key (16; zero on consoles that are not Korean)
//     0x284  pad (116)  0x2F8  PRNG seed (4)  0x2FC  pad (4)
//   0x300-0x3FF  padding
//
// Common keys (they decrypt the title keys of tickets: index 0 normal, 1 Korean)
// and the console's own keys (NAND encryption and HMAC, backup key, NG key pair,
// IDs) are kept apart: the console's keys are never used as a common key.
//
// Nothing here prints a key: reports name fields and checks, never values.
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orca::disc {

inline constexpr std::size_t kBootMiiDumpSize = 0x400;

// The console's own keys (Dolphin's IOSC::LoadEntries takes these from keys.bin).
struct ConsoleKeys {
    u32 ng_id{0};                    // console ID
    std::array<u8, 30> ng_priv{};    // NG private key (ECC-233)
    std::array<u8, 20> nand_hmac{};  // NAND HMAC key
    std::array<u8, 16> nand_key{};   // NAND AES key
    std::array<u8, 16> backup_key{}; // backup / PRNG key
    u32 ms_id{0}, ca_id{0}, ng_key_id{0};
    u8 boot2_version{0};
    ~ConsoleKeys();
    ConsoleKeys() = default;
    ConsoleKeys(const ConsoleKeys&) = default;
    ConsoleKeys& operator=(const ConsoleKeys&) = default;
};

struct BootMiiKeys {
    std::array<u8, 16> common_key{};
    std::optional<std::array<u8, 16>> korean_key; // only when the field is not zero
    ConsoleKeys console;
    // what was checked, for reports (never values): "header", "console ID", ...
    std::vector<std::string> checks;
    ~BootMiiKeys();
    BootMiiKeys() = default;
    BootMiiKeys(const BootMiiKeys&) = default;
    BootMiiKeys& operator=(const BootMiiKeys&) = default;
};

// Parses and validates a dump: exact size; the text header "BackupMii v1,
// ConsoleID: " + 8 hex digits + newline, NUL-terminated inside its 0x100 bytes;
// that console ID equal to the OTP's NG ID; a common key that is not zero and
// differs from the NAND and backup keys; a Korean key that is either zero (absent)
// or differs from the common key. The MS/CA IDs are reported (retail 2/1,
// development 3/2), not required. Nullopt with the reason (no values) on failure.
std::optional<BootMiiKeys> parse_bootmii_keys(std::span<const u8> dump, std::string& error);

// A short report of a parsed dump: fields found and checks passed, no values.
std::string describe_bootmii_keys(const BootMiiKeys& k);

} // namespace orca::disc
