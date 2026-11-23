#include "orca/hw/audio_interface.hpp"

namespace orca::hw {

AudioInterface::AudioInterface(core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz)
    : scheduler_(scheduler), pi_(pi), cpu_hz_(cpu_hz) {}

u32 AudioInterface::sample_count() const {
    if ((aicr_ & ai::PSTAT) == 0U) return base_count_;
    const u64 elapsed = scheduler_.now() - base_cycle_;
    return base_count_ + static_cast<u32>(elapsed * rate() / cpu_hz_);
}

void AudioInterface::rebase() {
    base_count_ = sample_count();
    base_cycle_ = scheduler_.now();
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
    // first cycle at which floor(elapsed * rate / cpu_hz) reaches base + target
    const u64 target_samples = static_cast<u64>(now_count - base_count_) + delta;
    const u64 cycle = (target_samples * cpu_hz_ + rate() - 1U) / rate();
    event_ = scheduler_.schedule_at(base_cycle_ + cycle, "AI AIIT", [this](u64, u64) {
        event_ = 0;
        if ((aicr_ & ai::AIINTVLD) == 0U) aicr_ |= ai::AIINT;
        update_interrupt();
        reschedule();
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
        const u32 stored = ai::PSTAT | ai::AFR | ai::AIINTMSK | ai::AIINTVLD | ai::DSPRATE;
        aicr_ = (aicr_ & ai::AIINT) | (v & stored);
        if ((v & ai::AIINT) != 0U) aicr_ &= ~ai::AIINT; // write 1 to clear
        if ((v & ai::SCRESET) != 0U) base_count_ = 0;
        update_interrupt();
        reschedule();
        return true;
    }
    case 0x04: aivr_ = v; return true;
    case 0x08: return fail("AI: AISCNT is read-only");
    case 0x0C: aiit_ = v; reschedule(); return true;
    default: return fail("AI: register not implemented");
    }
}

} // namespace orca::hw
