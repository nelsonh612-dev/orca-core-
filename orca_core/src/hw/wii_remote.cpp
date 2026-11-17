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
