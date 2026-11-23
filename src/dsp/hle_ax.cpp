// Orca Core — HLE of the GameCube AX ucode (Nintendo's audio library).
//
// Behaviour as Dolphin's DSPHLE/UCodes/AX.cpp, AXVoice.h and AXStructs.h describe
// it (read as information; this is Orca's code): the CPU sends 0xBABE0000 | size
// and then the address of a command list; the ucode runs it over 5 ms of audio at
// 32 kHz (160 samples) in nine 32-bit mixing buffers (main, aux A, aux B; left,
// right, surround), processes each voice ("parameter block", PB) of a linked list
// through the shared accelerator (orca::dsp::DspCore, the same one the low-level
// emulation uses), resamples, applies the volume envelope, the low-pass filter and
// the per-bus volumes (with ramps), and finally writes 16-bit stereo to RAM for the
// AI DMA. It then sends DSP_YIELD with an interrupt.
// Not verified against the real ucode (it is not available here): see ESTADO.md.
#include "orca/dsp/hle.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace orca::dsp::hle {

namespace {

constexpr unsigned kSamples = 32 * 5; // 5 ms at 32 kHz

// PB layout in 16-bit words (the newer GameCube layout, with the low-pass filter)
namespace pbw {
inline constexpr unsigned NEXT_HI = 0, NEXT_LO = 1, SRC_TYPE = 4, COEF_SELECT = 5, MIXER_CONTROL = 6, RUNNING = 7,
                          IS_STREAM = 8, MIXER = 9, ITD_ON = 27, NUM_UPDATES = 34, UPDATES_HI = 39, UPDATES_LO = 40,
                          DPOP = 41, VOL = 50, VOL_DELTA = 51, LOOPING = 55, FORMAT = 56, LOOP_HI = 57, LOOP_LO = 58,
                          END_HI = 59, END_LO = 60, CUR_HI = 61, CUR_LO = 62, COEFS = 63, GAIN = 79, PRED_SCALE = 80,
                          YN1 = 81, YN2 = 82, RATIO_HI = 83, RATIO_LO = 84, FRAC = 85, LAST = 86, LOOP_PS = 90,
                          LOOP_YN1 = 91, LOOP_YN2 = 92, LPF_ON = 93, LPF_YN1 = 94, LPF_A0 = 95, LPF_B0 = 96,
                          LOOP_COUNTER = 97, SIZE = 122;
}
using Pb = std::array<u16, pbw::SIZE>;

// mixing-buffer indexes
enum Buf : unsigned { ML, MR, MS, AL, AR, AS, BL, BR, BS, kBufs };

// mixer_control bits (internal form)
enum : u32 {
    MIX_MAIN_L = 0x000001, MIX_MAIN_L_RAMP = 0x000002, MIX_MAIN_R = 0x000004, MIX_MAIN_R_RAMP = 0x000008,
    MIX_MAIN_S = 0x000010, MIX_MAIN_S_RAMP = 0x000020, MIX_AUXA_L = 0x000040, MIX_AUXA_L_RAMP = 0x000080,
    MIX_AUXA_R = 0x000100, MIX_AUXA_R_RAMP = 0x000200, MIX_AUXA_S = 0x000400, MIX_AUXA_S_RAMP = 0x000800,
    MIX_AUXB_L = 0x001000, MIX_AUXB_L_RAMP = 0x002000, MIX_AUXB_R = 0x004000, MIX_AUXB_R_RAMP = 0x008000,
    MIX_AUXB_S = 0x010000, MIX_AUXB_S_RAMP = 0x020000, MIX_ALL_RAMPS = 0xAAAAAA,
};

constexpr u32 kOldAx = 0x4E8A8B21U; // the oldest known version: no low-pass filter in the PB, other mixer bits

s16 clamp16(s64 v) { return static_cast<s16>(std::clamp<s64>(v, -0x8000, 0x7FFF)); }

class AxUcode final : public Ucode {
public:
    AxUcode(Dsp& dsp, u32 hash) : Ucode(dsp, hash) {}
    const char* name() const noexcept override { return "AX (GameCube)"; }
    void start() override { dsp_.push_mail(kMailInit, true); }

    void mail(u32 m) override {
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

private:
    enum class State { Size, Address, Task } state_{State::Size};

    void bad_mail(u32 m, const char* expected) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "DSP HLE AX: mail %08X where %s was expected", m, expected);
        dsp_.fail(buf);
    }

    // ---- RAM access (big-endian) ------------------------------------------------
    bool read16s(u32 addr, u16* out, std::size_t n) {
        std::vector<u8> b(2U * n);
        if (!dsp_.host().ram_read(addr & 0x1FFFFFFFU, b)) return fault(addr);
        for (std::size_t i = 0; i < n; ++i) out[i] = static_cast<u16>(b[2 * i] << 8 | b[2 * i + 1]);
        return true;
    }
    bool write16s(u32 addr, const u16* in, std::size_t n) {
        std::vector<u8> b(2U * n);
        for (std::size_t i = 0; i < n; ++i) { b[2 * i] = static_cast<u8>(in[i] >> 8); b[2 * i + 1] = static_cast<u8>(in[i]); }
        return dsp_.host().ram_write(addr & 0x1FFFFFFFU, b) || fault(addr);
    }
    bool read32s(u32 addr, s32* out, std::size_t n) {
        std::vector<u8> b(4U * n);
        if (!dsp_.host().ram_read(addr & 0x1FFFFFFFU, b)) return fault(addr);
        for (std::size_t i = 0; i < n; ++i)
            out[i] = static_cast<s32>(u32{b[4 * i]} << 24 | u32{b[4 * i + 1]} << 16 | u32{b[4 * i + 2]} << 8 | b[4 * i + 3]);
        return true;
    }
    bool write32s(u32 addr, const s32* in, std::size_t n) {
        std::vector<u8> b(4U * n);
        for (std::size_t i = 0; i < n; ++i) {
            const auto v = static_cast<u32>(in[i]);
            b[4 * i] = static_cast<u8>(v >> 24); b[4 * i + 1] = static_cast<u8>(v >> 16);
            b[4 * i + 2] = static_cast<u8>(v >> 8); b[4 * i + 3] = static_cast<u8>(v);
        }
        return dsp_.host().ram_write(addr & 0x1FFFFFFFU, b) || fault(addr);
    }
    bool fault(u32 addr) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "DSP HLE AX: access outside RAM at %08X", addr);
        dsp_.fail(buf);
        return false;
    }

    // ---- command list --------------------------------------------------------------
    bool copy_list(u32 addr, u32 size) {
        if (size >= list_.size()) { dsp_.fail("DSP HLE AX: command list longer than 512 words"); return false; }
        return read16s(addr, list_.data(), size);
    }

    void run_list() {
        u32 i = 0, pb_addr = 0;
        auto arg = [&]() -> u16 { return i < list_.size() ? list_[i++] : 0; };
        auto arg32 = [&]() -> u32 { const u32 hi = arg(); return hi << 16 | arg(); };
        for (unsigned guard = 0; guard < 100000U && !dsp_.failed(); ++guard) {
            const u16 cmd = arg();
            switch (cmd) {
            case 0x00: setup(arg32()); break;
            case 0x01: { const u32 a = arg32(); const u16 vm = arg(), va = arg(), vb = arg(); download_and_mix(a, vm, va, vb); break; }
            case 0x02: pb_addr = arg32(); break;
            case 0x03: process_pbs(pb_addr); break;
            case 0x04: case 0x05: { const u32 w = arg32(), r = arg32(); mix_aux(cmd - 0x04U, w, r); break; }
            case 0x06: upload_lrs(arg32()); break;
            case 0x07: set_main_lr(arg32(), false); break;
            case 0x09: mix_aux(1, 0, arg32()); break;
            case 0x0A: case 0x0B: case 0x0C: break; // no operation in the known versions
            case 0x0D: { const u32 a = arg32(); const u16 n = arg(); if (!copy_list(a, n)) return; i = 0; break; }
            case 0x0E: { const u32 s = arg32(), lr = arg32(); output(lr, s); break; }
            case 0x0F: return;
            case 0x10: { const u32 u = arg32(), d = arg32(); mix_auxb_lr(u, d); break; }
            case 0x11: set_main_lr(arg32(), true); break;
            case 0x12: {
                if (hash_ == kOldAx) { dsp_.fail("DSP HLE AX: compressor command in the oldest AX version (not present there)"); return; }
                const u16 threshold = arg(), frames = arg();
                compressor(threshold, frames, arg32());
                break;
            }
            case 0x13: {
                const u32 a = arg32(), b = arg32(), ml = arg32(), mr = arg32(), bl = arg32(), br = arg32();
                send_aux_and_mix(a, b, ml, mr, bl, br);
                break;
            }
            default: {
                char buf[96];
                std::snprintf(buf, sizeof buf, "DSP HLE AX: command %04X is not implemented", cmd);
                dsp_.fail(buf);
                return;
            }
            }
        }
    }

    void setup(u32 addr) {
        // per buffer: initial value (32 bits) and a per-sample delta (16 bits)
        std::array<u16, 3 * kBufs> init{};
        if (!read16s(addr, init.data(), init.size())) return;
        for (unsigned b = 0; b < kBufs; ++b) {
            const auto value = static_cast<s32>(u32{init[3 * b]} << 16 | init[3 * b + 1]);
            const auto delta = static_cast<s16>(init[3 * b + 2]);
            for (unsigned k = 0; k < kSamples; ++k) buf_[b][k] = value == 0 ? 0 : value + static_cast<s32>(k) * delta;
        }
    }

    void download_and_mix(u32 addr, u16 vm, u16 va, u16 vb) {
        const u16 vols[3] = {vm, va, vb};
        std::vector<s32> in(3U * kSamples);
        for (unsigned g = 0; g < 3; ++g) {
            if (!read32s(addr, in.data(), in.size())) return; // the same three buffers for each group
            for (unsigned c = 0; c < 3; ++c)
                for (unsigned k = 0; k < kSamples; ++k)
                    buf_[3 * g + c][k] += static_cast<s32>((static_cast<s64>(in[c * kSamples + k]) * vols[g]) >> 15);
        }
    }

    void mix_aux(unsigned id, u32 write_addr, u32 read_addr) {
        const unsigned base = id == 0U ? AL : BL;
        if (write_addr != 0U)
            for (unsigned c = 0; c < 3; ++c)
                if (!write32s(write_addr + c * kSamples * 4U, buf_[base + c].data(), kSamples)) return;
        std::vector<s32> in(3U * kSamples);
        if (!read32s(read_addr, in.data(), in.size())) return;
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned k = 0; k < kSamples; ++k) buf_[ML + c][k] += in[c * kSamples + k];
    }

    void upload_lrs(u32 addr) {
        for (unsigned c = 0; c < 3; ++c)
            if (!write32s(addr + c * kSamples * 4U, buf_[ML + c].data(), kSamples)) return;
    }

    void set_main_lr(u32 addr, bool opposite) {
        std::array<s32, kSamples> in{};
        if (!read32s(addr, in.data(), kSamples)) return;
        for (unsigned k = 0; k < kSamples; ++k) {
            buf_[ML][k] = opposite ? -in[k] : in[k];
            buf_[MR][k] = in[k];
            buf_[MS][k] = 0;
        }
    }

    void mix_auxb_lr(u32 up, u32 down) {
        if (!write32s(up, buf_[BL].data(), kSamples) || !write32s(up + kSamples * 4U, buf_[BR].data(), kSamples)) return;
        std::vector<s32> in(2U * kSamples);
        if (!read32s(down, in.data(), in.size())) return;
        for (unsigned k = 0; k < kSamples; ++k) { buf_[BL][k] = in[k]; buf_[ML][k] += in[k]; }
        for (unsigned k = 0; k < kSamples; ++k) { buf_[BR][k] = in[kSamples + k]; buf_[MR][k] += in[kSamples + k]; }
    }

    void send_aux_and_mix(u32 auxa_up, u32 auxb_s_up, u32 ml, u32 mr, u32 bl, u32 br) {
        for (unsigned c = 0; c < 3; ++c)
            if (!write32s(auxa_up + c * kSamples * 4U, buf_[AL + c].data(), kSamples)) return;
        if (!write32s(auxb_s_up, buf_[BS].data(), kSamples)) return;
        const u32 addrs[4] = {ml, mr, bl, br};
        const unsigned bufs[4] = {ML, MR, BL, BR};
        std::array<s32, kSamples> in{};
        for (unsigned j = 0; j < 4; ++j) {
            if (!read32s(addrs[j], in.data(), kSamples)) return;
            for (unsigned k = 0; k < kSamples; ++k) buf_[bufs[j]][k] += in[k];
        }
    }

    void compressor(u16 threshold, u16 release_frames, u32 table) {
        bool triggered = false;
        for (unsigned k = 0; k < kSamples && !triggered; ++k)
            triggered = std::abs(buf_[ML][k]) > static_cast<s32>(threshold) || std::abs(buf_[MR][k]) > static_cast<s32>(threshold);
        const u32 frame_bytes = kSamples * 2U;
        u32 off = 0;
        if (triggered) { off = compressor_pos_ * frame_bytes; compressor_pos_ = release_frames; }
        else if (compressor_pos_ != 0U) { --compressor_pos_; off = (11U + compressor_pos_) * frame_bytes; } // after the 11 attack ramps
        else return;
        std::array<u16, kSamples> ramp{};
        if (!read16s(table + off, ramp.data(), kSamples)) return;
        for (unsigned k = 0; k < kSamples; ++k) {
            buf_[ML][k] = static_cast<s32>((static_cast<s64>(buf_[ML][k]) * ramp[k]) >> 15);
            buf_[MR][k] = static_cast<s32>((static_cast<s64>(buf_[MR][k]) * ramp[k]) >> 15);
        }
    }

    void output(u32 lr_addr, u32 surround_addr) {
        if (!write32s(surround_addr, buf_[MS].data(), kSamples)) return;
        std::array<u16, 2 * kSamples> lr{};
        for (unsigned k = 0; k < kSamples; ++k) {
            lr[2 * k] = static_cast<u16>(clamp16(buf_[MR][k])); // right then left, as the AI reads them
            lr[2 * k + 1] = static_cast<u16>(clamp16(buf_[ML][k]));
        }
        (void)write16s(lr_addr, lr.data(), lr.size());
    }

    // ---- voices ---------------------------------------------------------------------
    bool has_lpf() const noexcept { return hash_ != kOldAx; }

    bool read_pb(u32 addr, Pb& pb) {
        pb.fill(0);
        if (has_lpf()) return read16s(addr, pb.data(), pb.size());
        // the oldest version has no low-pass filter: the words after it move up by 4
        return read16s(addr, pb.data(), pbw::LPF_ON) &&
               read16s(addr + 2U * pbw::LPF_ON, pb.data() + pbw::LOOP_COUNTER, pb.size() - pbw::LOOP_COUNTER);
    }
    bool write_pb(u32 addr, const Pb& pb) {
        if (has_lpf()) return write16s(addr, pb.data(), pb.size());
        return write16s(addr, pb.data(), pbw::LPF_ON) &&
               write16s(addr + 2U * pbw::LPF_ON, pb.data() + pbw::LOOP_COUNTER, pb.size() - pbw::LOOP_COUNTER);
    }

    u32 mixer_control(u16 mc) const noexcept {
        u32 r = 0;
        if (hash_ == kOldAx) {
            r |= MIX_MAIN_L | MIX_MAIN_R;
            if ((mc & 0x10U) != 0U) { // Dolby Pro Logic II
                if ((mc & 0x6U) == 0U) r |= MIX_AUXB_L | MIX_AUXB_R;
                if ((mc & 0x7U) == 1U) r |= MIX_AUXA_L | MIX_AUXA_R | MIX_AUXA_S;
            } else {
                if ((mc & 0x1U) != 0U) r |= MIX_AUXA_L | MIX_AUXA_R;
                if ((mc & 0x2U) != 0U) r |= MIX_AUXB_L | MIX_AUXB_R;
                if ((mc & 0x4U) != 0U) {
                    r |= MIX_MAIN_S;
                    if ((r & MIX_AUXA_L) != 0U) r |= MIX_AUXA_S;
                    if ((r & MIX_AUXB_L) != 0U) r |= MIX_AUXB_S;
                }
            }
            if ((mc & 0x8U) != 0U) r |= MIX_ALL_RAMPS;
            return r;
        }
        if ((mc & 0x0001U) != 0U) r |= MIX_MAIN_L;
        if ((mc & 0x0002U) != 0U) r |= MIX_MAIN_R;
        if ((mc & 0x0004U) != 0U) r |= MIX_MAIN_S;
        if ((mc & 0x0008U) != 0U) r |= MIX_MAIN_L_RAMP | MIX_MAIN_R_RAMP | MIX_MAIN_S_RAMP;
        if ((mc & 0x0010U) != 0U) r |= MIX_AUXA_L;
        if ((mc & 0x0020U) != 0U) r |= MIX_AUXA_R;
        if ((mc & 0x0040U) != 0U) r |= MIX_AUXA_L_RAMP | MIX_AUXA_R_RAMP;
        if ((mc & 0x0080U) != 0U) r |= MIX_AUXA_S;
        if ((mc & 0x0100U) != 0U) r |= MIX_AUXA_S_RAMP;
        if ((mc & 0x0200U) != 0U) r |= MIX_AUXB_L;
        if ((mc & 0x0400U) != 0U) r |= MIX_AUXB_R;
        if ((mc & 0x0800U) != 0U) r |= MIX_AUXB_L_RAMP | MIX_AUXB_R_RAMP;
        if ((mc & 0x1000U) != 0U) r |= MIX_AUXB_S;
        if ((mc & 0x2000U) != 0U) r |= MIX_AUXB_S_RAMP;
        return r; // 0x4000 (surround input selection for AUXB) only matters with initial time delay
    }

    void process_pbs(u32 addr) {
        unsigned guard = 0;
        while (addr != 0U && !dsp_.failed()) {
            if (++guard > 4096U) { dsp_.fail("DSP HLE AX: the PB list does not end (more than 4096 voices)"); return; }
            Pb pb{};
            if (!read_pb(addr, pb)) return;
            std::array<u16, 64> updates{}; // 32 (offset, value) pairs
            const u32 up_addr = u32{pb[pbw::UPDATES_HI]} << 16 | pb[pbw::UPDATES_LO];
            bool have_updates = false;
            for (unsigned m = 0; m < 5; ++m) have_updates = have_updates || pb[pbw::NUM_UPDATES + m] != 0U;
            if (have_updates && !read16s(up_addr, updates.data(), updates.size())) return;
            u32 start = 0;
            for (unsigned ms = 0; ms < 5; ++ms) {
                const u32 count = pb[pbw::NUM_UPDATES + ms];
                if (start < 32U && count <= 32U - start)
                    for (u32 u = start; u < start + count; ++u)
                        if (updates[2 * u] < pbw::SIZE) pb[updates[2 * u]] = updates[2 * u + 1];
                start += count;
                voice(pb, ms * 32U);
                if (dsp_.failed()) return;
            }
            if (!write_pb(addr, pb)) return;
            addr = u32{pb[pbw::NEXT_HI]} << 16 | pb[pbw::NEXT_LO];
        }
    }

    // One sample from the accelerator, with the AX handling of the end of the sample.
    s16 acc_sample(Pb& pb) {
        DspCore& c = dsp_.host().core();
        u16 v = 0;
        if (!c.accelerator_read(ifx::ACDSAMP, v)) { dsp_.fail("DSP HLE AX: accelerator: " + c.error()); return 0; }
        if ((c.pending_exceptions() & (1U << 5)) != 0U) { // end of the sample
            c.clear_pending_exceptions();
            if (pb[pbw::LOOPING] != 0U) {
                c.accelerator_write(ifx::PRED_SCALE, pb[pbw::LOOP_PS]);
                u16 y1 = 0, y2 = 0;
                c.accelerator_read(ifx::YN1, y1);
                c.accelerator_read(ifx::YN2, y2);
                if (pb[pbw::IS_STREAM] != 1U) { y1 = pb[pbw::LOOP_YN1]; y2 = pb[pbw::LOOP_YN2]; }
                else ++pb[pbw::LOOP_COUNTER];
                c.accelerator_write(ifx::YN1, y1);
                c.accelerator_write(ifx::YN2, y2); // writing YN2 lets the accelerator read again
            } else {
                pb[pbw::RUNNING] = 0;
            }
        }
        return static_cast<s16>(v);
    }

    void voice(Pb& pb, unsigned at) {
        if (pb[pbw::RUNNING] != 1U) return;
        DspCore& c = dsp_.host().core();
        // accelerator set-up from the PB
        auto w = [&](u16 reg, u16 v) { c.accelerator_write(reg, v); };
        w(ifx::FORMAT, pb[pbw::FORMAT]);
        w(ifx::ACSAH, pb[pbw::LOOP_HI]); w(ifx::ACSAL, pb[pbw::LOOP_LO]);
        w(ifx::ACEAH, pb[pbw::END_HI]); w(ifx::ACEAL, pb[pbw::END_LO]);
        w(ifx::ACCAH, pb[pbw::CUR_HI]); w(ifx::ACCAL, pb[pbw::CUR_LO]);
        for (u16 k = 0; k < 16; ++k) w(static_cast<u16>(0xFFA0U + k), pb[pbw::COEFS + k]);
        w(ifx::GAIN, pb[pbw::GAIN]);
        w(ifx::PRED_SCALE, pb[pbw::PRED_SCALE]);
        w(ifx::YN1, pb[pbw::YN1]);
        w(ifx::YN2, pb[pbw::YN2]);
        c.clear_pending_exceptions();

        // resampling (32 output samples)
        std::array<s16, 32> s{};
        u32 pos = pb[pbw::FRAC];
        const u32 ratio = u32{pb[pbw::RATIO_HI]} << 16 | pb[pbw::RATIO_LO];
        const u16 type = pb[pbw::SRC_TYPE];
        std::span<const u16> coef = c.coef_rom();
        if (type == 0U && coef.empty()) {
            dsp_.warn("AX: polyphase resampling without the DSP COEF ROM dump: linear interpolation is used instead");
        }
        if (type == 0U || type == 1U) {
            s16 t[4] = {static_cast<s16>(pb[pbw::LAST]), static_cast<s16>(pb[pbw::LAST + 1]), static_cast<s16>(pb[pbw::LAST + 2]),
                        static_cast<s16>(pb[pbw::LAST + 3])};
            u32 idx = 0; // t[idx & 3] is the oldest of the four
            for (unsigned k = 0; k < 32; ++k) {
                pos += ratio;
                while (pos >= 0x10000U) { t[idx++ & 3U] = acc_sample(pb); pos -= 0x10000U; }
                if (type == 0U && !coef.empty()) { // 4-tap polyphase with the COEF ROM
                    const u32 f = ((pos & 0xFFFFU) >> 9) << 2;
                    const auto* cf = &coef[(pb[pbw::COEF_SELECT] & 3U) * 0x200U + f];
                    const s64 sum = s64{t[idx & 3U]} * static_cast<s16>(cf[0]) + s64{t[(idx + 1) & 3U]} * static_cast<s16>(cf[1]) +
                                    s64{t[(idx + 2) & 3U]} * static_cast<s16>(cf[2]) + s64{t[(idx + 3) & 3U]} * static_cast<s16>(cf[3]);
                    s[k] = clamp16(sum >> 15);
                } else { // linear, between the two oldest of the four
                    const u32 frac = pos & 0xFFFFU;
                    if (frac != 0U) {
                        const s32 a = t[idx & 3U], b = t[(idx + 1) & 3U];
                        s[k] = static_cast<s16>((a * static_cast<s32>((0x10000U - frac) & 0xFFFFU) + b * static_cast<s32>(frac)) >> 16);
                    } else {
                        s[k] = t[idx & 3U];
                    }
                }
            }
            for (unsigned k = 0; k < 4; ++k) pb[pbw::LAST + k] = static_cast<u16>(t[(idx + k) & 3U]);
        } else { // no conversion
            for (unsigned k = 0; k < 32; ++k) s[k] = acc_sample(pb);
            for (unsigned k = 0; k < 4; ++k) pb[pbw::LAST + k] = static_cast<u16>(s[28 + k]);
        }
        if (dsp_.failed()) return;
        pb[pbw::FRAC] = static_cast<u16>(pos);
        u16 v = 0;
        c.accelerator_read(ifx::ACCAH, v); pb[pbw::CUR_HI] = v;
        c.accelerator_read(ifx::ACCAL, v); pb[pbw::CUR_LO] = v;
        c.accelerator_read(ifx::YN1, v); pb[pbw::YN1] = v;
        c.accelerator_read(ifx::YN2, v); pb[pbw::YN2] = v;
        c.accelerator_read(ifx::PRED_SCALE, v); pb[pbw::PRED_SCALE] = v;

        // volume envelope (signed on GameCube)
        for (auto& x : s) {
            x = clamp16((s32{x} * static_cast<s16>(pb[pbw::VOL])) >> 15);
            pb[pbw::VOL] = static_cast<u16>(pb[pbw::VOL] + pb[pbw::VOL_DELTA]);
        }
        if (pb[pbw::LPF_ON] != 0U) {
            for (auto& x : s) {
                x = clamp16((static_cast<s32>(pb[pbw::LPF_A0]) * x + static_cast<s16>(pb[pbw::LPF_B0]) * static_cast<s16>(pb[pbw::LPF_YN1])) >> 15);
                pb[pbw::LPF_YN1] = static_cast<u16>(x);
            }
        }
        if (pb[pbw::ITD_ON] != 0U) dsp_.warn("AX: initial time delay (3D phase shift) is not applied");
        // per-bus mixing: (volume word, delta word) pairs in the PB mixer, dpop words
        const u32 mc = mixer_control(pb[pbw::MIXER_CONTROL]);
        struct Route { u32 on, ramp; unsigned buf, mixer_word, dpop_word; };
        static constexpr Route routes[] = {
            {MIX_MAIN_L, MIX_MAIN_L_RAMP, ML, 0, 0}, {MIX_MAIN_R, MIX_MAIN_R_RAMP, MR, 2, 3},
            {MIX_MAIN_S, MIX_MAIN_S_RAMP, MS, 14, 6}, {MIX_AUXA_L, MIX_AUXA_L_RAMP, AL, 4, 1},
            {MIX_AUXA_R, MIX_AUXA_R_RAMP, AR, 6, 4}, {MIX_AUXA_S, MIX_AUXA_S_RAMP, AS, 16, 7},
            {MIX_AUXB_L, MIX_AUXB_L_RAMP, BL, 8, 2}, {MIX_AUXB_R, MIX_AUXB_R_RAMP, BR, 10, 5},
            {MIX_AUXB_S, MIX_AUXB_S_RAMP, BS, 12, 8}};
        for (const Route& r : routes) {
            if ((mc & r.on) == 0U) continue;
            u16& vol = pb[pbw::MIXER + r.mixer_word];
            const u16 delta = (mc & r.ramp) != 0U ? pb[pbw::MIXER + r.mixer_word + 1] : u16{0};
            for (unsigned k = 0; k < 32; ++k) {
                const s16 out = clamp16((s64{s[k]} * vol) >> 15);
                buf_[r.buf][at + k] += out;
                vol = static_cast<u16>(vol + delta);
                pb[pbw::DPOP + r.dpop_word] = static_cast<u16>(out);
            }
        }
    }

    std::array<u16, 512> list_{};
    u32 size_{0};
    std::array<std::array<s32, kSamples>, kBufs> buf_{};
    u16 compressor_pos_{0};
};

} // namespace

std::unique_ptr<Ucode> make_ax_ucode(Dsp& dsp, u32 hash) {
    switch (hash) {
    // GameCube AX versions (hashes listed in Dolphin's UCodes.cpp)
    case 0x4E8A8B21U: case 0x3AD3B7ACU: case 0x3DAF59B9U: case 0x07F88145U: case 0xE2136399U: case 0x3389A79EU:
        return std::make_unique<AxUcode>(dsp, hash);
    default:
        return nullptr;
    }
}

} // namespace orca::dsp::hle
