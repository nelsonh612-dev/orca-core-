#include "orca/hw/dsp_interface.hpp"
#include "orca/core/host_timer.hpp"

#include <cstdio>
#include <cstring>

namespace orca::hw {

DspInterface::DspInterface(mem::Bus& bus, core::Scheduler& scheduler, ProcessorInterface& pi, bool has_aram,
                           u64 cpu_hz)
    : bus_(bus), scheduler_(scheduler), pi_(pi), cpu_hz_(cpu_hz), wii_(!has_aram) {
    if (has_aram) aram_.assign(dsp::kAramSize, 0U);
}

void DspInterface::update_interrupt() {
    const bool a = (csr_ & dsp::CSR_AIDINT) != 0U && (csr_ & dsp::CSR_AIDINTMASK) != 0U;
    const bool r = (csr_ & dsp::CSR_ARINT) != 0U && (csr_ & dsp::CSR_ARINTMASK) != 0U;
    const bool d = (csr_ & dsp::CSR_DSPINT) != 0U && (csr_ & dsp::CSR_DSPINTMASK) != 0U;
    if (a || r || d) pi_.raise(pi::CAUSE_DSP); else pi_.lower(pi::CAUSE_DSP);
}

bool DspInterface::dsp_dma_read(u32 pa, std::span<u8> out) {
    return bus_.is_ram(pa, static_cast<u32>(out.size())) && bus_.dma_read(pa, out) == mem::BusStatus::Ok;
}
bool DspInterface::dsp_dma_write(u32 pa, std::span<const u8> in) {
    return bus_.is_ram(pa, static_cast<u32>(in.size())) && bus_.dma_write(pa, in) == mem::BusStatus::Ok;
}

bool DspInterface::dsp_aram_read8(u32 a, u8& v) {
    if (a >= aram_.size()) return false; // also covers Wii (no ARAM)
    v = aram_[a];
    return true;
}
bool DspInterface::dsp_aram_write16(u32 a, u16 v) {
    if (aram_.size() < 2U || a > aram_.size() - 2U) return false;
    aram_[a] = static_cast<u8>(v >> 8);
    aram_[a + 1U] = static_cast<u8>(v);
    return true;
}

void DspInterface::dsp_reset(bool boot_from_ram) {
    core_.clear_mailboxes();
    if (boot_from_ram) {
        // DSP manual "Initialization": 1024 bytes from main memory 0x01000000 to IMEM 0.
        std::array<u8, 1024> boot{};
        if (bus_.is_ram(0x01000000U, 1024U)) (void)bus_.dma_read(0x01000000U, boot);
        for (unsigned i = 0; i < 512; ++i)
            core_.iram()[i] = static_cast<u16>((static_cast<u16>(boot[2 * i]) << 8) | boot[2 * i + 1]);
        core_.reset(0x0000);
    } else {
        core_.reset(::orca::dsp::kIromBase);
    }
}

void DspInterface::use_hle(bool on) {
    ++hle_generation_;
    if (!on) { hle_.reset(); return; }
    ::orca::dsp::hle::Host& host = *this;
    hle_ = std::make_unique<::orca::dsp::hle::Dsp>(host, wii_);
    hle_->set_halted((csr_ & dsp::CSR_HALT) != 0U);
    hle_tick();
}

void DspInterface::interrupt_cpu(u64 cycles) {
    if (cycles == 0U) { raise_dspint(); return; }
    scheduler_.schedule_in(cycles, "DSP HLE interrupt", [this](u64, u64) { raise_dspint(); });
}

// HLE: one update per millisecond of emulated time
void DspInterface::hle_tick() {
    const u64 gen = hle_generation_;
    scheduler_.schedule_in(cpu_hz_ / 1000U, "DSP HLE update", [this, gen](u64, u64) {
        if (gen != hle_generation_ || !hle_) return;
        {
            const core::HostTimer timer(host_ns_);
            hle_->update();
        }
        hle_tick();
    });
}

void DspInterface::kick_dsp() {
    if (slice_pending_ || core_.halted() || core_.failed()) return;
    slice_pending_ = true;
    scheduler_.schedule_in(kSliceInstructions * kCpuCyclesPerDspCycle, "DSP slice", [this](u64, u64) { run_slice(); });
}

void DspInterface::run_slice() {
    slice_pending_ = false;
    {
        const core::HostTimer timer(host_ns_);
        (void)core_.run(kSliceInstructions);
    }
    kick_dsp();
}

bool DspInterface::read16(u32 offset, u16& v) {
    if (hle_) {
        switch (offset) {
        case dsp::DSPMBH: v = hle_->dsp_mailbox_high(); return true;
        case dsp::DSPMBL: v = hle_->dsp_mailbox_low(); return true;
        case dsp::CPUMBH: v = hle_->cpu_mailbox_high(); return true;
        case dsp::CPUMBL: v = hle_->cpu_mailbox_low(); update_interrupt(); return true;
        default: break;
        }
    }
    switch (offset) {
    case dsp::DSPMBH: v = core_.cpu_read_cmb_high(); return true; // bit 15: not yet read by the DSP
    case dsp::DSPMBL: v = core_.cpu_read_cmb_low(); return true;
    case dsp::CPUMBH: v = core_.cpu_read_dmb_high(); return true;
    case dsp::CPUMBL: v = core_.cpu_read_dmb_low(); return true;   // reading the low half consumes the mail
    case dsp::CSR: v = csr_; return true;
    case dsp::AR_SIZE: v = ar_size_; return true;
    case dsp::AR_MODE: v = 1; return true; // read-only: ARAM controller initialised
    case dsp::AR_REFRESH: v = ar_refresh_; return true;
    case dsp::MMADDR_H: v = static_cast<u16>(mmaddr_ >> 16); return true;
    case dsp::MMADDR_L: v = static_cast<u16>(mmaddr_); return true;
    case dsp::ARADDR_H: v = static_cast<u16>(araddr_ >> 16); return true;
    case dsp::ARADDR_L: v = static_cast<u16>(araddr_); return true;
    case dsp::CNT_H: v = static_cast<u16>(cnt_ >> 16); return true;
    case dsp::CNT_L: v = static_cast<u16>(cnt_); return true;
    case dsp::AIDMA_H: v = aidma_h_; return true;
    case dsp::AIDMA_L: v = aidma_l_; return true;
    case dsp::AIDMA_CTRL: v = aidma_ctrl_; return true;
    case dsp::AIDMA_LEFT: v = aid_remaining_ > 0U ? static_cast<u16>(aid_remaining_ - 1U) : 0U; return true;
    default: return fail("DSP: register not implemented");
    }
}

bool DspInterface::write16(u32 offset, u16 v) {
    if (hle_ && (offset == dsp::DSPMBH || offset == dsp::DSPMBL)) {
        const core::HostTimer timer(host_ns_);
        if (offset == dsp::DSPMBH) hle_->cpu_write_high(v);
        else hle_->cpu_write_low(v); // a ucode's work (an AX command list) runs here
        return true;
    }
    switch (offset) {
    case dsp::DSPMBH: core_.cpu_write_cmb_high(v); return true;
    case dsp::DSPMBL: core_.cpu_write_cmb_low(v); return true; // mail ready for the DSP
    case dsp::CPUMBH: case dsp::CPUMBL: return fail("DSP: the CPU mailbox is read-only");
    case dsp::CSR: {
        const u16 rw = dsp::CSR_HALT | dsp::CSR_AIDINTMASK | dsp::CSR_ARINTMASK | dsp::CSR_DSPINTMASK | dsp::CSR_BIT11;
        const u16 w1c = dsp::CSR_AIDINT | dsp::CSR_ARINT | dsp::CSR_DSPINT;
        const bool init_cleared = (csr_ & dsp::CSR_BIT11) != 0U && (v & dsp::CSR_BIT11) == 0U;
        csr_ = static_cast<u16>((csr_ & ~rw) | (v & rw));
        csr_ = static_cast<u16>(csr_ & ~(v & w1c));
        // RES restarts the DSP at its boot ROM (Dolphin DSPHLE: SetUCode(UCODE_ROM);
        // the LLE core starts at the ROM reset vector).
        if ((v & dsp::CSR_RES) != 0U) {
            if (hle_) hle_->reset(false);
            else dsp_reset(false); // completes at once (model)
            aidma_ctrl_ = 0;       // Dolphin: RES clears the AI DMA control
            ++aid_generation_;
        }
        // Clearing DSPInit (bit 11, set at power-on) runs the init code that
        // __OSInitAudioSystem placed at main memory 0x01000000: 1 KiB copied to IMEM 0
        // and started there; the HLE runs the known init ucode (mail 0x80544348).
        // Bit 10 then reads 1 for a short while (Dolphin: 130 timebase ticks,
        // measured on a Wii), which the SDK waits out.
        if (init_cleared) {
            if (hle_) hle_->reset(true);
            else dsp_reset(true);
            csr_ |= dsp::CSR_INITCODE;
            const u64 gen = ++init_generation_;
            scheduler_.schedule_in(130U * 12U, "DSP init code", [this, gen](u64, u64) {
                if (gen == init_generation_) csr_ = static_cast<u16>(csr_ & ~dsp::CSR_INITCODE);
            });
        }
        if (hle_) { // PIINT wakes DSP code; the HLE ucodes react to mail only
            hle_->set_halted((csr_ & dsp::CSR_HALT) != 0U);
            update_interrupt();
            return true;
        }
        if ((v & dsp::CSR_PIINT) != 0U) core_.request_external_interrupt();
        core_.set_halted((csr_ & dsp::CSR_HALT) != 0U);
        update_interrupt();
        kick_dsp();
        return true;
    }
    case dsp::AR_SIZE: ar_size_ = static_cast<u16>(v & 0x007FU); return true;
    case dsp::AR_MODE: return true; // read-only register: the write has no effect
    case dsp::AR_REFRESH: ar_refresh_ = static_cast<u16>(v & 0x07FFU); return true;
    case dsp::MMADDR_H: case dsp::MMADDR_L: case dsp::ARADDR_H: case dsp::ARADDR_L: case dsp::CNT_H:
        if (aram_busy()) return fail("DSP: ARAM DMA registers written during a transfer");
        if (offset == dsp::MMADDR_H) mmaddr_ = (mmaddr_ & 0xFFFFU) | (static_cast<u32>(v & 0x03FFU) << 16);
        else if (offset == dsp::MMADDR_L) mmaddr_ = (mmaddr_ & 0xFFFF0000U) | (v & 0xFFE0U);
        else if (offset == dsp::ARADDR_H) araddr_ = (araddr_ & 0xFFFFU) | (static_cast<u32>(v & 0x03FFU) << 16);
        else if (offset == dsp::ARADDR_L) araddr_ = (araddr_ & 0xFFFF0000U) | (v & 0xFFE0U);
        else cnt_ = (cnt_ & 0xFFFFU) | (static_cast<u32>(v & 0x83FFU) << 16);
        return true;
    case dsp::CNT_L:
        if (aram_busy()) return fail("DSP: ARAM DMA registers written during a transfer");
        cnt_ = (cnt_ & 0xFFFF0000U) | (v & 0xFFE0U);
        return start_aram_dma();
    case dsp::AIDMA_H: aidma_h_ = static_cast<u16>(v & (wii_ ? 0x1FFFU : 0x03FFU)); return true;
    case dsp::AIDMA_L: aidma_l_ = static_cast<u16>(v & 0xFFE0U); return true;
    case dsp::AIDMA_CTRL: {
        const bool was = (aidma_ctrl_ & 0x8000U) != 0U;
        aidma_ctrl_ = v;
        if (!was && (v & 0x8000U) != 0U) {
            aid_cur_ = (static_cast<u32>(aidma_h_) << 16) | aidma_l_;
            aid_remaining_ = static_cast<u16>(v & 0x7FFFU);
            const u64 gen = ++aid_generation_;
            scheduler_.schedule_in(200, "AI DMA start", [this](u64, u64) { csr_ |= dsp::CSR_AIDINT; update_interrupt(); });
            scheduler_.schedule_in(aid_block_cycles(), "AI DMA block", [this, gen](u64, u64) {
                if (gen == aid_generation_) audio_block();
            });
        } // clearing the enable stops the chain: audio_block() returns without rescheduling
        return true;
    }
    default: return fail("DSP: register not implemented");
    }
}

u64 DspInterface::aid_block_cycles() const {
    // 8 stereo samples per 32-byte block; sample period = divisor / 108 MHz
    // (Dolphin AudioInterface.cpp: 48 kHz divisor 2248 on GameCube, 2250 on Wii; 32 kHz = 3/2 of it)
    const bool is32 = aid_32khz_ ? aid_32khz_() : true;
    const u64 d48 = wii_ ? 2250U : 2248U;
    const u64 divisor = is32 ? d48 * 3U / 2U : d48;
    return 8U * cpu_hz_ * divisor / 108000000ULL;
}

void DspInterface::audio_block() {
    if ((aidma_ctrl_ & 0x8000U) == 0U) return;
    std::array<u8, 32> block{};
    if (bus_.is_ram(aid_cur_, 32U)) (void)bus_.dma_read(aid_cur_, block);
    if (audio_sink_) audio_sink_(block);
    ++audio_blocks_;
    if (aid_remaining_ != 0U) {
        --aid_remaining_;
        aid_cur_ += 32U;
    }
    if (aid_remaining_ == 0U) { // relatch and interrupt
        aid_cur_ = (static_cast<u32>(aidma_h_) << 16) | aidma_l_;
        aid_remaining_ = static_cast<u16>(aidma_ctrl_ & 0x7FFFU);
        csr_ |= dsp::CSR_AIDINT;
        update_interrupt();
    }
    const u64 gen = aid_generation_;
    scheduler_.schedule_in(aid_block_cycles(), "AI DMA block", [this, gen](u64, u64) {
        if (gen == aid_generation_) audio_block();
    });
}

bool DspInterface::start_aram_dma() {
    char buf[160];
    const u32 len = cnt_ & 0x7FFFFFE0U;
    if (len == 0U) return fail("DSP: ARAM DMA with length 0 (behaviour undocumented)");
    if (wii_) {
        // Wii: no ARAM chip; the "ARAM" side of this DMA is MEM2 (Dolphin's wii_mode:
        // the ARAM pointer is the 64 MiB EXRAM). AR address = offset into MEM2.
        if (!bus_.is_ram(dsp::kWiiAramBase + araddr_, len)) {
            std::snprintf(buf, sizeof buf, "DSP: ARAM DMA range %08X+%X is outside MEM2 (Wii)", araddr_, len);
            return fail(buf);
        }
    }
    // GameCube: an AR address at or above the 16 MiB of ARAM goes to the high-speed
    // port (HSP) under the console, where nothing is attached here; one that starts
    // inside ARAM and runs past its end wraps within it (Dolphin DSP.cpp: the AR
    // address is mirrored every 64 MiB, ARAM is indexed with its size mask, and an
    // address past the ARAM size goes to the HSP device, the null one by default).
    if (!bus_.is_ram(mmaddr_, len)) {
        std::snprintf(buf, sizeof buf, "DSP: ARAM DMA main-memory range %08X+%X is not RAM", mmaddr_, len);
        return fail(buf);
    }
    csr_ |= dsp::CSR_DMABUSY;
    scheduler_.schedule_in(aram_cycles_per_unit_ * (len / 32U), "ARAM DMA", [this](u64, u64) { complete_aram_dma(); });
    return true;
}

void DspInterface::complete_aram_dma() {
    const u32 len = cnt_ & 0x7FFFFFE0U;
    if (wii_) {
        std::vector<u8> data(len);
        const bool to_main = (cnt_ & dsp::kCntToMain) != 0U;
        const u32 ar = dsp::kWiiAramBase + araddr_;
        (void)bus_.dma_read(to_main ? ar : mmaddr_, data);
        (void)bus_.dma_write(to_main ? mmaddr_ : ar, data);
        if (!to_main && (ar_size_ & 0xFU) == 4U) // same mirroring of the low 4 MiB as on GameCube
            for (u32 off = 0; off < len; off += 32U)
                if (araddr_ + off < 0x400000U)
                    (void)bus_.dma_write(ar + off + 0x400000U, std::span<const u8>(data.data() + off, 32U));
    } else if (araddr_ >= aram_.size()) {
        // Expansion (HSP) port with no device: a write is not stored anywhere and a
        // read drives no data, so main memory keeps what it held (Dolphin's null HSP
        // device). The SDK's ARAM size check (ARCheckSize) probes here.
        ++hsp_empty_transfers_;
    } else if ((cnt_ & dsp::kCntToMain) != 0U) {
        std::vector<u8> data(len);
        for (u32 i = 0; i < len; ++i) data[i] = aram_[(araddr_ + i) & (dsp::kAramSize - 1U)];
        (void)bus_.dma_write(mmaddr_, data);
    } else {
        std::vector<u8> data(len);
        (void)bus_.dma_read(mmaddr_, data);
        for (u32 i = 0; i < len; ++i) aram_[(araddr_ + i) & (dsp::kAramSize - 1U)] = data[i];
        // (AR_SIZE & 0xF) == 4: writes below 4 MiB also land 4 MiB higher (Dolphin DSP.cpp)
        if ((ar_size_ & 0xFU) == 4U) {
            for (u32 off = 0; off < len; off += 32U) {
                const u32 a = araddr_ + off;
                if (a < 0x400000U)
                    for (u32 i = 0; i < 32U; ++i) aram_[(a + 0x400000U + i) & (dsp::kAramSize - 1U)] = data[off + i];
            }
        }
    }
    mmaddr_ += len;
    araddr_ = (araddr_ + len) & 0x03FFFFFFU; // the register holds 26 bits
    cnt_ &= dsp::kCntToMain; // count reaches 0; the direction bit is kept (model)
    csr_ = static_cast<u16>((csr_ & ~dsp::CSR_DMABUSY) | dsp::CSR_ARINT);
    update_interrupt();
}

bool DspInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size == 2U) { u16 v = 0; if (!read16(offset, v)) return false; value = v; return true; }
    if (size == 4U) {
        u16 h = 0, l = 0;
        if (!read16(offset, h) || !read16(offset + 2U, l)) return false;
        value = (static_cast<u32>(h) << 16) | l;
        return true;
    }
    return fail("DSP: only 16- and 32-bit accesses are implemented");
}

bool DspInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size == 2U) return write16(offset, static_cast<u16>(value));
    if (size == 4U) return write16(offset, static_cast<u16>(value >> 16)) && write16(offset + 2U, static_cast<u16>(value));
    return fail("DSP: only 16- and 32-bit accesses are implemented");
}

} // namespace orca::hw
