// Orca Core — pixel statistics the Pixel Engine exposes to the CPU.
//
// Sources (information only): Dolphin VideoCommon/PixelEngine.h/.cpp,
// BPStructs.cpp and VideoBackends/Software (Tev.cpp, Rasterizer.cpp,
// SWBoundingBox.cpp).
//   * Bounding box (PE 0x10-0x16 = left, right, top, bottom): set by BP 0x55
//     (left = bits 0-9, right = bits 10-19) and BP 0x56 (top, bottom); every
//     pixel that reaches blending (after alpha test and Z) widens it to the
//     edges of its 2x2 quad ("the GPU rasterizes in 2x2 pixel groups").
//   * Performance counters (PE 0x18-0x26, low/high halves; Dolphin marks the
//     register order as not verified): quads entering / passing the early Z
//     test (Z compare location = before texturing), entering / passing the late
//     Z test, and entering blending. The four Z counters only count while Z
//     testing is enabled; blend input always counts (as in Dolphin's software
//     renderer). BP 0x57 clears them. Orca counts a 2x2 quad once per
//     primitive when at least one of its pixels reaches the stage (Dolphin's
//     software renderer approximates with every third pixel).
#pragma once
#include "orca/core/bits.hpp"

#include <array>

namespace orca::gpu {

struct PixelStats {
    enum Counter : unsigned { ZcompLocInput = 0, ZcompLocOutput, ZcompInput, ZcompOutput, BlendInput, kCount };
    std::array<u16, 4> bbox{}; // left, right, top, bottom
    std::array<u32, kCount> quads{};
    // False while a GPU backend draws: it does not track these values (yet), so
    // reading them stops instead of returning stale numbers.
    bool tracked{true};
    // Set by BP 0x55/0x56 (box) and BP 0x57 (counters): a GPU backend only tracks them
    // from that point on. `epoch` changes whenever the CPU side changes the values.
    bool bbox_armed{false}, quads_armed{false};
    u32 epoch{0};

    void set_bbox_bp(bool second, u32 value) noexcept {
        const unsigned o = second ? 2U : 0U;
        bbox[o] = static_cast<u16>(value & 0x3FFU);
        bbox[o + 1U] = static_cast<u16>((value >> 10) & 0x3FFU);
    }
    void widen(u32 x, u32 y) noexcept {
        const auto l = static_cast<u16>(x & ~1U), r = static_cast<u16>(x | 1U);
        const auto t = static_cast<u16>(y & ~1U), b = static_cast<u16>(y | 1U);
        if (l < bbox[0]) bbox[0] = l;
        if (r > bbox[1]) bbox[1] = r;
        if (t < bbox[2]) bbox[2] = t;
        if (b > bbox[3]) bbox[3] = b;
    }
};

} // namespace orca::gpu
