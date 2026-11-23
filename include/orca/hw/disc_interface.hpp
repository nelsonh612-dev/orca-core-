// Orca Core — Disc Interface (DI), physical 0x0C006000.
//
// Registers (32-bit; public GameCube documentation, YAGCD / libogc):
//   0x00 DISR    status: BRK(0) DEINTMASK(1) DEINT(2) TCINTMASK(3) TCINT(4)
//                BRKINTMASK(5) BRKINT(6); interrupt bits are write-1-to-clear
//   0x04 DICVR   cover: CVR(0, 1 = open) CVRINTMASK(1) CVRINT(2, w1c)
//   0x08-0x10    DICMDBUF0-2   command words
//   0x14 DIMAR   DMA memory address (physical, 32-byte aligned)
//   0x18 DILENGTH DMA length (multiple of 32)
//   0x1C DICR    control: TSTART(0) DMA(1) RW(2: 1 = write to drive)
//   0x20 DIIMMBUF immediate data
//
// Implemented command: 0xA8 "read" with DMA (CMDBUF1 = disc offset >> 2,
// DILENGTH = byte count). On completion DIMAR advances by the length,
// DILENGTH becomes 0, TSTART clears and TCINT is set (raising the PI DI
// interrupt when TCINTMASK is set). A read outside the image sets DEINT.
// Every other command, break requests, writes to the drive, immediate-mode
// transfers and DICFG accesses are reported as device errors with a reason
// in last_error(): the CPU stops cleanly instead of receiving invented data.
//
// Timing: there is no drive model; a transfer completes `latency_cycles`
// after TSTART (default 0 = at the next scheduler step, still asynchronous).
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/disc/disc_image.hpp"
#include "orca/hw/processor_interface.hpp"

#include <memory>
#include <string>

namespace orca::hw {

namespace di {
inline constexpr u32 kBase = 0x0C006000U;
inline constexpr u32 DISR_BRK = 0x01U, DISR_DEINTMASK = 0x02U, DISR_DEINT = 0x04U, DISR_TCINTMASK = 0x08U,
                     DISR_TCINT = 0x10U, DISR_BRKINTMASK = 0x20U, DISR_BRKINT = 0x40U;
inline constexpr u32 DICVR_CVR = 0x01U, DICVR_CVRINTMASK = 0x02U, DICVR_CVRINT = 0x04U;
inline constexpr u32 DICR_TSTART = 0x01U, DICR_DMA = 0x02U, DICR_RW = 0x04U;
inline constexpr u32 kCommandRead = 0xA8U;
inline constexpr u32 kAddressMask = 0x03FFFFE0U;
} // namespace di

class DiscInterface final : public mem::MmioDevice {
public:
    DiscInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi);

    void insert(std::unique_ptr<disc::DiscReader> disc) { disc_ = std::move(disc); }
    disc::DiscReader* disc() noexcept { return disc_.get(); }
    void set_latency_cycles(u64 cycles) noexcept { latency_ = cycles; }

    bool busy() const noexcept { return (dicr_ & di::DICR_TSTART) != 0U; }
    const std::string& last_error() const noexcept { return last_error_; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "DI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    bool start_command();
    void complete();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    mem::Bus& bus_;
    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    std::unique_ptr<disc::DiscReader> disc_;
    u64 latency_{0};
    u32 disr_{0}, dicvr_{0}, cmd_[3]{}, dimar_{0}, dilength_{0}, dicr_{0}, diimmbuf_{0};
    std::string last_error_;
};

} // namespace orca::hw
