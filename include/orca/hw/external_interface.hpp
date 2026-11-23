// Orca Core — External Interface (EXI), physical 0x0C006800 (window 0x40).
//
// Three channels, each with five 32-bit registers (stride 0x14), from the
// public GameCube documentation (YAGCD chapter 5, WiiBrew "External Interface"):
//   +0x00 CSR    13 ROMDIS (channel 0 only; once set, only a reset clears it)
//                12 EXT (read-only: a device is plugged into the slot)
//                11 EXTINT (w1c)  10 EXTINTMASK
//                7-9 CS (one bit per device, at most one set)
//                4-6 CLK (000 = 1 MHz ... 101 = 32 MHz, 110/111 reserved)
//                3 TCINT (w1c)  2 TCINTMASK  1 EXIINT (w1c)  0 EXIINTMASK
//   +0x04 MAR    DMA physical address (32-byte aligned)
//   +0x08 LENGTH DMA length (bits 0-4 always zero)
//   +0x0C CR     4-5 TLEN (immediate length - 1)  2-3 RW (00 read, 01 write,
//                10 read+write — invalid for DMA, 11 undefined)  1 DMA  0 TSTART
//   +0x10 DATA   immediate data, first byte on the wire = bits 31-24
//
// Any channel's (TCINT & TCINTMASK) | (EXIINT & EXIINTMASK) | (EXTINT &
// EXTINTMASK) raises the PI EXI cause.
//
// Model decisions that are NOT confirmed by hardware measurements (listed in
// docs/ESTADO.md, "pendiente de verificación en hardware"):
//   * A transfer lasts bits / CLK frequency (no extra protocol overhead).
//   * Data is exchanged with the device when TSTART is written (so device
//     errors stop the CPU on that store); the results — DATA, the RAM written
//     by a DMA read, TSTART clearing and TCINT — appear at completion.
//   * An immediate read of N < 4 bytes replaces only the top N bytes of DATA.
//   * An immediate write leaves DATA unchanged; MAR/LENGTH are not modified
//     by a DMA.
// Everything without a documented behaviour is a clear device error
// (mmio_error) instead of invented data: transfers with no device selected,
// to an empty CS, read+write exchanges, DMA with RW = 10/11, reserved CLK
// values, two CS bits at once, and non-32-bit accesses.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace orca::hw {

namespace exi {
inline constexpr u32 kBase = 0x0C006800U;
inline constexpr u32 kOffsetInPage = 0x800U;
inline constexpr u32 kWindowSize = 0x40U;
inline constexpr u32 kChannelStride = 0x14U;
inline constexpr u32 kChannels = 3U;
inline constexpr u32 kDevicesPerChannel = 3U;

inline constexpr u32 CSR_EXIINTMASK = 1U << 0, CSR_EXIINT = 1U << 1, CSR_TCINTMASK = 1U << 2,
                     CSR_TCINT = 1U << 3, CSR_CLK_MASK = 7U << 4, CSR_CS_MASK = 7U << 7,
                     CSR_EXTINTMASK = 1U << 10, CSR_EXTINT = 1U << 11, CSR_EXT = 1U << 12,
                     CSR_ROMDIS = 1U << 13;
inline constexpr u32 CR_TSTART = 1U << 0, CR_DMA = 1U << 1, CR_RW_MASK = 3U << 2, CR_TLEN_MASK = 3U << 4;
inline constexpr u32 RW_READ = 0U, RW_WRITE = 1U, RW_READWRITE = 2U;

// EXI clock in Hz for CSR.CLK values 0..5 (1, 2, 4, 8, 16, 32 MHz).
constexpr u64 clock_hz(u32 clk) noexcept { return 1000000ULL << clk; }

// Device map (YAGCD / WiiBrew): channel 0: CS0 memory card slot A,
// CS1 RTC/SRAM/IPL ROM (Macronix chip), CS2 serial port 1;
// channel 1: CS0 memory card slot B; channel 2: CS0 AD16.
inline constexpr u32 kIplChannel = 0U, kIplDevice = 1U;
} // namespace exi

// A device on an EXI channel. Bytes are in wire order. A device returns false
// (with a reason in `error`) for anything it does not implement.
class ExiDevice {
public:
    virtual ~ExiDevice() = default;
    virtual const char* exi_name() const noexcept = 0;
    // External devices (memory card slots) report EXT = 1 on their channel.
    virtual bool external() const noexcept { return false; }
    virtual void select() {}
    virtual void deselect() {}
    virtual bool write(std::span<const u8> bytes, std::string& error) = 0;
    virtual bool read(std::span<u8> bytes, std::string& error) = 0;
    // DMA transfers default to the immediate path; a device may handle them as a block.
    virtual bool dma_read(std::span<u8> bytes, std::string& error) { return read(bytes, error); }
    virtual bool dma_write(std::span<const u8> bytes, std::string& error) { return write(bytes, error); }
    // CPU cycles until TCINT for a DMA (default: the EXI clock time).
    virtual u64 dma_cycles(bool is_write, u32 bytes, u64 clock_cycles) const {
        (void)is_write; (void)bytes;
        return clock_cycles;
    }
    // Called when channel 0's CSR.ROMDIS becomes 1.
    virtual void on_romdis() {}
};

class ExternalInterface final : public mem::MmioDevice {
public:
    ExternalInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz);

    // Attaches a device (not owned). Returns false if out of range or occupied.
    bool attach(u32 channel, u32 cs, ExiDevice& device);
    // Device interrupt line (EXIINT) — e.g. a memory card's INT pin.
    void set_device_interrupt(u32 channel);
    // Insertion/removal of an external device (sets EXTINT).
    void notify_slot_change(u32 channel);

    bool busy(u32 channel) const noexcept { return (ch_[channel].cr & exi::CR_TSTART) != 0U; }
    u32 csr(u32 channel) const noexcept;
    const std::string& last_error() const noexcept { return last_error_; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "EXI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    struct Channel {
        u32 csr{0}; // stored bits: masks, CS, CLK, ROMDIS and interrupt status
        u32 mar{0}, length{0}, cr{0}, data{0};
        std::array<ExiDevice*, exi::kDevicesPerChannel> devices{};
        // Completion payload computed at TSTART.
        u32 pending_data{0};
        bool pending_has_data{false};
        std::vector<u8> pending_dma; // bytes to write to RAM at completion
    };
    bool write_csr(u32 channel, u32 value);
    bool start(u32 channel);
    void complete(u32 channel);
    ExiDevice* selected(u32 channel) const noexcept;
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    mem::Bus& bus_;
    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    u64 cpu_hz_;
    std::array<Channel, exi::kChannels> ch_{};
    std::string last_error_;
};

} // namespace orca::hw
