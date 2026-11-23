// Orca Core — a new Wii NAND with the minimum a game needs, for users without
// a NAND copied from their console.
//
// Formats (WiiBrew "/shared2/sys/SYSCONF", "setting.txt", "/sys/uid.sys"):
//   SYSCONF: "SCv0", entry count, offset table (plus the past-the-end
//     offset), entries {type << 5 | (name length - 1), name, data}, 0x4000
//     bytes ending in "SCed". Types: 1 big array (length - 1 as u16),
//     2 small array (length - 1 as u8), 3 byte, 4 short, 5 long, 7 bool.
//   setting.txt: "KEY=VALUE\r\n" lines in 256 bytes, encrypted by XOR with a
//     key stream: key 0x73B5DBFA, each byte XOR the key's low byte, then the
//     key rotates left by one bit.
// The SYSCONF entries are the set Dolphin creates for a new configuration
// (used as information); the values the user chooses (language, region,
// aspect ratio, nickname, ...) come from NandSetup. BT.DINF lists the
// emulated remotes with locally administered addresses 02:4F:52:43:41:0n.
#pragma once
#include "orca/ios/bluetooth.hpp"
#include "orca/ios/nand_fs.hpp"

#include <string>
#include <vector>

namespace orca::ios {

enum class WiiRegion : u8 { Japan, Usa, Europe, Korea };

struct NandSetup {
    WiiRegion region{WiiRegion::Usa};
    u8 language{1};           // IPL.LNG: 0 Japanese, 1 English, 2 German, 3 French, 4 Spanish, 5 Italian, 6 Dutch, ...
    u8 country{49};           // IPL.SADR first byte (49 = United States)
    bool widescreen{true};    // IPL.AR
    bool progressive{false};  // IPL.PGS
    bool eurgb60{true};       // IPL.E60
    u8 sensor_bar_top{1};     // BT.BAR: 0 below, 1 above the screen
    std::string nickname{"Orca"};
    std::string serial{"000000000"};
    unsigned remotes{4};      // emulated remotes listed in BT.DINF (at most 4)
};

std::vector<u8> make_sysconf(const NandSetup& s);
std::vector<u8> make_setting_txt(const NandSetup& s);   // encrypted, 256 bytes
std::string decrypt_setting_txt(std::span<const u8> data);
ios::BdAddr emulated_remote_address(unsigned index);    // HCI order

// Creates the directories and files above on an empty (or partial) NAND;
// existing files are left alone. False with `error` on failure.
bool create_nand(NandFs& nand, const NandSetup& s, std::string& error);

} // namespace orca::ios
