// Orca Core — Nintendo Wii Remote HID input report decoding.
// Host Bluetooth discovery, pairing, L2CAP and output reports are separate.
#pragma once
#include "orca/core/bits.hpp"
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
    // Raw packed IR bytes: 12 for report 0x33, 10 for report 0x37.
    std::array<u8,12> ir_data{};
    u8 ir_size{};
    std::array<u8,21> extension_data{};
    u8 extension_size{};
    bool has_status{};
    u8 status_flags{}; // low nibble: flat battery, extension, speaker, IR
    u8 led_mask{};     // bits 0..3 correspond to LEDs 1..4
    u8 battery_level{};
    bool has_acknowledgement{};
    u8 acknowledged_report_id{};
    u8 acknowledgement_error{};
};
// report includes its report ID as byte zero, as delivered by Android HID.
// Build the 0x11 LED/rumble output report. led_mask bit 0..3 selects LEDs 1..4.
std::array<u8,2> make_wii_remote_led_report(u8 led_mask, bool rumble) noexcept;
// Configure one of the supported input report modes (0x30/31/33/35/37).
std::array<u8,3> make_wii_remote_report_mode(u8 report_id, bool continuous,
                                             bool rumble) noexcept;
std::array<u8,2> make_wii_remote_status_request(bool rumble) noexcept;
bool decode_wii_remote_input_report(std::span<const u8> report,
                                    WiiRemoteState& state,
                                    std::string& error) noexcept;
} // namespace orca::hw
