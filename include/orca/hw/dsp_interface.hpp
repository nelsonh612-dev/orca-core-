// Orca Core — DSP interface block, physical 0x0C005000 (window 0x200):
// mailboxes, control/status register, ARAM controller + ARAM DMA, AI DMA.
//
// Sources: YAGCD chapter 5 (register map, CSR bits, mailbox protocol,
// AR_DMA_CNT direction bit, AI DMA control); WiiBrew "Hardware/DSP" (ARAM
// DMA started by writing AR_DMA_CNT_L, processed in 32-byte units: after
// each unit the count drops by 32 and both addresses advance by 32; the low
// five address bits are hard-wired to 0); Linux gcn-aram.c (ARINT marks the
// end of an ARAM DMA, 32-bit accesses to the register pairs, CSR bits
// RES/PIINT/HALT/AIDINT/AIDINTMASK/ARINT/ARINTMASK/DSPINT/DSPINTMASK/DSPDMA).
//
// 16-bit registers (16- and 32-bit accesses):
//   0x00/0x02 DSP mailbox (CPU -> DSP), bit 15 of 0x00 = mail pending
//   0x04/0x06 CPU mailbox (DSP -> CPU), bit 15 of 0x04 = mail present
//   0x0A CSR: 0 RES, 1 PIINT, 2 HALT, 3 AIDINT, 4 AIDINTMASK, 5 ARINT,
//             6 ARINTMASK, 7 DSPINT, 8 DSPINTMASK, 9 ARAM DMA busy (RO), 11 ?
//   0x12 AR_SIZE, 0x16 AR_MODE, 0x1A AR_REFRESH (stored, no modelled effect)
//   0x20/0x22 AR_DMA_MMADDR, 0x24/0x26 AR_DMA_ARADDR,
//   0x28/0x2A AR_DMA_CNT (bit 31: 0 = main memory -> ARAM, 1 = ARAM -> main)
//   0x30/0x32 AI DMA start address, 0x36 AI DMA control (bit 15 play,
//             bits 0-14 length / 32), 0x3A AI DMA bytes left
// PI DSP cause = (AIDINT & AIDINTMASK) | (ARINT & ARINTMASK) | (DSPINT & DSPINTMASK).
//
// The DSP core (orca/dsp/dsp_core.hpp) runs in slices driven by the
// scheduler, 1 DSP instruction per 6 CPU cycles (81 MHz DSP on a 486 MHz
// GameCube; the same ratio is used on Wii). Mailboxes connect to the core.
// RES resets the core; with bit 11 also set, the 1024 bytes at main memory
// 0x01000000 are copied to IRAM 0 and the DSP starts at 0 (DSP manual,
// "Initialization", from libogc __dsp_bootstrap). Without bit 11 the reset
// starts at IROM 0x8000 (needs an IROM dump). HALT in CSR stops the core; the
// HALT instruction sets it. PIINT requests the DSP external interrupt.
// Clear device errors: starting AI DMA playback (timing not confirmed), ARAM
// DMA with length 0, outside RAM or outside ARAM, and undocumented offsets.
// Model choices (docs/ESTADO.md): RES completes at once (RES reads 0); an
// ARAM DMA takes `aram_cycles_per_unit` per 32-byte unit (default 0).
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/dsp/dsp_core.hpp"
#include "orca/hw/processor_interface.hpp"

#include <array>
#include <string>
#include <vector>

namespace orca::hw {

namespace dsp {
inline constexpr u32 kBase = 0x0C005000U;
inline constexpr u32 kSize = 0x200U;
inline constexpr u32 kAramSize = 16U * 1024U * 1024U; // GameCube internal ARAM
inline constexpr u32 DSPMBH = 0x00, DSPMBL = 0x02, CPUMBH = 0x04, CPUMBL = 0x06, CSR = 0x0A, AR_SIZE = 0x12,
                     AR_MODE = 0x16, AR_REFRESH = 0x1A, MMADDR_H = 0x20, MMADDR_L = 0x22, ARADDR_H = 0x24,
                     ARADDR_L = 0x26, CNT_H = 0x28, CNT_L = 0x2A, AIDMA_H = 0x30, AIDMA_L = 0x32, AIDMA_CTRL = 0x36,
                     AIDMA_LEFT = 0x3A;
inline constexpr u16 CSR_RES = 1U << 0, CSR_PIINT = 1U << 1, CSR_HALT = 1U << 2, CSR_AIDINT = 1U << 3,
                     CSR_AIDINTMASK = 1U << 4, CSR_ARINT = 1U << 5, CSR_ARINTMASK = 1U << 6, CSR_DSPINT = 1U << 7,
                     CSR_DSPINTMASK = 1U << 8, CSR_DMABUSY = 1U << 9, CSR_BIT11 = 1U << 11;
inline constexpr u32 kCntToMain = 0x80000000U;
} // namespace dsp

class DspInterface final : public mem::MmioDevice, private ::orca::dsp::DspHost {
public:
    // `has_aram` = false on Wii (no ARAM there).
    DspInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi, bool has_aram);

    void set_aram_cycles_per_unit(u64 c) noexcept { aram_cycles_per_unit_ = c; }
    std::vector<u8>& aram() noexcept { return aram_; }
    bool aram_busy() const noexcept { return (csr_ & dsp::CSR_DMABUSY) != 0U; }
    u16 csr() const noexcept { return csr_; }
    ::orca::dsp::DspCore& core() noexcept { return core_; }
    bool dsp_failed() const noexcept { return core_.failed(); }
    const std::string& dsp_error() const noexcept { return core_.error(); }
    void raise_dspint() { csr_ |= dsp::CSR_DSPINT; update_interrupt(); }
    static constexpr u64 kCpuCyclesPerDspCycle = 6;
    static constexpr u64 kSliceInstructions = 100;

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "DSP"; }
    std::string mmio_error() const override { return last_error_; }

private:
    bool read16(u32 offset, u16& v);
    bool write16(u32 offset, u16 v);
    bool start_aram_dma();
    void complete_aram_dma();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }
    void dsp_reset(bool boot_from_ram);
    void kick_dsp();
    void run_slice();
    // DspHost
    bool dsp_dma_read(u32 physical, std::span<u8> out) override;
    bool dsp_dma_write(u32 physical, std::span<const u8> in) override;
    void dsp_interrupt_cpu() override { raise_dspint(); }
    void dsp_halted() override { csr_ |= dsp::CSR_HALT; }

    mem::Bus& bus_;
    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    std::vector<u8> aram_;
    u64 aram_cycles_per_unit_{0};
    ::orca::dsp::DspCore core_{*this};
    bool slice_pending_{false};
    u16 csr_{dsp::CSR_HALT}, ar_size_{0}, ar_mode_{0}, ar_refresh_{0};
    u32 mmaddr_{0}, araddr_{0}, cnt_{0};
    u16 aidma_h_{0}, aidma_l_{0}, aidma_ctrl_{0};
    std::string last_error_;
};

} // namespace orca::hw
