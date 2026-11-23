// Orca Core — parts shared by the AX ucodes. See hle_ax_common.hpp.
#include "hle_ax_common.hpp"

namespace orca::dsp::hle::ax {

void AxBase::mail(u32 m) {
    if (task_switching()) { task_switch_param(m); return; }
    switch (state_) {
    case State::Size:
        if ((m & 0xFFFF0000U) == 0xBABE0000U) { size_ = m & 0xFFFFU; state_ = State::Address; }
        else bad_mail(m, "a command list (0xBABExxxx)");
        break;
    case State::Address:
        if (!copy_list(m, size_)) return;
        run_list();
        if (dsp_.failed()) return;
        // the CPU is told when the list is done; Dolphin's delay (minimum seen in games)
        dsp_.push_mail(kMailYield, true, 2500);
        state_ = State::Task;
        break;
    case State::Task: {
        const u32 t = 0xCDD10000U | (m & 0xFFFFU); // the ucode does not check the prefix
        if (t == kCpuResume) { dsp_.push_mail(kMailResume, true); state_ = State::Size; }
        else if (t == kCpuNewUcode) { begin_task_switch(); state_ = State::Size; }
        else if (t == kCpuReset) dsp_.reset(false);
        else if (t == kCpuContinue) state_ = State::Size;
        else bad_mail(m, "a task mail (0xCDD1000x)");
        break;
    }
    }
}

void AxBase::bad_mail(u32 m, const char* expected) {
    char buf[128];
    std::snprintf(buf, sizeof buf, "DSP HLE %s: mail %08X where %s was expected", name(), m, expected);
    dsp_.fail(buf);
}

bool AxBase::fault(u32 addr) {
    char buf[96];
    std::snprintf(buf, sizeof buf, "DSP HLE %s: access outside RAM at %08X", name(), addr);
    dsp_.fail(buf);
    return false;
}

bool AxBase::read16s(u32 addr, u16* out, std::size_t n) {
    std::vector<u8> b(2U * n);
    if (!dsp_.host().ram_read(addr & 0x1FFFFFFFU, b)) return fault(addr);
    for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<u16>(b[2 * i] << 8 | b[2 * i + 1]);
    return true;
}

bool AxBase::write16s(u32 addr, const u16* in, std::size_t n) {
    std::vector<u8> b(2U * n);
    for (std::size_t i = 0; i < n; ++i) { b[2 * i] = static_cast<u8>(in[i] >> 8); b[2 * i + 1] = static_cast<u8>(in[i]); }
    return dsp_.host().ram_write(addr & 0x1FFFFFFFU, b) || fault(addr);
}

bool AxBase::read32s(u32 addr, s32* out, std::size_t n) {
    std::vector<u8> b(4U * n);
    if (!dsp_.host().ram_read(addr & 0x1FFFFFFFU, b)) return fault(addr);
    for (std::size_t i = 0; i < n; ++i)
        out[i] = static_cast<s32>(u32{b[4 * i]} << 24 | u32{b[4 * i + 1]} << 16 | u32{b[4 * i + 2]} << 8 | b[4 * i + 3]);
    return true;
}

bool AxBase::write32s(u32 addr, const s32* in, std::size_t n) {
    std::vector<u8> b(4U * n);
    for (std::size_t i = 0; i < n; ++i) {
        const auto v = static_cast<u32>(in[i]);
        b[4 * i] = static_cast<u8>(v >> 24); b[4 * i + 1] = static_cast<u8>(v >> 16);
        b[4 * i + 2] = static_cast<u8>(v >> 8); b[4 * i + 3] = static_cast<u8>(v);
    }
    return dsp_.host().ram_write(addr & 0x1FFFFFFFU, b) || fault(addr);
}

bool AxBase::copy_list(u32 addr, u32 size) {
    if (size >= list_.size()) { dsp_.fail(std::string("DSP HLE ") + name() + ": command list longer than 512 words"); return false; }
    return read16s(addr, list_.data(), size);
}

void AxBase::acc_setup(const u16* pb, const VoiceWords& w) {
    DspCore& c = dsp_.host().core();
    auto wr = [&](u16 reg, u16 v) { c.accelerator_write(reg, v); };
    wr(ifx::FORMAT, pb[w.format]);
    wr(ifx::ACSAH, pb[w.loop]); wr(ifx::ACSAL, pb[w.loop + 1]);
    wr(ifx::ACEAH, pb[w.end]); wr(ifx::ACEAL, pb[w.end + 1]);
    wr(ifx::ACCAH, pb[w.cur]); wr(ifx::ACCAL, pb[w.cur + 1]);
    for (u16 k = 0; k < 16; ++k) wr(static_cast<u16>(0xFFA0U + k), pb[w.coefs + k]);
    wr(ifx::GAIN, pb[w.gain]);
    wr(ifx::PRED_SCALE, pb[w.pred_scale]);
    wr(ifx::YN1, pb[w.yn1]);
    wr(ifx::YN2, pb[w.yn2]);
    c.clear_pending_exceptions();
}

s16 AxBase::acc_sample(u16* pb, const VoiceWords& w) {
    DspCore& c = dsp_.host().core();
    u16 v = 0;
    if (!c.accelerator_read(ifx::ACDSAMP, v)) { dsp_.fail(std::string("DSP HLE ") + name() + ": accelerator: " + c.error()); return 0; }
    if ((c.pending_exceptions() & (1U << 5)) != 0U) { // end of the sample
        c.clear_pending_exceptions();
        if (pb[w.looping] != 0U) {
            c.accelerator_write(ifx::PRED_SCALE, pb[w.loop_ps]);
            u16 y1 = 0, y2 = 0;
            c.accelerator_read(ifx::YN1, y1);
            c.accelerator_read(ifx::YN2, y2);
            if (pb[w.is_stream] != 1U) { y1 = pb[w.loop_yn1]; y2 = pb[w.loop_yn2]; }
            else if (w.loop_counter >= 0) ++pb[static_cast<unsigned>(w.loop_counter)];
            c.accelerator_write(ifx::YN1, y1);
            c.accelerator_write(ifx::YN2, y2); // writing YN2 lets the accelerator read again
        } else {
            pb[w.running] = 0;
        }
    }
    return static_cast<s16>(v);
}

void AxBase::acc_store(u16* pb, const VoiceWords& w) {
    DspCore& c = dsp_.host().core();
    c.accelerator_read(ifx::ACCAH, pb[w.cur]);
    c.accelerator_read(ifx::ACCAL, pb[w.cur + 1]);
    c.accelerator_read(ifx::YN1, pb[w.yn1]);
    c.accelerator_read(ifx::YN2, pb[w.yn2]);
    c.accelerator_read(ifx::PRED_SCALE, pb[w.pred_scale]);
}

} // namespace orca::dsp::hle::ax
