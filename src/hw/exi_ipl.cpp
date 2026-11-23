#include "orca/hw/exi_ipl.hpp"

#include <cstdio>

namespace orca::hw {

IplRtcSram::IplRtcSram(core::Scheduler& scheduler, u64 cpu_hz) : scheduler_(scheduler), cpu_hz_(cpu_hz) {}

bool IplRtcSram::load_rom(std::vector<u8> rom) {
    if (rom.empty() || rom.size() > exi_ipl::kRomSize) return false;
    rom_ = std::move(rom);
    return true;
}

void IplRtcSram::set_rtc(u32 seconds) {
    rtc_base_ = seconds;
    rtc_base_cycle_ = scheduler_.now();
}

u32 IplRtcSram::rtc() const {
    const u64 elapsed = (scheduler_.now() - rtc_base_cycle_) / cpu_hz_;
    return static_cast<u32>(rtc_base_ + elapsed); // 32-bit counter wraps
}

void IplRtcSram::select() {
    phase_ = Phase::Command;
    command_ = 0;
    command_bytes_ = 0;
    cursor_ = 0;
}

void IplRtcSram::deselect() { select(); }

bool IplRtcSram::decode(std::string& error) {
    char buf[128];
    const bool is_write = (command_ & exi_ipl::kWriteBit) != 0U;
    const u32 address = (command_ & 0x7FFFFFFFU) >> 6;
    if (address < exi_ipl::kRomSize) {
        if (is_write) { error = "the IPL ROM is read-only"; return false; }
        if (rom_.empty()) { error = "IPL ROM read, but no ROM image is loaded"; return false; }
        if (romdis_) { error = "IPL ROM read with the descrambler disabled (ROMDIS): raw data is not modelled"; return false; }
        target_ = Target::Rom;
        cursor_ = address;
    } else if (address == exi_ipl::kRtcAddress) {
        target_ = Target::Rtc;
        cursor_ = 0;
        if (!is_write) {
            const u32 v = rtc();
            rtc_buf_ = {static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)};
        }
    } else if (address >= exi_ipl::kSramAddress && address < exi_ipl::kSramAddress + exi_ipl::kSramSize) {
        target_ = Target::Sram;
        cursor_ = address - exi_ipl::kSramAddress;
    } else {
        std::snprintf(buf, sizeof buf, "command %08X: address 0x%06X is not implemented", command_, address);
        error = buf;
        return false;
    }
    phase_ = is_write ? Phase::Write : Phase::Read;
    return true;
}

bool IplRtcSram::limit(u32 count, std::string& error) const {
    u64 end = static_cast<u64>(cursor_) + count;
    const char* what = "";
    u64 size = 0;
    switch (target_) {
    case Target::Rom: size = rom_.size(); what = "past the end of the loaded IPL ROM image"; break;
    case Target::Rtc: size = 4U; what = "past the 4-byte RTC counter"; break;
    case Target::Sram: size = exi_ipl::kSramSize; what = "past the end of the 64-byte SRAM"; break;
    }
    if (end > size) {
        error = std::string("access ") + what + " (behaviour undocumented)";
        return false;
    }
    return true;
}

bool IplRtcSram::write(std::span<const u8> bytes, std::string& error) {
    std::size_t i = 0;
    while (i < bytes.size() && phase_ == Phase::Command) {
        command_ = (command_ << 8) | bytes[i++];
        if (++command_bytes_ == 4U && !decode(error)) {
            select();
            return false;
        }
    }
    if (i == bytes.size()) return true;
    if (phase_ == Phase::Read) { error = "data written during a read sequence"; return false; }
    const auto rest = static_cast<u32>(bytes.size() - i);
    if (!limit(rest, error)) return false;
    for (; i < bytes.size(); ++i, ++cursor_) {
        if (target_ == Target::Sram) {
            sram_[cursor_] = bytes[i];
        } else { // RTC
            rtc_buf_[cursor_] = bytes[i];
            if (cursor_ == 3U)
                set_rtc((static_cast<u32>(rtc_buf_[0]) << 24) | (static_cast<u32>(rtc_buf_[1]) << 16) |
                        (static_cast<u32>(rtc_buf_[2]) << 8) | rtc_buf_[3]);
        }
    }
    return true;
}

bool IplRtcSram::read(std::span<u8> bytes, std::string& error) {
    if (phase_ == Phase::Command) { error = "read before a complete 4-byte command word"; return false; }
    if (phase_ == Phase::Write) { error = "read during a write sequence"; return false; }
    if (!limit(static_cast<u32>(bytes.size()), error)) return false;
    for (u8& b : bytes) {
        switch (target_) {
        case Target::Rom: b = rom_[cursor_]; break;
        case Target::Rtc: b = rtc_buf_[cursor_]; break;
        case Target::Sram: b = sram_[cursor_]; break;
        }
        ++cursor_;
    }
    return true;
}

} // namespace orca::hw
