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
//  * TextureEncoder.cpp (software encoder): block sizes, block counts
//    ((width >> half) >> log2(block width)) + 1, texel packing per format,
//    6-bit expansion, box filters (6-bit sum s -> s + (s >> 6); 8-bit sum >> 2),
//    intensity I = (4096 + 66 R + 129 G + 25 B) >> 8, depth copies
//    (Z bytes high/mid/low play the role of R/G/B).
//  * SWEfbInterface.cpp: vertical copy filter (weights 0-1 previous row, 2-4
//    current row, 5-6 next row, sum >> 6 clamped to 255, alpha untouched),
//    row clamping (clamp_top/bottom), gamma (pow(c/255, 1/gamma)*255,
//    truncated), XFB encoding (BT.601 YUV with rounding, 1/4-1/2-1/4 chroma
//    filter, Y + 16, UV + 128, border pixels repeated), EFB pixel layout.
//  * BPFunctions.cpp ClearScreen: alpha is not written unless the format is
//    RGBA6_Z24; colour quantized to RGB565 and depth to 16 bits for
//    RGB565_Z16 (Z16 -> Z24 = (z & 0xFFFF00) | (z >> 16)).
//  * TextureDecoder.h: XFB memory layout "YUVY" (Y, U, Y, V bytes).
//
// Orca choices / pending hardware verification (also listed in docs/ESTADO.md):
//  * Dolphin applies the copy filter and gamma to texture copies in its
//    hardware backends (PR 10204, PR 10466) but its software encoder does not
//    (Dolphin issue 14020). Orca applies them with the XFB formulas, per EFB
//    pixel, before the box filter and before format packing; depth copies are
//    not filtered. The exact shader (TextureConversionShader.cpp) has not been
//    read.
//  * Texels past the rectangle (block padding) read the EFB with linear
//    addressing, as Dolphin's encoder does; reads past the end of the EFB give 0.
//  * Dolphin's half-scale depth encoder orders RG8/GB8 bytes differently from
//    its full-scale one; Orca uses the full-scale order for both.
//  * XFB copies: the rectangle must start on an even column and have an even
//    width, and the vertical scale must be exactly 1.0 (scaling needs
//    CopyRegion.h, not available); otherwise the copy stops with an error.
#pragma once
#include "orca/gpu/bp_memory.hpp"
#include "orca/gpu/sw/renderer.hpp"
#include "orca/memory/bus.hpp"

#include <string>

namespace orca::gpu::sw {

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

// Executes the copy and/or clear for the BP 0x52 value `trigger`, with the
// other registers taken from `bp`. Returns false with a message for
// configurations that are not implemented or that write outside RAM.
bool efb_copy(u32 trigger, const BpMemory& bp, Efb& efb, mem::Bus& bus, u32 phys_mask, CopyResult& result,
              std::string& error);

} // namespace orca::gpu::sw
