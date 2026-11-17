// Orca Core — Nintendo Wii Remote HID input report decoding.
// Host Bluetooth discovery, pairing, L2CAP and output reports are separate.
#pragma once
#include "orca/core/bits.hpp"
#include <array>
#include <span>
#include <string>

namespace orca::hw {
struct WiiRemoteButtons {
    static constexpr u16 TWO=0x8000, ONE=0x0001, B=0x0002, A=0x0004;
    static constexpr u16 MINUS=0x0008, HOME=0x0010, LEFT=0x0800;
    static constexpr u16 RIGHT=0x0400, DOWN=0x0200, UP=0x0100, PLUS=0x1000;
};
struct WiiRemoteState {
    u16 buttons{};
    std::array<u8,3> accelerometer{};
    bool has_accelerometer{};
    u8 report_id{};
    // Raw packed IR bytes: 12 for report 0x33, 10 for report 0x37.
    std::array<u8,12> ir_data{};
    u8 ir_size{};
    std::array<u8,21> extension_data{};
    u8 extension_size{};
};
// report includes its report ID as byte zero, as delivered by Android HID.
bool decode_wii_remote_input_report(std::span<const u8> report,
                                    WiiRemoteState& state,
                                    std::string& error) noexcept;
} // namespace orca::hw
