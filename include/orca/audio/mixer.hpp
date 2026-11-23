// Orca Core — audio output: the AI DMA stream (DSP output) and the streaming
// (DTK) audio, each converted to 48 kHz and mixed, in emulated time.
//
// Inputs keep their own clock: the DMA stream arrives in 32-byte blocks of 8
// frames (big-endian 16-bit, right then left, as the AI reads them; Dolphin's
// Mixer.cpp documents the order) and the streaming audio in 28-frame blocks
// (channel of DTK header byte 0 = right, byte 1 = left, as Dolphin plays them),
// scaled by AIVR (volume v -> gain (v + v/128) / 256). Each is resampled to
// 48000 Hz with exact rational steps (input rate = 108 MHz / divisor) and linear
// interpolation. advance(now) produces the output frames due by `now` minus a
// fixed latency, so the result only depends on emulated time; a stream without
// data contributes silence (counted as an underrun when it was active).
// The output goes to a single-producer single-consumer ring that a frontend
// thread (Android AAudio, a WAV writer) reads with read().
#pragma once
#include "orca/core/bits.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace orca::audio {

inline constexpr u32 kOutputRate = 48000;
inline constexpr u64 kAiClock = 108000000ULL;

class Mixer {
public:
    // `latency_frames`: output lags emulated time by this much, so inputs that
    // arrive in blocks are always complete when mixed.
    Mixer(u64 cpu_hz, u32 latency_frames = 480, std::size_t ring_frames = 1U << 15);

    void push_dma_block(std::span<const u8> block_be, u32 divisor);         // RL big-endian frames
    void push_stream(std::span<const s16> ab_pairs, u32 divisor, u32 aivr); // (byte-0 ch, byte-1 ch) pairs
    void advance(u64 now_cycle);

    // Frontend side (another thread is fine): interleaved left/right frames.
    std::size_t available() const noexcept;
    std::size_t read(std::span<s16> out_lr); // returns frames read
    // Optional synchronous consumer (tests, WAV dumps), called from advance().
    void set_sink(std::function<void(std::span<const s16>)> sink) { sink_ = std::move(sink); }

    u64 frames_produced() const noexcept { return produced_; }
    u64 dma_underruns() const noexcept { return dma_.underruns; }
    u64 stream_underruns() const noexcept { return stream_.underruns; }
    u64 ring_overflows() const noexcept { return overflows_; }

private:
    struct Channel {
        std::deque<s32> l, r;  // pending input frames
        u64 pos{0};            // position of the next output, in 1/48000 of an input frame, from the front
        u32 divisor{0};        // current input rate = kAiClock / divisor
        bool active{false};    // has received data since it last ran dry
        u64 underruns{0};
        void push(s32 left, s32 right, u32 div);
        void next(s32& left, s32& right);
    };
    void emit(s16 l, s16 r);

    u64 cpu_hz_;
    u32 latency_;
    u64 produced_{0};
    Channel dma_, stream_;
    std::vector<s16> ring_;
    std::size_t mask_;
    std::atomic<std::size_t> head_{0}, tail_{0}; // frames written / read
    u64 overflows_{0};
    std::function<void(std::span<const s16>)> sink_;
    std::vector<s16> batch_;
};

} // namespace orca::audio
