// Orca Core — Nintendo Wii Remote HID input report decoding.
// Host Bluetooth discovery, pairing, L2CAP and output reports are separate.
#pragma once
#include "orca/core/types.hpp"
#include <array>
#include <span>
#include <string>

namespace orca::hw {
struct WiiRemoteButtons {
    static constexpr u16 TWO=0x0001, ONE=0x0002, B=0x0004, A=0x0008;
    static constexpr u16 MINUS=0x0010, HOME=0x0080, LEFT=0x0100;
    static constexpr u16 RIGHT=0x0200, DOWN=0x0400, UP=0x0800, PLUS=0x1000;
};
struct WiiRemoteState {
    u16 buttons{};
    std::array<u8,3> accelerometer{};
    bool has_accelerometer{};
    u8 report_id{};
    std::array<u8,21> extension_data{};
    u8 extension_size{};
};
// report includes its report ID as byte zero, as delivered by Android HID.
bool decode_wii_remote_input_report(std::span<const u8> report,
                                    WiiRemoteState& state,
                                    std::string& error) noexcept;
} // namespace orca::hw
