// Orca Core — Audio Interface (AI), physical 0x0C006C00 (YAGCD 5.10).
//   0x00 AICR: PSTAT(0) AFR(1: 0 = 48 kHz, 1 = 32 kHz) AIINTMSK(2) AIINT(3, w1c)
//              AIINTVLD(4: 1 = AIINT holds its value) SCRESET(5: write 1 -> AISCNT = 0)
//              DSP sample rate(6)
//   0x04 AIVR: volume (stored)
//   0x08 AISCNT: stereo samples output while PSTAT = 1 (read-only)
//   0x0C AIIT: AIINT is requested when AISCNT reaches this value
// The sample counter advances in exact proportion to CPU time at the AFR rate.
// AIINT with AIINTMSK raises the PI AI cause. No audio is produced: streaming
// audio data and the DSP audio DMA belong to future milestones.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"

#include <string>

namespace orca::hw {

namespace ai {
inline constexpr u32 kOffsetInPage = 0xC00U;
inline constexpr u32 PSTAT = 0x01U, AFR = 0x02U, AIINTMSK = 0x04U, AIINT = 0x08U, AIINTVLD = 0x10U, SCRESET = 0x20U, DSPRATE = 0x40U;
} // namespace ai

class AudioInterface final : public mem::MmioDevice {
public:
    AudioInterface(core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz);
    u32 sample_count() const;
    u32 rate() const noexcept { return (aicr_ & ai::AFR) != 0U ? 32000U : 48000U; }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "AI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    void rebase();       // folds elapsed samples into base_count_ at the current time
    void reschedule();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    u64 cpu_hz_;
    u32 aicr_{0}, aivr_{0}, aiit_{0};
    u32 base_count_{0};
    u64 base_cycle_{0};
    core::Scheduler::EventId event_{0};
    std::string last_error_;
};

} // namespace orca::hw
