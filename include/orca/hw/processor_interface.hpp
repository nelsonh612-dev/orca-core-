// Orca Core — Processor Interface (PI): interrupt controller, CPU→GPU FIFO
// registers, and the write-gather pipe port.
//
// Register map (physical 0x0C003000, 32-bit registers), per the publicly
// available GameCube hardware documentation (YAGCD):
//   0x00 INTSR      interrupt cause (read); writing the RSW bit clears it
//   0x04 INTMR      interrupt mask
//   0x0C FIFO_BASE  CPU FIFO start (physical, 32-byte aligned)
//   0x10 FIFO_END   CPU FIFO end
//   0x14 FIFO_WP    CPU FIFO write pointer (bit 29: wrapped)
// Every other PI register is not implemented yet: accesses report a device
// error (the CPU stops with MemoryFault and a clear reason) instead of
// returning invented values.
//
// The gather pipe (physical 0x0C008000) collects CPU stores and writes them to
// RAM at FIFO_WP in 32-byte bursts, wrapping from FIFO_END to FIFO_BASE.
#pragma once
#include "orca/memory/bus.hpp"

#include <array>
#include <functional>
#include <string>

namespace orca::hw {

namespace pi {
inline constexpr u32 kBase = 0x0C003000U;
inline constexpr u32 kGatherPipeBase = 0x0C008000U;
// INTSR / INTMR cause bits
inline constexpr u32 CAUSE_PI_ERROR = 0x00000001U;
inline constexpr u32 CAUSE_RSW = 0x00000002U;   // reset switch
inline constexpr u32 CAUSE_DI = 0x00000004U;
inline constexpr u32 CAUSE_SI = 0x00000008U;
inline constexpr u32 CAUSE_EXI = 0x00000010U;
inline constexpr u32 CAUSE_AI = 0x00000020U;
inline constexpr u32 CAUSE_DSP = 0x00000040U;
inline constexpr u32 CAUSE_MEM = 0x00000080U;
inline constexpr u32 CAUSE_VI = 0x00000100U;
inline constexpr u32 CAUSE_PE_TOKEN = 0x00000200U;
inline constexpr u32 CAUSE_PE_FINISH = 0x00000400U;
inline constexpr u32 CAUSE_CP = 0x00000800U;
inline constexpr u32 CAUSE_DEBUG = 0x00001000U;
inline constexpr u32 CAUSE_HSP = 0x00002000U;
inline constexpr u32 CAUSE_HOLLYWOOD = 0x00004000U; // Wii IPC
inline constexpr u32 FIFO_WRAP = 0x20000000U;
// PI_FLIPPER_REV of a retail console (Flipper/Hollywood revision C, as Dolphin reports)
inline constexpr u32 kFlipperRevision = 0x246500B1U;
inline constexpr u32 FIFO_ADDR_MASK = 0x03FFFFE0U;
} // namespace pi

class ProcessorInterface final : public mem::MmioDevice {
    std::array<u64, 32> raised_{};
public:
    // `line` is called whenever the CPU external-interrupt line changes level.
    explicit ProcessorInterface(mem::Bus& bus, std::function<void(bool)> line);

    void raise(u32 cause);
    void lower(u32 cause);
    u32 causes() const noexcept { return causes_; }
    // How many times each cause bit went from clear to set (diagnostics).
    u64 raised(unsigned bit) const noexcept { return bit < 32U ? raised_[bit] : 0U; }
    u32 mask() const noexcept { return mask_; }
    bool line() const noexcept { return line_level_; }

    u32 fifo_base() const noexcept { return fifo_base_; }
    u32 fifo_end() const noexcept { return fifo_end_; }
    u32 fifo_write_pointer() const noexcept { return fifo_wp_; }

    // True when a burst can be written at the current write pointer (RAM target).
    bool fifo_target_ok() const { return bus_.is_ram(fifo_wp_ & pi::FIFO_ADDR_MASK, 32); }
    // Called by the gather pipe for each 32-byte burst.
    bool write_fifo_burst(const std::array<u8, 32>& burst);

    // While the CPU and GPU FIFOs are linked the command processor drives the
    // write pointer (Dolphin copies CP's pointers into the PI after each burst).
    void set_fifo_from_cp(u32 write_pointer, u32 base, u32 end, bool wrapped) {
        fifo_base_ = base & pi::FIFO_ADDR_MASK;
        fifo_end_ = end & pi::FIFO_ADDR_MASK;
        fifo_wp_ = (write_pointer & pi::FIFO_ADDR_MASK) | (wrapped ? pi::FIFO_WRAP : (fifo_wp_ & pi::FIFO_WRAP));
    }
    // Called after each burst has been written (command processor hook).
    void set_burst_listener(std::function<void(u32 physical)> f) { burst_listener_ = std::move(f); }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "PI"; }
    std::string mmio_error() const override { return error_.empty() ? "register not implemented" : error_; }
    // GameCube: clearing bit 2 of PI_RESET_CODE resets the drive (not on Wii).
    void set_gamecube(bool gc) noexcept { gamecube_ = gc; }
    u32 reset_code() const noexcept { return reset_code_; }

private:
    void update_line();

    mem::Bus& bus_;
    std::function<void(bool)> line_callback_;
    std::function<void(u32)> burst_listener_;
    u32 causes_{0};
    u32 mask_{0};
    u32 fifo_base_{0}, fifo_end_{0}, fifo_wp_{0};
    bool line_level_{false};
    bool gamecube_{true};
    // Dolphin's power-on values: cold reset, PI_UNKNOWN 0x1FF, bus drive strength
    u32 error_cause_{0}, reset_code_{0}, unknown_{0x000001FFU}, bus_strength_{0x02492492U};
    std::string error_;
    bool fail(std::string why) { error_ = std::move(why); return false; }
    bool read32(u32 offset, u32& value);
};

class GatherPipe final : public mem::MmioDevice {
public:
    explicit GatherPipe(ProcessorInterface& pi) : pi_(pi) {}
    bool mmio_read(u32, unsigned, u64&) override { return false; } // write-only port
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "GatherPipe"; }
    u32 pending_bytes() const noexcept { return fill_; }

private:
    ProcessorInterface& pi_;
    std::array<u8, 64> buffer_{};
    u32 fill_{0};
};

} // namespace orca::hw
