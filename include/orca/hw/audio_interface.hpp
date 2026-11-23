// Orca Core — Audio Interface (AI), physical 0x0C006C00.
//   0x00 AICR: PSTAT(0: streaming plays) AISFR(1: streaming rate) AIINTMSK(2)
//              AIINT(3, w1c) AIINTVLD(4: 1 = AIINT holds its value)
//              SCRESET(5: write 1 -> AISCNT = 0) AIDFR(6: DMA rate)
//   0x04 AIVR: streaming volume, left in bits 0-7, right in bits 8-15
//   0x08 AISCNT: streaming samples played while PSTAT = 1 (read-only)
//   0x0C AIIT: AIINT is requested when AISCNT reaches this value
// Rates: AISFR = 1 is 48 kHz and 0 is 32 kHz (inverted with respect to YAGCD;
// Dolphin's AudioInterface.cpp documents that games behave this way); AIDFR = 1
// is 32 kHz. The real rates are 108 MHz / divisor with divisor 2248 (GameCube) or
// 2250 (Wii) for "48 kHz" and 3/2 of it for "32 kHz" (GameCube: 48043 / 32029 Hz).
// The sample counter advances in exact proportion to CPU time. While PSTAT = 1
// the streaming source (the drive's DTK audio) is read in blocks of 28 samples
// at the streaming rate and handed, with AIVR, to the streaming sink (mixer).
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"

#include <functional>
#include <span>
#include <string>

namespace orca::hw {

namespace ai {
inline constexpr u32 kOffsetInPage = 0xC00U;
inline constexpr u32 PSTAT = 0x01U, AISFR = 0x02U, AIINTMSK = 0x04U, AIINT = 0x08U, AIINTVLD = 0x10U, SCRESET = 0x20U,
                     AIDFR = 0x40U;
inline constexpr u32 AFR = AISFR, DSPRATE = AIDFR; // older names
inline constexpr u64 kClock = 108000000ULL;       // divisors below count cycles of this clock
inline constexpr u32 kSamplesPerStreamBlock = 28;
} // namespace ai

class AudioInterface final : public mem::MmioDevice {
public:
    AudioInterface(core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz, bool wii = false);
    u32 sample_count() const;
    bool stream_rate_48k() const noexcept { return (aicr_ & ai::AISFR) != 0U; }
    u32 stream_divisor() const noexcept { return divisor(stream_rate_48k()); }
    // AIDFR (bit 6): rate of the AI DMA fed by the DSP; 1 = 32 kHz
    bool dma_rate_32khz() const noexcept { return (aicr_ & ai::AIDFR) != 0U; }
    u32 dma_divisor() const noexcept { return divisor(!dma_rate_32khz()); }
    u32 volume() const noexcept { return aivr_; }

    // Streaming audio: `source` fills up to n stereo frames and returns how many;
    // `sink` receives each block (frames, the divisor of its rate, AIVR).
    using StreamSource = std::function<std::size_t(std::span<s16>, std::size_t)>;
    using StreamSink = std::function<void(std::span<const s16>, u32 divisor, u32 aivr)>;
    void set_stream_source(StreamSource s) { source_ = std::move(s); }
    void set_stream_sink(StreamSink s) { sink_ = std::move(s); }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "AI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    u32 divisor(bool rate48) const noexcept { const u32 d = wii_ ? 2250U : 2248U; return rate48 ? d : d * 3U / 2U; }
    u64 samples_in(u64 cycles) const noexcept; // whole samples played in `cycles` CPU cycles
    u64 cycles_for(u64 samples) const noexcept; // first cycle count at which `samples` are complete
    void rebase();       // folds elapsed samples into base_count_ at the current time
    void reschedule();
    void schedule_stream();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    u64 cpu_hz_;
    bool wii_;
    u32 aicr_{0}, aivr_{0}, aiit_{0};
    u32 base_count_{0};
    u64 base_cycle_{0};
    u64 base_elapsed_{0}; // samples from base_cycle_ already handed to the stream
    core::Scheduler::EventId event_{0}, stream_event_{0};
    StreamSource source_;
    StreamSink sink_;
    std::string last_error_;
};

} // namespace orca::hw
