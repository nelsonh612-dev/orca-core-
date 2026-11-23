// Orca Core — BP (raster/pixel pipeline) registers.
//
// Source: Dolphin's VideoCommon/BPMemory.h (register numbers) and
// BPStructs.cpp (LoadBPReg), read as information only. There are 256 24-bit
// registers. Register 0xFE is a write mask: a load into any other register
// keeps the bits outside the mask and afterwards resets the mask to 0xFFFFFF.
// Reset state: every register 0 except the mask (0xFFFFFF).
#pragma once
#include "orca/core/bits.hpp"

#include <array>

namespace orca::gpu {

namespace bp {
inline constexpr u8 GENMODE = 0x00, IND_MTXA = 0x06, IND_IMASK = 0x0F, IND_CMD = 0x10, SCISSOR_TL = 0x20,
                    SCISSOR_BR = 0x21, LINE_PT_WIDTH = 0x22, RAS1_SS0 = 0x25, RAS1_SS1 = 0x26, IREF = 0x27,
                    TREF = 0x28, SU_SSIZE = 0x30, SU_TSIZE = 0x31, ZMODE = 0x40, BLENDMODE = 0x41,
                    CONSTANT_ALPHA = 0x42, ZCOMPARE = 0x43, FIELD_MASK = 0x44, SET_DRAW_DONE = 0x45,
                    PE_TOKEN = 0x47, PE_TOKEN_INT = 0x48, EFB_TL = 0x49, EFB_WH = 0x4A, EFB_ADDR = 0x4B,
                    EFB_STRIDE = 0x4D, COPY_YSCALE = 0x4E, CLEAR_AR = 0x4F, CLEAR_GB = 0x50, CLEAR_Z = 0x51,
                    TRIGGER_EFB_COPY = 0x52, COPY_FILTER0 = 0x53, COPY_FILTER1 = 0x54, CLEAR_BBOX1 = 0x55,
                    CLEAR_BBOX2 = 0x56, CLEAR_PIXEL_PERF = 0x57, REVBITS = 0x58, SCISSOR_OFFSET = 0x59,
                    PRELOAD_ADDR = 0x60, PRELOAD_TMEM_EVEN = 0x61, PRELOAD_TMEM_ODD = 0x62, PRELOAD_MODE = 0x63,
                    LOAD_TLUT0 = 0x64, LOAD_TLUT1 = 0x65, TEX_INVALIDATE = 0x66, FIELD_MODE = 0x68,
                    TX_SETMODE0 = 0x80, TX_SETMODE1 = 0x84, TX_SETIMAGE0 = 0x88, TX_SETIMAGE1 = 0x8C,
                    TX_SETIMAGE2 = 0x90, TX_SETIMAGE3 = 0x94, TX_SETTLUT = 0x98, TX_SETMODE0_4 = 0xA0,
                    TX_SETMODE1_4 = 0xA4, TX_SETIMAGE0_4 = 0xA8, TX_SETIMAGE1_4 = 0xAC, TX_SETIMAGE2_4 = 0xB0,
                    TX_SETIMAGE3_4 = 0xB4, TX_SETTLUT_4 = 0xB8, TEV_COLOR_ENV = 0xC0, TEV_ALPHA_ENV = 0xC1,
                    TEV_COLOR_RA = 0xE0, TEV_COLOR_BG = 0xE1, FOG_RANGE = 0xE8, FOG_PARAM0 = 0xEE,
                    FOG_B_MAGNITUDE = 0xEF, FOG_B_EXPONENT = 0xF0, FOG_PARAM3 = 0xF1, FOG_COLOR = 0xF2,
                    ALPHA_COMPARE = 0xF3, BIAS = 0xF4, ZTEX2 = 0xF5, TEV_KSEL = 0xF6, BP_MASK = 0xFE;
} // namespace bp

class BpMemory {
public:
    BpMemory() { reset(); }
    void reset() {
        regs_.fill(0);
        regs_[bp::BP_MASK] = 0xFFFFFFU;
    }
    // Applies a load and returns the register's new value.
    u32 load(u8 reg, u32 value) {
        const u32 mask = regs_[bp::BP_MASK];
        const u32 v = ((regs_[reg] & ~mask) | (value & mask)) & 0xFFFFFFU;
        regs_[reg] = v;
        if (reg != bp::BP_MASK) regs_[bp::BP_MASK] = 0xFFFFFFU;
        return v;
    }
    u32 reg(u8 r) const noexcept { return regs_[r]; }

private:
    std::array<u32, 256> regs_{};
};

} // namespace orca::gpu
