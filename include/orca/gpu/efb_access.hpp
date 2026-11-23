// Orca Core — CPU access to the embedded framebuffer (physical 0x08000000).
//
// Sources (information only): Dolphin Core/PowerPC/MMU.cpp (EFB_Read/EFB_Write,
// address decoding), VideoCommon/EFBInterface.cpp (peek/poke conversions, alpha
// read mode, Z16 peeks) and VideoCommon/VideoCommon.h (RGBA6/RGB565 expansion,
// CompressZ16). The code is Orca's own.
//
// Address (offset from 0x08000000): x = bits 2-11, y = bits 12-21, bit 22 = Z,
// bit 23 = "Z + colour" access (not documented -> clear stop).
//   * Coordinates outside 640x528 read 0 and ignore writes (Dolphin).
//   * Colour read, as ARGB8888: the pixel in the current EFB format (RGBA6
//     expanded c6 << 2 | c6 >> 4, RGB565 quantised to 5/6/5 and expanded),
//     alpha 0xFF unless RGBA6; then PE ALPHAREAD bits 0-1: 0 -> alpha 0x00,
//     1 -> alpha 0xFF, 2 -> as stored, 3 -> stop.
//   * Z read: the 24-bit depth; with RGB565_Z16 the 16-bit compressed form of
//     the Z format in ZCOMPARE bits 3-5 (linear, near, mid, far).
//   * Colour write (ARGB8888): stored in the current format (RGBA6 keeps the
//     top 6 bits of R, G, B, A; the other formats store R, G, B).
//   * Z write: bits 0-23.
//   * Only 32-bit accesses; pixel formats 4-7 (YUV) stop like drawing does.
#pragma once
#include "orca/gpu/gpu.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/memory/bus.hpp"

#include <string>

namespace orca::gpu {

inline constexpr u32 kEfbPhysicalBase = 0x08000000U;
inline constexpr u32 kEfbPhysicalSize = 0x01000000U;

// Z16 compression of a 24-bit depth (formats 0-3 of ZCOMPARE bits 3-5).
u32 compress_z16(u32 z24, u32 zformat) noexcept;

class EfbAccess final : public mem::MmioDevice {
public:
    EfbAccess(Gpu& gpu, hw::PixelEngine& pe) : gpu_(gpu), pe_(pe) {}
    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "EFB"; }
    std::string mmio_error() const override { return error_; }

private:
    bool fail(std::string m) { error_ = std::move(m); return false; }
    Gpu& gpu_;
    hw::PixelEngine& pe_;
    std::string error_;
};

} // namespace orca::gpu
