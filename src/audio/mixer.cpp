// Orca Core — audio mixer. See mixer.hpp.
#include "orca/audio/mixer.hpp"

#include <algorithm>

namespace orca::audio {

namespace {
s32 be16s(const u8* p) noexcept { return static_cast<s16>(static_cast<u16>(p[0] << 8 | p[1])); }
std::size_t pow2_at_least(std::size_t n) { std::size_t p = 1; while (p < n) p <<= 1U; return p; }
} // namespace

Mixer::Mixer(u64 cpu_hz, u32 latency_frames, std::size_t ring_frames)
    : cpu_hz_(cpu_hz), latency_(latency_frames), ring_(2U * pow2_at_least(ring_frames)), mask_(pow2_at_least(ring_frames) - 1U) {}

void Mixer::Channel::push(s32 left, s32 right, u32 div) {
    if (div != divisor) { // new rate: keep the position, in the new unit
        if (divisor != 0U) pos = pos * div / divisor;
        divisor = div;
    }
    l.push_back(left);
    r.push_back(right);
    active = true;
}

// One 48 kHz output frame: linear interpolation at the current position, then the
// position advances by (input rate / 48000) input frames, exactly.
void Mixer::Channel::next(s32& left, s32& right) {
    left = right = 0;
    if (divisor == 0U) return;
    // the input rate in frames/s is kAiClock / divisor: step = kAiClock / divisor / 48000 frames,
    // kept as a fraction with denominator den = divisor * 48000
    const u64 den = u64{divisor} * kOutputRate;
    const u64 i = pos / den, frac = pos % den;
    if (l.size() < i + 2U) { // not enough input: silence, position kept
        if (active) { ++underruns; active = false; }
        return;
    }
    const s64 a = l[i], b = l[i + 1U], c = r[i], d = r[i + 1U];
    left = static_cast<s32>(a + (b - a) * static_cast<s64>(frac) / static_cast<s64>(den));
    right = static_cast<s32>(c + (d - c) * static_cast<s64>(frac) / static_cast<s64>(den));
    pos += kAiClock;
    while (pos >= den && !l.empty()) { l.pop_front(); r.pop_front(); pos -= den; }
}

void Mixer::push_dma_block(std::span<const u8> block, u32 divisor) {
    for (std::size_t k = 0; k + 4U <= block.size(); k += 4U) dma_.push(be16s(&block[k + 2U]), be16s(&block[k]), divisor);
}

void Mixer::push_stream(std::span<const s16> ab, u32 divisor, u32 aivr) {
    const s32 vl = static_cast<s32>(aivr & 0xFFU), vr = static_cast<s32>((aivr >> 8) & 0xFFU);
    const s32 gl = vl + (vl >> 7), gr = vr + (vr >> 7);
    for (std::size_t k = 0; k + 1U < ab.size(); k += 2U) stream_.push((ab[k + 1U] * gl) >> 8, (ab[k] * gr) >> 8, divisor);
}

void Mixer::emit(s16 l, s16 r) {
    const std::size_t h = head_.load(std::memory_order_relaxed), t = tail_.load(std::memory_order_acquire);
    if (h - t > mask_) { ++overflows_; return; } // nobody is reading: drop the newest frame
    ring_[2U * (h & mask_)] = l;
    ring_[2U * (h & mask_) + 1U] = r;
    head_.store(h + 1U, std::memory_order_release);
}

void Mixer::advance(u64 now) {
    const u128 due128 = static_cast<u128>(now) * kOutputRate / cpu_hz_;
    const u64 due = static_cast<u64>(due128);
    if (due <= latency_) return;
    const u64 target = due - latency_;
    batch_.clear();
    while (produced_ < target) {
        s32 dl = 0, dr = 0, sl = 0, sr = 0;
        dma_.next(dl, dr);
        stream_.next(sl, sr);
        const auto l = static_cast<s16>(std::clamp(dl + sl, -32768, 32767));
        const auto r = static_cast<s16>(std::clamp(dr + sr, -32768, 32767));
        emit(l, r);
        if (sink_) { batch_.push_back(l); batch_.push_back(r); }
        ++produced_;
    }
    if (sink_ && !batch_.empty()) sink_(batch_);
}

std::size_t Mixer::available() const noexcept {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_relaxed);
}

std::size_t Mixer::read(std::span<s16> out) {
    const std::size_t t = tail_.load(std::memory_order_relaxed), h = head_.load(std::memory_order_acquire);
    const std::size_t n = std::min(h - t, out.size() / 2U);
    for (std::size_t k = 0; k < n; ++k) {
        out[2U * k] = ring_[2U * ((t + k) & mask_)];
        out[2U * k + 1U] = ring_[2U * ((t + k) & mask_) + 1U];
    }
    tail_.store(t + n, std::memory_order_release);
    return n;
}

} // namespace orca::audio
