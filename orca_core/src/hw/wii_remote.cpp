#include "orca/hw/wii_remote.hpp"
#include <algorithm>
#include <cstddef>

namespace orca::hw {
namespace {
u8 minimum_size(u8 id) noexcept {
    switch (id) {
    case 0x30: return 3;  // buttons
    case 0x31: return 6;  // buttons + accelerometer
    case 0x33: return 18; // buttons + accelerometer + IR
    case 0x35: return 22; // buttons + accelerometer + extension
    case 0x37: return 22; // buttons + accelerometer + IR + extension
    default: return 0;
    }
}
}
std::array<u8,2> make_wii_remote_led_report(u8 led_mask, bool rumble) noexcept {
    const u8 status=static_cast<u8>(((led_mask&0x0FU)<<4U)|(rumble?1U:0U));
    return {0x11U,status};
}

std::array<u8,3> make_wii_remote_report_mode(u8 report_id, bool continuous,
                                             bool rumble) noexcept {
    const u8 flags=static_cast<u8>((continuous?0x04U:0U)|(rumble?0x01U:0U));
    return {0x12U,flags,report_id};
}

std::array<u8,2> make_wii_remote_status_request(bool rumble) noexcept {
    return {0x15U,static_cast<u8>(rumble?0x01U:0U)};
}

bool decode_wii_remote_input_report(std::span<const u8> report,
                                    WiiRemoteState& state,
                                    std::string& error) noexcept {
    if (report.empty()) { error="Wii Remote: empty HID input report"; return false; }
    const u8 id=report[0], required=minimum_size(id);
    if (required==0U) { error="Wii Remote: unsupported input report ID"; return false; }
    if (report.size()<required) { error="Wii Remote: truncated input report"; return false; }
    WiiRemoteState next{};
    next.report_id=id;
    next.buttons=static_cast<u16>((static_cast<u16>(report[1])<<8U)|report[2]);
    if (id!=0x30U) {
        next.has_accelerometer=true;
        next.accelerometer={report[3],report[4],report[5]};
    }
    // IR camera payload follows the accelerometer in 0x33/0x37 reports.
    // Keep the packed bytes intact; coordinate unpacking/calibration belongs
    // to the higher-level IR sensor model.
    if (id==0x33U || id==0x37U) {
        const std::size_t ir_count=id==0x33U?12U:10U;
        std::copy_n(report.begin()+6,ir_count,next.ir_data.begin());
        next.ir_size=static_cast<u8>(ir_count);
    }
    const std::size_t extension_offset=id==0x35U?6U:(id==0x37U?16U:report.size());
    if (extension_offset<report.size()) {
        const std::size_t count=std::min(next.extension_data.size(),report.size()-extension_offset);
        std::copy_n(report.begin()+static_cast<std::ptrdiff_t>(extension_offset),count,next.extension_data.begin());
        next.extension_size=static_cast<u8>(count);
    }
    state=next;
    error.clear();
    return true;
}
} // namespace orca::hw
