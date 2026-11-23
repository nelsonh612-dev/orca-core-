// Orca Core — parts shared by the AX ucodes (GameCube AX and Wii AXWii): mail
// protocol, big-endian RAM access, the accelerator with AX's end-of-sample
// handling, and the resampler. Internal header (src/dsp).
#pragma once
#include "orca/dsp/hle.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <vector>

namespace orca::dsp::hle::ax {

inline s16 clamp16(s64 v) { return static_cast<s16>(std::clamp<s64>(v, -0x8000, 0x7FFF)); }

// Where a PB layout keeps the fields the accelerator and the resampler use (word offsets).
struct VoiceWords {
    unsigned running, is_stream, looping, format, loop, end, cur, coefs, gain, pred_scale, yn1, yn2;
    unsigned src_type, coef_select, ratio, frac, last, loop_ps, loop_yn1, loop_yn2;
    int loop_counter; // -1: this layout has none
};

class AxBase : public Ucode {
public:
    AxBase(Dsp& dsp, u32 hash) : Ucode(dsp, hash) {}
    void start() override { dsp_.push_mail(kMailInit, true); }
    void mail(u32 m) override;

protected:
    virtual void run_list() = 0;

    void bad_mail(u32 m, const char* expected);
    bool fault(u32 addr);
    bool read16s(u32 addr, u16* out, std::size_t n);
    bool write16s(u32 addr, const u16* in, std::size_t n);
    bool read32s(u32 addr, s32* out, std::size_t n);
    bool write32s(u32 addr, const s32* in, std::size_t n);
    bool copy_list(u32 addr, u32 size);

    // Accelerator set-up from the PB, one sample (end of sample handled as AX does),
    // and the state written back to the PB.
    void acc_setup(const u16* pb, const VoiceWords& w);
    s16 acc_sample(u16* pb, const VoiceWords& w);
    void acc_store(u16* pb, const VoiceWords& w);
    // `out.size()` samples from `input` at the PB's ratio and converter type, updating
    // the PB's fractional position and four-sample history. `input` is called for each
    // new input sample.
    template <class Input>
    void resample(u16* pb, std::span<s16> out, Input input, u32 ratio, unsigned last_word, unsigned frac_word, u16 type,
                  unsigned coef_select);

    std::array<u16, 512> list_{};

private:
    enum class State { Size, Address, Task } state_{State::Size};
    u32 size_{0};
};

template <class Input>
void AxBase::resample(u16* pb, std::span<s16> out, Input input, u32 ratio, unsigned last_word, unsigned frac_word,
                      u16 type, unsigned coef_select) {
    std::span<const u16> coef = dsp_.host().core().coef_rom();
    if (type == 0U && coef.empty())
        dsp_.warn("AX: polyphase resampling without the DSP COEF ROM dump: linear interpolation is used instead");
    u32 pos = pb[frac_word];
    if (type == 0U || type == 1U) {
        s16 t[4] = {static_cast<s16>(pb[last_word]), static_cast<s16>(pb[last_word + 1]), static_cast<s16>(pb[last_word + 2]),
                    static_cast<s16>(pb[last_word + 3])};
        u32 idx = 0; // t[idx & 3] is the oldest of the four
        for (auto& o : out) {
            pos += ratio;
            while (pos >= 0x10000U) { t[idx++ & 3U] = input(); pos -= 0x10000U; }
            if (type == 0U && !coef.empty()) { // 4-tap polyphase with the COEF ROM
                const u32 f = ((pos & 0xFFFFU) >> 9) << 2;
                const u16* cf = &coef[(coef_select & 3U) * 0x200U + f];
                const s64 sum = s64{t[idx & 3U]} * static_cast<s16>(cf[0]) + s64{t[(idx + 1) & 3U]} * static_cast<s16>(cf[1]) +
                                s64{t[(idx + 2) & 3U]} * static_cast<s16>(cf[2]) + s64{t[(idx + 3) & 3U]} * static_cast<s16>(cf[3]);
                o = clamp16(sum >> 15);
            } else { // linear, between the two oldest of the four
                const u32 frac = pos & 0xFFFFU;
                if (frac != 0U) {
                    const s32 a = t[idx & 3U], b = t[(idx + 1) & 3U];
                    o = static_cast<s16>((a * static_cast<s32>(0x10000U - frac) + b * static_cast<s32>(frac)) >> 16);
                } else {
                    o = t[idx & 3U];
                }
            }
        }
        for (unsigned k = 0; k < 4; ++k) pb[last_word + k] = static_cast<u16>(t[(idx + k) & 3U]);
    } else { // no conversion
        for (auto& o : out) o = input();
        for (unsigned k = 0; k < 4; ++k) pb[last_word + k] = static_cast<u16>(out[out.size() - 4U + k]);
    }
    pb[frac_word] = static_cast<u16>(pos);
}

} // namespace orca::dsp::hle::ax
