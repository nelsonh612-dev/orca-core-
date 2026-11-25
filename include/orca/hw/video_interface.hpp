// Orca Core — Video Interface (VI), physical 0x0C002000.
//
// Sources: YAGCD chapter 5.3 and the NetBSD Wii driver register definitions
// (vireg.h, itself derived from YAGCD). Registers are 16-bit; 32-bit accesses
// cover two consecutive registers (big-endian). Offsets 0x00-0x7F are kept as
// a register file; DPV/DPH are computed from the timing model, DI0-DI3 have
// interrupt semantics.
//
// Timing model (derived from the register values, not hard-coded per mode):
//   * pixel clock = VI clock / 2: 13.5 MHz (VICLK=0, 27 MHz) or 27 MHz (54 MHz);
//   * a half line lasts HLW pixels (HTR0[8:0]); a line lasts 2*HLW;
//   * half lines per field = 3*EQU + PRB + 2*ACV + PSB, using VTO for odd and
//     VTE for even fields (VTR: ACV[13:4], EQU[3:0]; VTO/VTE: PSB[25:16], PRB[9:0]).
//   With the documented default NTSC values (HLW=429, EQU=6, ACV=240,
//   PRB/PSB = 24/3 odd, 25/2 even) this gives 525 half lines per field and a
//   59.94 Hz field rate; the PAL defaults give 625 half lines and exactly 50 Hz.
//   * DPV (VCT) counts lines from 1 at the start of pre-equalization, on a
//     frame basis (1-525 NTSC) when interlaced, on a field basis when DCR[NIN]=1;
//     DPH (HCT) counts pixels 1..2*HLW and restarts every line.
//   * DIn fires when (VCT, HCT) is reached while DIn[ENB]=1: DIn[INT] is set and
//     the PI VI cause is raised while any enabled DIn has INT set. Writing 0
//     to INT clears it (writing 1 does not set it).
// Timing runs while DCR[ENB]=1 and restarts from position (1,1) whenever ENB
// goes 0 -> 1 or a timing register changes (approximation: real hardware
// double-buffers some fields). XFB addresses come from TFBL/BFBL; the page-offset bit
// (address in 32-byte units) of TFBL also applies to BFBL, as Dolphin notes.
// Picture configuration (0x48): STD bits 0-7 (stride, 16-byte units per half line),
// WPL bits 8-14 (width, 16-pixel units).
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"

#include <array>
#include <functional>
#include <string>

namespace orca::hw {

namespace vi {
inline constexpr u32 kBase = 0x0C002000U;
inline constexpr u32 VTR = 0x00, DCR = 0x02, HTR0 = 0x04, HTR1 = 0x08, VTO = 0x0C, VTE = 0x10;
inline constexpr u32 BBOI = 0x14, BBEI = 0x18, TFBL = 0x1C, TFBR = 0x20, BFBL = 0x24, BFBR = 0x28;
inline constexpr u32 PICCONF = 0x48;
inline constexpr u32 DPV = 0x2C, DPH = 0x2E, DI0 = 0x30, DI1 = 0x34, DI2 = 0x38, DI3 = 0x3C, DL0 = 0x40, VICLK = 0x6C;
inline constexpr u16 DCR_ENB = 0x0001, DCR_RST = 0x0002, DCR_NIN = 0x0004;
inline constexpr u32 DI_INT = 0x80000000U, DI_ENB = 0x10000000U;
} // namespace vi

// What the VI scans out for one field (Dolphin VideoInterface.cpp OutputField, without its
// "force progressive" option): width = WPL * 16 pixels, bytes between lines = STD * 32,
// lines = ACV; the bottom (even) field reads BFBL, the top (odd) field TFBL.
struct XfbField {
    u32 address{0};
    u32 width{0};
    u32 stride{0};
    u32 height{0};
};

struct VideoTiming {
    bool valid{false};
    u32 half_line_pixels{0};   // HLW
    u32 odd_half_lines{0};
    u32 even_half_lines{0};
    bool interlaced{true};
    u64 pixel_hz{0};
};

class VideoInterface final : public mem::MmioDevice {
public:
    VideoInterface(core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz);

    // The state the IPL / system menu leaves (Dolphin VideoInterface::Preset): an
    // enabled interlaced 525-half-line timing, format NTSC (`pal` false) or PAL,
    // the two display interrupts the SDK uses, and a 640-pixel picture. The SDK's
    // VIInit keeps an enabled VI and takes the format from DCR; a disabled one it
    // resets to NTSC, also rewriting the video mode word at 0x800000CC.
    void preset(bool pal);

    VideoTiming timing() const;
    // Current beam position (VCT, HCT), both 1-based; (0, 0) when timing is off.
    std::pair<u32, u32> position() const;
    bool bottom_field() const;      // true while the even (second) field is scanned
    u64 cycles_per_line() const;    // CPU cycles per video line, 0 when timing is off
    u32 xfb_address(bool bottom) const; // physical address of the field's frame buffer
    XfbField xfb_field(bool bottom) const;

    // Called at the start of every field (for a frontend to present a frame).
    void set_field_listener(std::function<void(bool bottom)> f) { field_listener_ = std::move(f); }
    u64 fields_started() const noexcept { return fields_; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "VI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    u16 reg(u32 off) const { return regs_[off / 2U]; }
    u32 reg32(u32 off) const { return (u32{regs_[off / 2U]} << 16U) | regs_[off / 2U + 1U]; }
    u16 read16(u32 off) const;
    bool write16(u32 off, u16 v);
    u64 pixels_since_start() const;
    u64 cycles_for_pixels(u64 pixels) const;
    void restart_timing();
    void reschedule();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    u64 cpu_hz_;
    std::array<u16, 0x40> regs_{};
    bool running_{false};
    bool fresh_start_{false}; // events exactly at the start position may still fire "now"
    u64 start_cycle_{0};
    std::array<core::Scheduler::EventId, 4> di_events_{};
    core::Scheduler::EventId field_event_{0};
    std::function<void(bool)> field_listener_;
    u64 fields_{0};
    std::string last_error_;
};

} // namespace orca::hw
