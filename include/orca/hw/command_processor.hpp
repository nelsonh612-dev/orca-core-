// Orca Core — Command Processor CPU-side registers and the GPU FIFO (0x0C000000).
//
// Sources: YAGCD for the base address; Dolphin's VideoCommon/CommandProcessor.h,
// CommandProcessor.cpp and Fifo.cpp (read as information only; the FIFO loop
// was confirmed in snippets of several Dolphin versions). 16-bit registers:
//   0x00 STATUS (read only): bit 0 FIFO above high watermark, 1 below low
//        watermark, 2 read idle (distance 0 or read == write pointer),
//        3 command idle (distance 0, at the breakpoint or reads disabled),
//        4 breakpoint hit
//   0x02 CTRL (6 bits): 0 GP read enable, 1 breakpoint enable, 2 overflow
//        interrupt enable, 3 underflow interrupt enable, 4 CPU-GPU link enable,
//        5 breakpoint interrupt enable
//   0x04 CLEAR (reads 0; not emulated, as in Dolphin)
//   0x06 PERF_SELECT (3 bits), 0x0A unknown (8 bits): stored
//   0x20-0x3E FIFO base, end, high/low watermark, read-write distance, write
//        pointer, read pointer, breakpoint as LO/HI pairs; LO keeps bits
//        0xFFE0, HI keeps 0x03FF (GameCube) / 0x1FFF (Wii)
//   0x40-0x5A XF/vertex-cache metrics read 0; 0x64 reads 4 (Dolphin's stubs)
// Each gather-pipe burst while linked advances the write pointer (to the base
// when it equals the end, else +32), adds 32 to the distance, copies the
// pointers back to the PI, then runs the GPU. The GPU reads 32 bytes at the
// read pointer while reads are enabled, the distance is non-zero and the
// read pointer is not at an enabled breakpoint; the read pointer wraps the same
// way. The CP interrupt (PI cause 0x800) is (breakpoint && its enable) ||
// (above high watermark && enable) || (below low watermark && enable),
// gated by GP read enable. The GPU runs immediately (no GPU timing yet).
// Accesses to undocumented offsets or that are not 16/32 bits are errors.
#pragma once
#include "orca/gpu/gpu.hpp"
#include "orca/hw/processor_interface.hpp"
#include "orca/memory/bus.hpp"

#include <string>

namespace orca::hw {

namespace cp {
inline constexpr u32 kBase = 0x0C000000U;
inline constexpr u32 STATUS = 0x00, CTRL = 0x02, CLEAR = 0x04, PERF_SELECT = 0x06, UNK_0A = 0x0A, FIFO_BASE = 0x20,
                     FIFO_END = 0x24, FIFO_HI_WM = 0x28, FIFO_LO_WM = 0x2C, FIFO_RW_DIST = 0x30, FIFO_WP = 0x34,
                     FIFO_RP = 0x38, FIFO_BP = 0x3C;
inline constexpr u16 CTRL_READ = 0x01, CTRL_BP_ENABLE = 0x02, CTRL_OVF_INT = 0x04, CTRL_UNDF_INT = 0x08,
                     CTRL_LINK = 0x10, CTRL_BP_INT = 0x20;
inline constexpr u16 ST_OVERFLOW = 0x01, ST_UNDERFLOW = 0x02, ST_READ_IDLE = 0x04, ST_CMD_IDLE = 0x08,
                     ST_BREAKPOINT = 0x10;
} // namespace cp

class CommandProcessor final : public mem::MmioDevice {
public:
    // Diagnostics: host time spent executing GPU commands (ns).
    u64 host_ns() const noexcept { return host_ns_; }
    CommandProcessor(mem::Bus& bus, ProcessorInterface& pi, gpu::Gpu& gpu, bool wii = false);
    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "CommandProcessor"; }
    std::string mmio_error() const override { return error_; }

    // Gather-pipe burst hook (installed on the PI).
    void on_gather_burst();

    u32 base() const noexcept { return base_; }
    u32 end() const noexcept { return end_; }
    u32 write_pointer() const noexcept { return wp_; }
    u32 read_pointer() const noexcept { return rp_; }
    u32 distance() const noexcept { return dist_; }
    u16 ctrl() const noexcept { return ctrl_; }
    // The FIFO or the GPU hit something it cannot handle.
    bool stopped() const noexcept { return fifo_failed_ || gpu_.failed(); }
    std::string stop_reason() const { return fifo_failed_ ? fifo_error_ : gpu_.error(); }

private:
    bool read16(u32 offset, u16& v);
    bool write16(u32 offset, u16 v);
    u32* reg32(u32 offset);
    bool at_breakpoint() const { return (ctrl_ & cp::CTRL_BP_ENABLE) != 0U && rp_ == bp_; }
    void run_gpu();
    void update_status_and_interrupt();
    bool fail(std::string msg) { error_ = std::move(msg); return false; }

    mem::Bus& bus_;
    ProcessorInterface& pi_;
    gpu::Gpu& gpu_;
    u16 hi_mask_;
    u16 ctrl_{0}, perf_select_{0}, unk_0a_{0};
    u32 base_{0}, end_{0}, hi_wm_{0}, lo_wm_{0}, dist_{0}, wp_{0}, rp_{0}, bp_{0};
    bool bp_hit_{false}, overflow_{false}, underflow_{false};
    bool running_{false};
    u64 host_ns_{0};
    bool fifo_failed_{false};
    std::string fifo_error_;
    std::string error_;
};

} // namespace orca::hw
