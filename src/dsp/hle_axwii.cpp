// Orca Core — HLE of the Wii AX ucode (AXWii).
//
// As Dolphin's DSPHLE/UCodes/AXWii.cpp and AXVoice.h describe it (information
// only; Orca's code): 3 ms per command list (96 samples at 32 kHz) in twelve
// buffers (main, aux A, aux B, aux C; left, right, surround) plus eight Wii Remote
// speaker buffers (6 samples per ms); 32-bit mixer control; unsigned volume
// envelope; biquad filters in the late versions; per-millisecond PB updates only
// in the oldest versions; master and aux volumes ramped from the previous value
// (Dolphin's floating-point ramp, which it notes is not exactly the ucode's: a
// warning is recorded); DSP_SYNC with an interrupt after the output.
// Three PB memory layouts (differing in the update and filter fields) are read
// into the largest one. Not verified against the real ucode (not available here).
#include "hle_ax_common.hpp"

namespace orca::dsp::hle {

namespace {

using ax::clamp16;
constexpr unsigned kMs = 3, kSamples = 32 * kMs, kWmSamples = 6 * kMs;

// largest PB layout, in 16-bit words
namespace pbw {
inline constexpr unsigned NEXT_HI = 0, NEXT_LO = 1, SRC_TYPE = 4, COEF_SELECT = 5, MIXER_HI = 6, MIXER_LO = 7, RUNNING = 8,
                          IS_STREAM = 9, MIXER = 10, ITD_ON = 34, DPOP = 41, UPDATES = 53, UPDATES_HI = 56,
                          UPDATES_LO = 57, VOL = 58, VOL_DELTA = 59, LOOPING = 60, FORMAT = 61, LOOP_HI = 62,
                          END_HI = 64, CUR_HI = 66, COEFS = 68, GAIN = 84, PRED_SCALE = 85, YN1 = 86, YN2 = 87,
                          RATIO_HI = 88, RATIO_LO = 89, FRAC = 90, LAST = 91, LOOP_PS = 95, LOOP_YN1 = 96, LOOP_YN2 = 97,
                          LPF_ON = 98, BIQUAD = 102, HPF_END = 106,
                          BIQUAD_END = 112, REMOTE = 112, REMOTE_MC = 113, REMOTE_MIXER = 114, REMOTE_DPOP = 130,
                          REMOTE_FRAC = 138, REMOTE_LAST = 139, REMOTE_IIR = 143, SIZE = 155;
}
using Pb = std::array<u16, pbw::SIZE>;

constexpr ax::VoiceWords kWords{pbw::RUNNING, pbw::IS_STREAM, pbw::LOOPING, pbw::FORMAT, pbw::LOOP_HI, pbw::END_HI,
                                pbw::CUR_HI, pbw::COEFS, pbw::GAIN, pbw::PRED_SCALE, pbw::YN1, pbw::YN2, pbw::SRC_TYPE,
                                pbw::COEF_SELECT, pbw::RATIO_HI, pbw::FRAC, pbw::LAST, pbw::LOOP_PS, pbw::LOOP_YN1,
                                pbw::LOOP_YN2, -1};

enum Buf : unsigned { ML, MR, MS, AL, AR, AS, BL, BR, BS, CL, CR, CS, kBufs };

enum : u32 {
    MAIN_L = 1U << 0, MAIN_L_R = 1U << 1, MAIN_R = 1U << 2, MAIN_R_R = 1U << 3, MAIN_S = 1U << 4, MAIN_S_R = 1U << 5,
    AUXA_L = 1U << 6, AUXA_L_R = 1U << 7, AUXA_R = 1U << 8, AUXA_R_R = 1U << 9, AUXA_S = 1U << 10, AUXA_S_R = 1U << 11,
    AUXB_L = 1U << 12, AUXB_L_R = 1U << 13, AUXB_R = 1U << 14, AUXB_R_R = 1U << 15, AUXB_S = 1U << 16, AUXB_S_R = 1U << 17,
    AUXC_L = 1U << 18, AUXC_L_R = 1U << 19, AUXC_R = 1U << 20, AUXC_R_R = 1U << 21, AUXC_S = 1U << 22, AUXC_S_R = 1U << 23,
};

u32 mixer_control(u32 mc) {
    u32 r = 0;
    if ((mc & 0x00000001U) != 0U) r |= MAIN_L;
    if ((mc & 0x00000002U) != 0U) r |= MAIN_R;
    if ((mc & 0x00000004U) != 0U) r |= MAIN_L | MAIN_R | MAIN_L_R | MAIN_R_R;
    if ((mc & 0x00000008U) != 0U) r |= MAIN_S;
    if ((mc & 0x00000010U) != 0U) r |= MAIN_S | MAIN_S_R;
    if ((mc & 0x00010000U) != 0U) r |= AUXA_L;
    if ((mc & 0x00020000U) != 0U) r |= AUXA_R;
    if ((mc & 0x00040000U) != 0U) r |= AUXA_L | AUXA_R | AUXA_L_R | AUXA_R_R;
    if ((mc & 0x00080000U) != 0U) r |= AUXA_S;
    if ((mc & 0x00100000U) != 0U) r |= AUXA_S | AUXA_S_R;
    if ((mc & 0x00200000U) != 0U) r |= AUXB_L;
    if ((mc & 0x00400000U) != 0U) r |= AUXB_R;
    if ((mc & 0x00800000U) != 0U) r |= AUXB_L | AUXB_R | AUXB_L_R | AUXB_R_R;
    if ((mc & 0x01000000U) != 0U) r |= AUXB_S;
    if ((mc & 0x02000000U) != 0U) r |= AUXB_S | AUXB_S_R;
    if ((mc & 0x04000000U) != 0U) r |= AUXC_L;
    if ((mc & 0x08000000U) != 0U) r |= AUXC_R;
    if ((mc & 0x10000000U) != 0U) r |= AUXC_L | AUXC_R | AUXC_L_R | AUXC_R_R;
    if ((mc & 0x20000000U) != 0U) r |= AUXC_S;
    if ((mc & 0x40000000U) != 0U) r |= AUXC_S | AUXC_S_R;
    return r;
}

// Dolphin's interpolation of the master/aux volume over a frame (single precision)
void volume_ramp(u16* out, u16 from, u16 to, std::size_t n) {
    float cur = from;
    const float step = static_cast<float>(static_cast<int>(to) - static_cast<int>(from)) / static_cast<float>(n);
    for (std::size_t i = 0; i < n; ++i) { cur += step; out[i] = static_cast<u16>(cur); }
}

class AxWiiUcode final : public ax::AxBase {
public:
    AxWiiUcode(Dsp& dsp, u32 hash) : AxBase(dsp, hash) {
        old_ = hash == 0xFA450138U || hash == 0x7699AF32U;
        new_filter_ = hash == 0x347112BAU || hash == 0x4CC52064U;
        // PB memory layouts: with updates and the high-pass filter (old); without updates
        // and with the high-pass filter; without updates and with the biquad.
        if (old_) layout_ = Layout::UpdatesHpf;
        else if (hash == 0xD9C4BF34U || hash == 0xADBC06BDU) layout_ = Layout::Hpf;
        else if (new_filter_) layout_ = Layout::Biquad;
    }
    const char* name() const noexcept override { return "AXWii"; }

private:
    void run_list() override {
        u32 i = 0, pb_addr = 0;
        auto arg = [&]() -> u16 { return i < list_.size() ? list_[i++] : 0; };
        auto arg32 = [&]() -> u32 { const u32 hi = arg(); return hi << 16 | arg(); };
        for (unsigned guard = 0; guard < 100000U && !dsp_.failed(); ++guard) {
            u16 cmd = arg();
            // the old versions number the commands one higher after 3 (they have PB_ADDR at 4)
            if (old_) {
                if (cmd == 0x04U) { pb_addr = arg32(); continue; }
                if (cmd == 0x05U) { process_pbs(pb_addr); continue; }
                if (cmd > 0x05U) --cmd;
            }
            switch (cmd) {
            case 0x00: setup(arg32()); break;
            case 0x01: case 0x02: add_to_lr(arg32(), cmd == 0x02U); break;
            case 0x03: add_sub_to_lr(arg32()); break;
            case 0x04: process_pbs(arg32()); break;                 // new versions only (old: handled above)
            case 0x05: case 0x06: case 0x07: {
                const u16 vol = arg();
                const u32 w = arg32(), r = arg32();
                mix_aux(cmd - 0x05U, w, r, vol);
                break;
            }
            case 0x08: case 0x09: {
                const u16 vol = arg();
                u32 a[6];
                for (auto& x : a) x = arg32();
                upload_aux_mix_lrsc(cmd - 0x08U, a, vol);
                break;
            }
            case 0x0A: { const u16 th = arg(), fr = arg(); compressor(th, fr, arg32()); break; }
            case 0x0B: case 0x0C: {
                const u16 vol = (old_ || hash_ == 0xD9C4BF34U) ? u16{0x8000} : arg(); // no volume in these versions
                const u32 s = arg32(), lr = arg32();
                output(lr, s, vol, cmd == 0x0CU);
                break;
            }
            case 0x0D: { u32 a[4]; for (auto& x : a) x = arg32(); output_wm(a); break; }
            case 0x0E: return;
            default: {
                char buf[96];
                std::snprintf(buf, sizeof buf, "DSP HLE AXWii: command %04X is not implemented", list_[i - 1]);
                dsp_.fail(buf);
                return;
            }
            }
        }
    }

    void setup(u32 addr) {
        std::array<u16, 3 * 20> init{};
        if (!read16s(addr, init.data(), init.size())) return;
        for (unsigned b = 0; b < 20; ++b) {
            const auto value = static_cast<s32>(u32{init[3 * b]} << 16 | init[3 * b + 1]);
            const auto delta = static_cast<s16>(init[3 * b + 2]);
            s32* p = b < kBufs ? buf_[b].data() : wm_[b - kBufs].data();
            const unsigned n = b < kBufs ? kSamples : kWmSamples;
            for (unsigned k = 0; k < n; ++k) p[k] = value == 0 ? 0 : value + static_cast<s32>(k) * delta;
        }
    }

    void add_to_lr(u32 addr, bool neg) {
        std::array<s32, kSamples> in{};
        if (!read32s(addr, in.data(), kSamples)) return;
        for (unsigned k = 0; k < kSamples; ++k) { const s32 v = neg ? -in[k] : in[k]; buf_[ML][k] += v; buf_[MR][k] += v; }
    }

    void add_sub_to_lr(u32 addr) {
        std::vector<s32> in(2 * kSamples);
        if (!read32s(addr, in.data(), in.size())) return;
        for (unsigned k = 0; k < kSamples; ++k) { buf_[ML][k] += in[k]; buf_[MR][k] -= in[kSamples + k]; }
    }

    void ramp_warning() { dsp_.warn("AXWii: master and aux volume ramps use Dolphin's floating-point interpolation (not the ucode's exact one)"); }

    void mix_aux(unsigned id, u32 write_addr, u32 read_addr, u16 vol) {
        std::array<u16, kSamples> ramp{};
        volume_ramp(ramp.data(), last_aux_[id], vol, kSamples);
        if (last_aux_[id] != vol) ramp_warning();
        last_aux_[id] = vol;
        const unsigned base = id == 0U ? AL : (id == 1U ? BL : CL);
        if (write_addr != 0U)
            for (unsigned c = 0; c < 3; ++c)
                if (!write32s(write_addr + c * kSamples * 4U, buf_[base + c].data(), kSamples)) return;
        std::vector<s32> in(3 * kSamples);
        if (!read32s(read_addr, in.data(), in.size())) return;
        for (unsigned c = 0; c < 3; ++c)
            for (unsigned k = 0; k < kSamples; ++k)
                buf_[ML + c][k] += static_cast<s32>((s64{in[c * kSamples + k]} * ramp[k]) >> 15);
    }

    void upload_aux_mix_lrsc(unsigned id, const u32* a, u16 vol) {
        const unsigned base = id != 0U ? BL : AL;
        const unsigned auxc = id != 0U ? CS : CR;
        for (unsigned c = 0; c < 3; ++c)
            if (!write32s(a[0] + c * kSamples * 4U, buf_[base + c].data(), kSamples)) return;
        if (!write32s(a[1], buf_[auxc].data(), kSamples)) return;
        std::array<u16, kSamples> ramp{};
        volume_ramp(ramp.data(), last_aux_[id], vol, kSamples);
        if (last_aux_[id] != vol) ramp_warning();
        last_aux_[id] = vol;
        const unsigned dest[4] = {ML, MR, MS, CL};
        std::array<s32, kSamples> in{};
        for (unsigned m = 0; m < 4; ++m) {
            if (!read32s(a[2 + m], in.data(), kSamples)) return;
            for (unsigned k = 0; k < kSamples; ++k) {
                buf_[base][k] = in[k]; // the aux-left buffer is the download area (as the ucode does)
                buf_[dest[m]][k] += static_cast<s32>((s64{in[k]} * ramp[k]) >> 15);
            }
        }
    }

    void compressor(u16 threshold, u16 release_frames, u32 table) {
        bool triggered = false;
        for (unsigned k = 0; k < kSamples && !triggered; ++k)
            triggered = std::abs(buf_[ML][k]) > static_cast<s32>(threshold) || std::abs(buf_[MR][k]) > static_cast<s32>(threshold);
        const u32 frame_bytes = kSamples * 2U;
        u32 off = 0;
        if (triggered) { off = compressor_pos_ * frame_bytes; compressor_pos_ = release_frames; }
        else if (compressor_pos_ != 0U) { --compressor_pos_; off = (11U + compressor_pos_) * frame_bytes; }
        else return;
        std::array<u16, kSamples> ramp{};
        if (!read16s(table + off, ramp.data(), kSamples)) return;
        for (unsigned k = 0; k < kSamples; ++k) {
            buf_[ML][k] = static_cast<s32>((s64{buf_[ML][k]} * ramp[k]) >> 15);
            buf_[MR][k] = static_cast<s32>((s64{buf_[MR][k]} * ramp[k]) >> 15);
        }
    }

    void output(u32 lr_addr, u32 surround_addr, u16 vol, bool upload_auxc) {
        std::array<u16, kSamples> ramp{};
        volume_ramp(ramp.data(), last_main_, vol, kSamples);
        if (last_main_ != vol) ramp_warning();
        last_main_ = vol;
        if (!write32s(surround_addr, buf_[MS].data(), kSamples)) return;
        if (upload_auxc && !write32s(surround_addr + kSamples * 4U, buf_[CL].data(), kSamples)) return;
        std::array<u16, 2 * kSamples> lr{};
        for (unsigned k = 0; k < kSamples; ++k) {
            buf_[ML][k] = clamp16((s64{buf_[ML][k]} * ramp[k]) >> 15);
            buf_[MR][k] = clamp16((s64{buf_[MR][k]} * ramp[k]) >> 15);
            lr[2 * k] = static_cast<u16>(static_cast<s16>(buf_[MR][k]));
            lr[2 * k + 1] = static_cast<u16>(static_cast<s16>(buf_[ML][k]));
        }
        if (!write16s(lr_addr, lr.data(), lr.size())) return;
        dsp_.push_mail(kMailSync, true);
    }

    void output_wm(const u32* a) {
        for (unsigned r = 0; r < 4; ++r) {
            std::array<u16, kWmSamples> out{};
            for (unsigned k = 0; k < kWmSamples; ++k) out[k] = static_cast<u16>(clamp16(wm_[2 * r][k]));
            if (!write16s(a[r], out.data(), out.size())) return;
        }
    }

    // ---- PBs ---------------------------------------------------------------------
    // Memory layout of each version relative to the largest one: which word ranges exist.
    enum class Layout { Unknown, UpdatesHpf, Hpf, Biquad } layout_{Layout::Unknown};
    bool has_updates() const noexcept { return layout_ == Layout::UpdatesHpf; }
    bool has_biquad() const noexcept { return layout_ == Layout::Biquad; } // else the 4-word high-pass filter

    bool read_pb(u32 addr, Pb& pb) {
        pb.fill(0);
        const bool upd = has_updates(), biq = has_biquad();
        u32 at = addr;
        auto chunk = [&](unsigned from, unsigned to) {
            if (!read16s(at, pb.data() + from, to - from)) return false;
            at += 2U * (to - from);
            return true;
        };
        if (upd) { if (!chunk(0, biq ? pbw::SIZE : pbw::HPF_END)) return false; }
        else if (!chunk(0, pbw::UPDATES) || !chunk(pbw::UPDATES + 5U, biq ? pbw::SIZE : pbw::HPF_END)) return false;
        if (!biq) return chunk(pbw::BIQUAD_END, pbw::SIZE);
        return true;
    }
    bool write_pb(u32 addr, const Pb& pb) {
        const bool upd = has_updates(), biq = has_biquad();
        u32 at = addr;
        auto chunk = [&](unsigned from, unsigned to) {
            if (!write16s(at, pb.data() + from, to - from)) return false;
            at += 2U * (to - from);
            return true;
        };
        if (upd) { if (!chunk(0, biq ? pbw::SIZE : pbw::HPF_END)) return false; }
        else if (!chunk(0, pbw::UPDATES) || !chunk(pbw::UPDATES + 5U, biq ? pbw::SIZE : pbw::HPF_END)) return false;
        if (!biq) return chunk(pbw::BIQUAD_END, pbw::SIZE);
        return true;
    }

    void process_pbs(u32 addr) {
        if (addr != 0U && layout_ == Layout::Unknown) {
            char buf[112];
            std::snprintf(buf, sizeof buf, "DSP HLE AXWii: the PB layout of ucode %08X is not known", hash_);
            dsp_.fail(buf);
            return;
        }
        unsigned guard = 0;
        while (addr != 0U && !dsp_.failed()) {
            if (++guard > 4096U) { dsp_.fail("DSP HLE AXWii: the PB list does not end (more than 4096 voices)"); return; }
            Pb pb{};
            if (!read_pb(addr, pb)) return;
            const bool per_ms = old_ && (pb[pbw::UPDATES] | pb[pbw::UPDATES + 1] | pb[pbw::UPDATES + 2]) != 0U;
            if (per_ms) {
                std::array<u16, 64> updates{};
                if (!read16s(u32{pb[pbw::UPDATES_HI]} << 16 | pb[pbw::UPDATES_LO], updates.data(), updates.size())) return;
                u32 start = 0;
                for (unsigned ms = 0; ms < kMs; ++ms) {
                    const u32 count = pb[pbw::UPDATES + ms];
                    if (start < 32U && count <= 32U - start)
                        for (u32 u = start; u < start + count; ++u)
                            if (updates[2 * u] < pbw::SIZE) pb[updates[2 * u]] = updates[2 * u + 1];
                    start += count;
                    voice(pb, ms * 32U, 32U, ms * 6U);
                    if (dsp_.failed()) return;
                }
            } else {
                voice(pb, 0, kSamples, 0);
            }
            if (dsp_.failed() || !write_pb(addr, pb)) return;
            addr = u32{pb[pbw::NEXT_HI]} << 16 | pb[pbw::NEXT_LO];
        }
    }

    static void biquad(std::span<s16> s, u16* f) { // on, xn1, xn2, yn1, yn2, b0, b1, b2, a1, a2
        for (auto& x : s) {
            const s16 xn0 = x;
            s64 t = s64{static_cast<s16>(f[5])} * xn0 + s64{static_cast<s16>(f[6])} * static_cast<s16>(f[1]) +
                    s64{static_cast<s16>(f[7])} * static_cast<s16>(f[2]) + s64{static_cast<s16>(f[8])} * static_cast<s16>(f[3]) +
                    s64{static_cast<s16>(f[9])} * static_cast<s16>(f[4]);
            t <<= 2;
            t += (t & 0x10000) != 0 ? 0x8000 : 0x7FFF;
            t >>= 16;
            const s16 yn0 = clamp16(t);
            f[2] = f[1]; f[4] = f[3]; f[1] = static_cast<u16>(xn0); f[3] = static_cast<u16>(yn0);
            x = yn0;
        }
    }
    static void lowpass(std::span<s16> s, u16* f) { // on, yn1, a0, b0
        for (auto& x : s) {
            x = clamp16((s32{static_cast<u16>(f[2])} * x + s32{static_cast<s16>(f[3])} * static_cast<s16>(f[1])) >> 15);
            f[1] = static_cast<u16>(x);
        }
    }

    void mix(std::span<const s16> s, s32* out, u16& vol, u16 delta, u16& dpop, bool ramp) {
        const u16 d = ramp ? delta : u16{0};
        for (std::size_t k = 0; k < s.size(); ++k) {
            const s16 v = clamp16((s64{s[k]} * vol) >> 15);
            out[k] += v;
            vol = static_cast<u16>(vol + d);
            dpop = static_cast<u16>(v);
        }
    }

    void voice(Pb& pb, unsigned at, unsigned count, unsigned wm_at) {
        if (pb[pbw::RUNNING] != 1U) return;
        acc_setup(pb.data(), kWords);
        std::array<s16, kSamples> buf{};
        std::span<s16> s(buf.data(), count);
        const u32 ratio = u32{pb[pbw::RATIO_HI]} << 16 | pb[pbw::RATIO_LO];
        resample(pb.data(), s, [&] { return acc_sample(pb.data(), kWords); }, ratio, pbw::LAST, pbw::FRAC, pb[pbw::SRC_TYPE],
                 pb[pbw::COEF_SELECT]);
        if (dsp_.failed()) return;
        acc_store(pb.data(), kWords);
        for (auto& x : s) { // unsigned envelope on Wii
            x = clamp16((s32{x} * static_cast<s32>(pb[pbw::VOL])) >> 15);
            pb[pbw::VOL] = static_cast<u16>(pb[pbw::VOL] + pb[pbw::VOL_DELTA]);
        }
        if (pb[pbw::LPF_ON] != 0U) lowpass(s, &pb[pbw::LPF_ON]);
        if (new_filter_ && pb[pbw::BIQUAD] != 0U) biquad(s, &pb[pbw::BIQUAD]);
        const u32 mc = mixer_control(u32{pb[pbw::MIXER_HI]} << 16 | pb[pbw::MIXER_LO]);
        struct Route { u32 on, ramp; unsigned buf, mixer_word, dpop_word; };
        static constexpr Route routes[] = {
            {MAIN_L, MAIN_L_R, ML, 0, 0}, {MAIN_R, MAIN_R_R, MR, 2, 4}, {MAIN_S, MAIN_S_R, MS, 16, 8},
            {AUXA_L, AUXA_L_R, AL, 4, 1}, {AUXA_R, AUXA_R_R, AR, 6, 5}, {AUXA_S, AUXA_S_R, AS, 18, 9},
            {AUXB_L, AUXB_L_R, BL, 8, 2}, {AUXB_R, AUXB_R_R, BR, 10, 6}, {AUXB_S, AUXB_S_R, BS, 20, 10},
            {AUXC_L, AUXC_L_R, CL, 12, 3}, {AUXC_R, AUXC_R_R, CR, 14, 7}, {AUXC_S, AUXC_S_R, CS, 22, 11}};
        for (const Route& r : routes)
            if ((mc & r.on) != 0U)
                mix(s, buf_[r.buf].data() + at, pb[pbw::MIXER + r.mixer_word], pb[pbw::MIXER + r.mixer_word + 1],
                    pb[pbw::DPOP + r.dpop_word], (mc & r.ramp) != 0U);
        if (pb[pbw::ITD_ON] != 0U) dsp_.warn("AXWii: initial time delay (3D phase shift) is not applied");
        if (pb[pbw::REMOTE] == 0U) return;
        // Wii Remote speakers: 6 samples per ms from these, polyphase ratio 0x55555 (~96/18)
        std::array<s16, kSamples> copy{};
        std::copy(s.begin(), s.end(), copy.begin());
        std::span<s16> cs(copy.data(), count);
        if (new_filter_ && pb[pbw::REMOTE_IIR] != 0U) {
            if (pb[pbw::REMOTE_IIR] == 2U) biquad(cs, &pb[pbw::REMOTE_IIR]);
            else lowpass(cs, &pb[pbw::REMOTE_IIR]);
        }
        const unsigned wm_count = count == kSamples ? kWmSamples : 6U;
        std::array<s16, kWmSamples> wm{};
        unsigned next = 0;
        resample(pb.data(), std::span<s16>(wm.data(), wm_count), [&] { return next < count ? cs[next++] : s16{0}; }, 0x55555U,
                 pbw::REMOTE_LAST, pbw::REMOTE_FRAC, 0 /* polyphase */, 0 /* first coefficient table */);
        const u16 rmc = pb[pbw::REMOTE_MC];
        for (unsigned ch = 0; ch < 8; ++ch) {
            const unsigned bits = (rmc >> (2U * ch)) & 3U;
            if (bits == 0U) continue;
            mix(std::span<const s16>(wm.data(), wm_count), wm_[ch].data() + wm_at, pb[pbw::REMOTE_MIXER + 2U * ch],
                pb[pbw::REMOTE_MIXER + 2U * ch + 1U], pb[pbw::REMOTE_DPOP + ch], (bits & 2U) != 0U);
        }
    }

    bool old_{false}, new_filter_{false};
    std::array<std::array<s32, kSamples>, kBufs> buf_{};
    std::array<std::array<s32, kWmSamples>, 8> wm_{}; // main0, aux0, main1, aux1, ...
    u16 last_main_{0x8000};
    u16 last_aux_[3]{0x8000, 0x8000, 0x8000};
    u16 compressor_pos_{0};
};

} // namespace

std::unique_ptr<Ucode> make_axwii_ucode(Dsp& dsp, u32 hash) {
    switch (hash) {
    // AXWii versions (hashes listed in Dolphin's UCodes.cpp)
    case 0x2EA36CE6U: case 0x5EF56DA3U: case 0x347112BAU: case 0xFA450138U: case 0xADBC06BDU: case 0x4CC52064U:
    case 0xD9C4BF34U: case 0x7699AF32U:
        return std::make_unique<AxWiiUcode>(dsp, hash);
    default:
        return nullptr;
    }
}

} // namespace orca::dsp::hle
