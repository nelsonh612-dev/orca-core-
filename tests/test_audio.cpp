// Audio output: the 48 kHz mixer (rates, interpolation, channel order, volume,
// latency, underruns, the frontend ring) and streaming audio through the AI.
#include "orca/audio/mixer.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"

#include <vector>

using namespace orca;
using audio::Mixer;

namespace {
constexpr u64 kHz = 486000000ULL;
// a 32-byte AI DMA block: frames (right, left) big-endian
std::vector<u8> block(const std::vector<std::pair<s16, s16>>& lr) {
    std::vector<u8> b;
    for (auto [l, r] : lr) {
        b.push_back(static_cast<u8>(static_cast<u16>(r) >> 8)); b.push_back(static_cast<u8>(r));
        b.push_back(static_cast<u8>(static_cast<u16>(l) >> 8)); b.push_back(static_cast<u8>(l));
    }
    return b;
}
u64 cycles_for_frames(u64 frames) { return frames * kHz / 48000U; }
} // namespace

TEST_CASE(mixer_48k_passes_frames_through_after_the_latency) {
    Mixer m(kHz, 480);
    // divisor 2250: exactly 48000 Hz (Wii); a ramp of 64 frames
    for (int b = 0; b < 8; ++b) {
        std::vector<std::pair<s16, s16>> f;
        for (int k = 0; k < 8; ++k) { const auto v = static_cast<s16>(100 * (8 * b + k)); f.push_back({v, static_cast<s16>(-v)}); }
        m.push_dma_block(block(f), 2250);
    }
    m.advance(cycles_for_frames(480));
    CHECK_EQ(m.frames_produced(), 0U);          // nothing is due before the latency
    m.advance(cycles_for_frames(480 + 60));
    CHECK_EQ(m.frames_produced(), 60U);
    std::vector<s16> out(2 * 100);
    CHECK_EQ(m.read(out), 60U);
    for (int k = 0; k < 60; ++k) {
        CHECK_EQ(out[2 * static_cast<std::size_t>(k)], static_cast<s16>(100 * k));     // left
        CHECK_EQ(out[2 * static_cast<std::size_t>(k) + 1], static_cast<s16>(-100 * k)); // right
    }
    CHECK_EQ(m.dma_underruns(), 0U);
    m.advance(cycles_for_frames(480 + 80));     // only 64 input frames: the stream runs dry
    CHECK_EQ(m.dma_underruns(), 1U);
}

TEST_CASE(mixer_32k_is_interpolated_to_48k) {
    Mixer m(kHz, 0);
    // divisor 3375: exactly 32000 Hz; ramp 0, 300, 600, ... -> at 48 kHz 0, 200, 400, ...
    for (int b = 0; b < 16; ++b) {
        std::vector<std::pair<s16, s16>> f;
        for (int k = 0; k < 8; ++k) { const auto v = static_cast<s16>(300 * (8 * b + k)); f.push_back({v, 7}); }
        m.push_dma_block(block(f), 3375);
    }
    m.advance(cycles_for_frames(150));
    std::vector<s16> out(2 * 150);
    CHECK_EQ(m.read(out), 150U);
    bool ramp = true;
    for (int k = 0; k < 150; ++k) ramp = ramp && out[2 * static_cast<std::size_t>(k)] == 200 * k && out[2 * static_cast<std::size_t>(k) + 1] == 7;
    CHECK(ramp);
}

TEST_CASE(mixer_streaming_volume_and_channels) {
    Mixer m(kHz, 0);
    // DTK pairs: (header-0 channel = right, header-1 channel = left)
    std::vector<s16> pcm;
    for (int k = 0; k < 56; ++k) { pcm.push_back(1000); pcm.push_back(-2000); }
    m.push_stream(pcm, 2250, 0x80FFU);          // left 255 (gain 1), right 128 (gain 129/256)
    m.advance(cycles_for_frames(40));
    std::vector<s16> out(2 * 40);
    CHECK_EQ(m.read(out), 40U);
    CHECK_EQ(out[0], -2000);
    CHECK_EQ(out[1], 1000 * 129 / 256);
    // both inputs add and saturate
    Mixer s(kHz, 0);
    std::vector<s16> loud(2 * 16, 30000);
    s.push_stream(loud, 2250, 0xFFFFU);
    std::vector<std::pair<s16, s16>> f(8, {20000, -20000});
    s.push_dma_block(block(f), 2250);
    s.push_dma_block(block(f), 2250);
    s.advance(cycles_for_frames(10));
    std::vector<s16> o(2 * 10);
    CHECK_EQ(s.read(o), 10U);
    CHECK_EQ(o[0], 32767);
    CHECK_EQ(o[1], 10000);
}

TEST_CASE(mixer_ring_overflow_and_sink) {
    Mixer m(kHz, 0, 64);
    std::size_t seen = 0;
    m.set_sink([&](std::span<const s16> f) { seen += f.size() / 2U; });
    m.advance(cycles_for_frames(200));          // silence: no input, nothing active, no underrun
    CHECK_EQ(m.frames_produced(), 200U);
    CHECK_EQ(seen, 200U);
    CHECK_EQ(m.available(), 64U);               // ring of 64 frames: the rest was dropped
    CHECK_EQ(m.ring_overflows(), 136U);
    CHECK_EQ(m.dma_underruns(), 0U);
}

TEST_CASE(streaming_audio_reaches_the_mixer_through_the_ai) {
    system::System sys({});
    // disc with streaming enabled and a DTK track of constant samples
    std::vector<u8> img(0x200000, 0);
    img[0x1C] = 0xC2; img[0x1D] = 0x33; img[0x1E] = 0x9F; img[0x1F] = 0x3D;
    for (u32 b = 0; b < 0x4000; ++b) {
        u8* p = &img[0x100000U + 32U * b];
        p[0] = p[1] = 0x00;                        // filter 0, shift 0: nibble << 12
        for (unsigned i = 0; i < 28; ++i) p[4 + i] = 0x21; // right (low nibble) 1 -> 4096, left (high) 2 -> 8192
    }
    sys.insert_disc(std::make_unique<disc::MemoryDisc>(img));
    sys.di().after_ipl_boot(true, 0);
    auto di_cmd = [&](u32 c0, u32 c1, u32 c2) {
        CHECK(sys.di().mmio_write(0x08, 4, c0)); CHECK(sys.di().mmio_write(0x0C, 4, c1)); CHECK(sys.di().mmio_write(0x10, 4, c2));
        CHECK(sys.di().mmio_write(0x1C, 4, 1));
    };
    di_cmd(0xE1000000U, 0x100000U >> 2, 0x80000);
    CHECK(sys.ai().mmio_write(0x04, 4, 0xFFFF));                     // full volume
    CHECK(sys.ai().mmio_write(0x00, 4, hw::ai::PSTAT | hw::ai::AISFR)); // play at "48 kHz"
    sys.cpu().state().pc = 0x3000;
    sys.bus().write32(0x3000, 0x48000000U);                          // b . (the CPU just spins)
    std::vector<s16> got;
    sys.mixer().set_sink([&](std::span<const s16> f) { got.insert(got.end(), f.begin(), f.end()); });
    (void)sys.run(kHz / 10);                                         // 100 ms
    // 100 ms at 48 kHz minus the 10 ms latency
    CHECK(sys.mixer().frames_produced() >= 4300U && sys.mixer().frames_produced() <= 4330U);
    // after the first few frames (interpolation start) every frame is the decoded constant
    bool constant = got.size() > 2000U;
    for (std::size_t k = 2 * 100; k + 1 < got.size(); k += 2) constant = constant && got[k] == 8192 && got[k + 1] == 4096;
    CHECK(constant);
    CHECK_EQ(sys.mixer().stream_underruns(), 0U);
    CHECK(sys.di().stream_playing());
}

TEST_MAIN("audio")
