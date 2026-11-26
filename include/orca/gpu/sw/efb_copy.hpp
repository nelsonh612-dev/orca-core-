// Orca Core — EFB copies and clears triggered by BP 0x52 (software reference).
//
// Behaviour taken from Dolphin, read as information only (the code is Orca's own):
//  * BPStructs.cpp (BPMEM_TRIGGER_EFB_COPY): rectangle = BP 49 (x bits 0-9,
//    y bits 10-19) with size BP 4A + 1; a rectangle starting outside the
//    640x528 EFB is ignored (no copy, no clear); one running past it is
//    clamped; destination = BP 4B << 5, row stride = BP 4D << 5, and a stride
//    of 0 skips the copy; the clear happens after the copy.
//  * BPMemory.h (UPE_Copy): bit 0 clamp top, 1 clamp bottom, 3-6 target
//    format (real format = fmt/2 + (fmt&1)*8), 7-8 gamma (1.0, 1.7, 2.2, 2.2),
//    9 half scale (2x2 box filter), 10 scale invert, 11 clear, 14 copy to XFB,
//    15 intensity, 16 automatic conversion (intensity applies when 15 and 16
//    are both set). Copy filter: BP 53/54, seven 6-bit weights.
//  * TextureConversionShader.cpp + TextureCacheBase.cpp (Dolphin's hardware-backend path,
//    the one tested against consoles for the copy filter and gamma, PR 10204 / 10466): the
//    EFB is read as RGB8 (6-bit channels expanded; depth bytes high/mid/low as R/G/B, alpha
//    255) with coordinates clamped to the EFB; the copy filter applies to colour AND depth:
//    rows previous (w0+w1), current (w2+w3+w4), next (w5+w6), sum >> 6, wrapped to 9 bits
//    when the three weights sum to 128 or more, then min(255), alpha untouched; rows are
//    clamped to the rectangle top/bottom when clamp_top/clamp_bottom are set (else to the
//    EFB); gamma = round(pow(c/255, 1/gamma) * 255); intensity copies replace R, G, B with
//    Y, U, V = (66R+129G+25B+4096, -38R-74G+112B+32768, 112R-94G-18B+32768) >> 8 with .5
//    rounded up, for any format and also from depth; block counts = ceil(texels / block).
//  * TextureEncoder.cpp (software encoder): texel packing per format and block layout (the
//    same as the hardware shaders), and the integer 2x2 box filter of half scale (6-bit sum
//    s -> s + (s >> 6); 8-bit sum >> 2).
//  * SWEfbInterface.cpp: XFB encoding (BT.601 YUV with rounding, 1/4-1/2-1/4 chroma filter,
//    Y + 16, UV + 128, border pixel repeated); CopyRegion.h: vertical scaling by nearest
//    row round(row * src_h / dst_h); BPStructs.cpp: lines written = 1 + height field * scale
//    (scale = BP 4E / 256, or 256 / BP 4E with scale invert).
//  * BPFunctions.cpp ClearScreen: alpha is not written unless the format is
//    RGBA6_Z24; colour quantized to RGB565 and depth to 16 bits for
//    RGB565_Z16 (Z16 -> Z24 = (z & 0xFFFF00) | (z >> 16)).
//  * TextureDecoder.h: XFB memory layout "YUVY" (Y, U, Y, V bytes).
//
// Orca choices / pending hardware verification (also listed in docs/ESTADO.md):
//  * Half scale: the hardware shaders average with the GPU's bilinear filter in float (GPU
//    dependent); Orca uses the software encoder's integer box filter, then the vertical
//    filter with the neighbour texels two EFB rows above/below.
//  * XFB: Dolphin's hardware shader averages the chroma of each pixel pair instead of the
//    1/4-1/2-1/4 filter of the software encoder (whose YUV constants are marked "determined by
//    hardware testing"; the shader marks its rounding as unchecked); Orca keeps the software
//    encoder. The filter and gamma are the shared ones above.
//  * XFB copies need an even start column and width. If fewer source lines result from the
//    scale than lines are written, the last ones repeat the nearest source row.
#pragma once
#include "orca/gpu/bp_memory.hpp"
#include "orca/gpu/sw/renderer.hpp"
#include "orca/memory/bus.hpp"

#include <array>
#include <string>
#include <vector>

namespace orca::gpu::sw {

// EFB area (x0, y0 inclusive; x1, y1 exclusive).
struct EfbRect {
    u32 x0{0}, y0{0}, x1{0}, y1{0};
};

// The EFB pixels a copy with the current BP 0x49/0x4A source rectangle can read
// or clear: the rectangle plus two pixels on every side (the copy filter reads
// the lines above and below; half-scale reads 2x2 blocks), clamped to the EFB.
EfbRect copy_source_area(const BpMemory& bp);

// What one BP 0x52 write did (for the VI and for tests).
struct CopyResult {
    bool ignored{false};   // rectangle outside the EFB: nothing done
    bool copied{false};    // something was written to RAM
    bool xfb{false};       // the copy was a display (XFB) copy
    bool cleared{false};
    u32 address{0};        // destination (physical)
    u32 stride{0};         // bytes between rows (XFB) or rows of blocks (texture)
    u32 width{0}, height{0}; // pixels (XFB) or texels (texture)
    u32 bytes{0};          // bytes written to RAM
};

// The source rectangle and pixel format of a copy (BP 0x49/0x4A, PE control),
// as efb_copy uses them. `ignored`: the rectangle is outside the EFB.
struct CopyArea {
    u32 left{0}, top{0}, right{0}, bottom{0};
    u32 fmt{0};
    bool ignored{false};
};
bool copy_area(const BpMemory& bp, CopyArea& area, std::string& error);

// Everything an XFB copy computes before touching pixels, so that another
// implementation (the GPU backend) produces the same bytes as efb_copy:
// destination, the output line -> source row map, the copy filter, gamma.
// The RAM rows are dest + y * stride, width * 2 bytes each (YUYV).
struct XfbPlan {
    u32 left{0}, top{0}, width{0}, src_h{0}, lines{0};
    u32 dest{0}, stride{0};
    std::array<u32, 3> weights{}; // previous, current, next row (sum >> 6)
    bool wrap{false};             // the weights sum to 128 or more: 9-bit wrap
    bool six{false};              // RGBA6 EFB: 6-bit channels expanded
    u32 clamp_top{0}, clamp_bottom{0}; // rows the filter may read (inclusive)
    std::array<u8, 256> gamma{};  // output value per filtered value
    std::vector<u32> rows;        // source row (0..src_h-1) of each output line
};
bool xfb_plan(u32 trigger, const BpMemory& bp, const CopyArea& area, u32 phys_mask, XfbPlan& plan, std::string& error);

// A clear (BP 0x52 bit 11) as whole-value writes: the 24-bit colour and/or depth
// written to every pixel of the area. `whole` is false when only part of a
// pixel changes (RGBA6 with only colour or only alpha enabled).
struct ClearPlan {
    bool color{false}, depth{false}, whole{true};
    u32 color_value{0}, depth_value{0};
};
ClearPlan clear_plan(const BpMemory& bp, const CopyArea& area);

// Executes the copy and/or clear for the BP 0x52 value `trigger`, with the
// other registers taken from `bp`. Returns false with a message for
// configurations that are not implemented or that write outside RAM.
bool efb_copy(u32 trigger, const BpMemory& bp, Efb& efb, mem::Bus& bus, u32 phys_mask, CopyResult& result,
              std::string& error);

} // namespace orca::gpu::sw
