#include "orca/hw/audio_interface.hpp"

#include <array>

namespace orca::hw {

AudioInterface::AudioInterface(core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz, bool wii)
    : scheduler_(scheduler), pi_(pi), cpu_hz_(cpu_hz), wii_(wii) {}

u64 AudioInterface::samples_in(u64 cycles) const noexcept {
    // cycles / cpu_hz seconds * kClock / divisor samples
    const u128 n = static_cast<u128>(cycles) * ai::kClock;
    return static_cast<u64>(n / (static_cast<u128>(cpu_hz_) * stream_divisor()));
}

u64 AudioInterface::cycles_for(u64 samples) const noexcept {
    const u128 num = static_cast<u128>(samples) * cpu_hz_ * stream_divisor();
    return static_cast<u64>((num + ai::kClock - 1U) / ai::kClock);
}

u32 AudioInterface::sample_count() const {
    if ((aicr_ & ai::PSTAT) == 0U) return base_count_;
    return base_count_ + static_cast<u32>(samples_in(scheduler_.now() - base_cycle_));
}

void AudioInterface::rebase() {
    base_count_ = sample_count();
    base_cycle_ = scheduler_.now();
    base_elapsed_ = 0;
}

void AudioInterface::update_interrupt() {
    if ((aicr_ & ai::AIINT) != 0U && (aicr_ & ai::AIINTMSK) != 0U) pi_.raise(pi::CAUSE_AI);
    else pi_.lower(pi::CAUSE_AI);
}

void AudioInterface::reschedule() {
    if (event_ != 0U) { scheduler_.cancel(event_); event_ = 0; }
    if ((aicr_ & ai::PSTAT) == 0U) return;
    const u32 now_count = sample_count();
    u64 delta = static_cast<u32>(aiit_ - now_count); // samples until AISCNT == AIIT (with 32-bit wrap)
    if (delta == 0U) delta = 0x100000000ULL;          // just matched: next match after a full wrap
    const u64 target_samples = static_cast<u64>(now_count - base_count_) + delta;
    event_ = scheduler_.schedule_at(base_cycle_ + cycles_for(target_samples), "AI AIIT", [this](u64, u64) {
        event_ = 0;
        if ((aicr_ & ai::AIINTVLD) == 0U) aicr_ |= ai::AIINT;
        update_interrupt();
        reschedule();
    });
}

// The streaming source is read one 28-sample block at a time, when the block has
// been played (its last sample is due), so the data reaches the mixer in time order.
void AudioInterface::schedule_stream() {
    if (stream_event_ != 0U) { scheduler_.cancel(stream_event_); stream_event_ = 0; }
    if ((aicr_ & ai::PSTAT) == 0U || !sink_) return;
    const u64 next = base_elapsed_ + ai::kSamplesPerStreamBlock;
    stream_event_ = scheduler_.schedule_at(base_cycle_ + cycles_for(next), "AI stream", [this](u64, u64) {
        stream_event_ = 0;
        std::array<s16, 2U * ai::kSamplesPerStreamBlock> pcm{};
        const std::size_t got = source_ ? source_(pcm, ai::kSamplesPerStreamBlock) : 0U;
        for (std::size_t k = 2U * got; k < pcm.size(); ++k) pcm[k] = 0; // nothing playing: silence
        base_elapsed_ += ai::kSamplesPerStreamBlock;
        sink_(pcm, stream_divisor(), aivr_);
        schedule_stream();
    });
}

bool AudioInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return fail("AI: only 32-bit accesses are implemented");
    switch (offset) {
    case 0x00: value = aicr_; return true;
    case 0x04: value = aivr_; return true;
    case 0x08: value = sample_count(); return true;
    case 0x0C: value = aiit_; return true;
    default: return fail("AI: register not implemented");
    }
}

bool AudioInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U) return fail("AI: only 32-bit accesses are implemented");
    const auto v = static_cast<u32>(value);
    switch (offset) {
    case 0x00: {
        rebase();
        const u32 stored = ai::PSTAT | ai::AISFR | ai::AIINTMSK | ai::AIINTVLD | ai::AIDFR;
        aicr_ = (aicr_ & ai::AIINT) | (v & stored);
        if ((v & ai::AIINT) != 0U) aicr_ &= ~ai::AIINT; // write 1 to clear
        if ((v & ai::SCRESET) != 0U) base_count_ = 0;
        update_interrupt();
        reschedule();
        schedule_stream();
        return true;
    }
    case 0x04: aivr_ = v; return true;
    case 0x08: // AISCNT can be set (Dolphin: sets the counter and restarts its timing)
        rebase();
        base_count_ = v;
        reschedule();
        schedule_stream();
        return true;
    case 0x0C: aiit_ = v; reschedule(); return true;
    default: return fail("AI: register not implemented");
    }
}

} // namespace orca::hw
