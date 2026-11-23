// Orca Core — XF (transform unit) memory and registers.
//
// Source: Dolphin's VideoCommon/XFMemory.h and XFStructs.cpp, read as
// information only. The XF is addressed in 32-bit words:
//   0x000-0x0FF position/texture matrices   0x400-0x45F normal matrices
//   0x500-0x5FF post-transform matrices     0x600-0x67F lights
//   0x1000-0x1057 registers (viewport 0x101A, projection 0x1020, texgen 0x1040...)
// Words in 0x0680-0x0FFF have no documented use; like Dolphin, writes there are
// stored. A load starting past 0x1057 is reported as an error (Dolphin ignores
// it with a warning; the hardware behaviour is unknown). A load that runs past
// 0x1057 keeps the words up to 0x1057 and reports the rest as an error
// (Dolphin silently drops them). Registers store the value as written.
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <span>
#include <string>

namespace orca::gpu {

namespace xf {
inline constexpr u32 POS_MATRICES = 0x000, NORMAL_MATRICES = 0x400, POST_MATRICES = 0x500, LIGHTS = 0x600;
inline constexpr u32 REGISTERS_START = 0x1000, REGISTERS_END = 0x1058; // exclusive
inline constexpr u32 ERROR_REG = 0x1000, CLIP_DISABLE = 0x1005, VTX_SPECS = 0x1008, NUM_CHAN = 0x1009,
                     AMB_COLOR0 = 0x100A, MAT_COLOR0 = 0x100C, CHAN0_COLOR = 0x100E, CHAN0_ALPHA = 0x1010,
                     DUAL_TEX = 0x1012, MATRIX_INDEX_A = 0x1018, MATRIX_INDEX_B = 0x1019, VIEWPORT = 0x101A,
                     PROJECTION = 0x1020, NUM_TEXGENS = 0x103F, TEX_MTX_INFO = 0x1040, POST_MTX_INFO = 0x1050;
} // namespace xf

class XfMemory {
public:
    // Writes the big-endian words of an XF load at `address`.
    bool load(u32 address, std::span<const u8> words_be, std::string& error);
    u32 word(u32 address) const { return address < xf::REGISTERS_END ? mem_[address] : 0U; }
    float as_float(u32 address) const;
    void reset() { mem_.fill(0); }

private:
    std::array<u32, xf::REGISTERS_END> mem_{};
};

} // namespace orca::gpu
