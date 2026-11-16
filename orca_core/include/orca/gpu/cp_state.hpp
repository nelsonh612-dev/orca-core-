// Orca Core — CP (command processor) vertex state: VCD, VAT, arrays, matrix indices.
//
// Sources (Dolphin, read as information only): VideoCommon/CPMemory.h for the
// register numbers and bit layouts; LoadCPReg as found in an older
// VertexLoaderManager.cpp (commit 52af077) for what each load writes; the
// read functions in VertexLoader_Position/Normal/Color/TextCoord.cpp for the
// number of bytes every component takes.
//   0x30 MATINDEX_A, 0x40 MATINDEX_B          stored as written (low nibble ignored)
//   0x50 VCD low (bits 0-16), 0x60 VCD high   stored as written (low nibble ignored)
//   0x70/0x80/0x90 + n  VAT group 0/1/2 of format n (n must be 0-7)
//   0xA0 + n array base, 0xB0 + n array stride (& 0xFF), n = 0-15
//   0x00/0x10/0x20      accepted and ignored (performance counters per the
//                       CPMemory.h notes)
// Anything else, and VAT loads with n >= 8 (Dolphin only asserts), is an error.
// Unverified (older Dolphin version): the old code OR-ed a VCD-low value with
// bits above 16 into the high half; Orca keeps the two halves separate.
//
// Vertex size: one byte per enabled matrix index (VCD low bits 0-8), then for
// each component: direct = element size x count; 8-/16-bit index = 1/2 bytes;
// a normal with 3 vectors, an index and VAT bit 31 (NormalIndex3) takes three
// indices. Element sizes: u8/s8 1, u16/s16 2, float (and formats 5-7) 4.
// Colour sizes: RGB565 2, RGB888 3, RGB888x 4, RGBA4444 2, RGBA6666 3,
// RGBA8888 4; colour formats 6 and 7 are an error.
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <string>

namespace orca::gpu {

namespace vcf { inline constexpr u32 NONE = 0, DIRECT = 1, INDEX8 = 2, INDEX16 = 3; }

struct CpState {
    u32 matrix_index_a{0}, matrix_index_b{0};
    u32 vcd_lo{0}, vcd_hi{0};
    std::array<std::array<u32, 3>, 8> vat{}; // [format][group]
    std::array<u32, 16> array_base{}, array_stride{};

    bool load(u8 reg, u32 value, std::string& error);

    // Field access (bit layouts from CPMemory.h).
    u32 pos_desc() const { return (vcd_lo >> 9) & 3U; }
    u32 normal_desc() const { return (vcd_lo >> 11) & 3U; }
    u32 color_desc(unsigned i) const { return (vcd_lo >> (13U + 2U * i)) & 3U; }
    u32 tex_desc(unsigned i) const { return (vcd_hi >> (2U * i)) & 3U; }
    bool pos_mtx_idx() const { return (vcd_lo & 1U) != 0U; }
    bool tex_mtx_idx(unsigned i) const { return ((vcd_lo >> (1U + i)) & 1U) != 0U; }

    // Bytes per vertex for VAT `n`; returns false (with a message) for formats
    // that cannot be sized.
    bool vertex_size(unsigned n, u32& size, std::string& error) const;
};

// Per-component VAT fields for format n.
struct VatFields {
    u32 pos_elems, pos_fmt, pos_frac;
    u32 nrm_elems, nrm_fmt;
    u32 col_elems[2], col_fmt[2];
    u32 tex_elems[8], tex_fmt[8], tex_frac[8];
    bool byte_dequant, normal_index3;
};
VatFields decode_vat(const std::array<u32, 3>& g);

} // namespace orca::gpu
