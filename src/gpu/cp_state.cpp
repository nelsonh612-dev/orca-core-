// Orca Core — CP vertex state. See cp_state.hpp for sources and rules.
#include "orca/gpu/cp_state.hpp"

#include <bit>
#include <cstdio>

namespace orca::gpu {
namespace {

u32 field(u32 v, unsigned shift, unsigned bits) { return (v >> shift) & ((1U << bits) - 1U); }
u32 element_size(u32 fmt) { return fmt <= 1U ? 1U : (fmt <= 3U ? 2U : 4U); }
u32 index_size(u32 desc) { return desc == vcf::INDEX8 ? 1U : 2U; }

} // namespace

VatFields decode_vat(const std::array<u32, 3>& g) {
    VatFields f{};
    f.pos_elems = field(g[0], 0, 1);
    f.pos_fmt = field(g[0], 1, 3);
    f.pos_frac = field(g[0], 4, 5);
    f.nrm_elems = field(g[0], 9, 1);
    f.nrm_fmt = field(g[0], 10, 3);
    f.col_elems[0] = field(g[0], 13, 1);
    f.col_fmt[0] = field(g[0], 14, 3);
    f.col_elems[1] = field(g[0], 17, 1);
    f.col_fmt[1] = field(g[0], 18, 3);
    f.tex_elems[0] = field(g[0], 21, 1);
    f.tex_fmt[0] = field(g[0], 22, 3);
    f.tex_frac[0] = field(g[0], 25, 5);
    f.byte_dequant = field(g[0], 30, 1) != 0U;
    f.normal_index3 = field(g[0], 31, 1) != 0U;
    f.tex_elems[1] = field(g[1], 0, 1);  f.tex_fmt[1] = field(g[1], 1, 3);  f.tex_frac[1] = field(g[1], 4, 5);
    f.tex_elems[2] = field(g[1], 9, 1);  f.tex_fmt[2] = field(g[1], 10, 3); f.tex_frac[2] = field(g[1], 13, 5);
    f.tex_elems[3] = field(g[1], 18, 1); f.tex_fmt[3] = field(g[1], 19, 3); f.tex_frac[3] = field(g[1], 22, 5);
    f.tex_elems[4] = field(g[1], 27, 1); f.tex_fmt[4] = field(g[1], 28, 3); f.tex_frac[4] = field(g[2], 0, 5);
    f.tex_elems[5] = field(g[2], 5, 1);  f.tex_fmt[5] = field(g[2], 6, 3);  f.tex_frac[5] = field(g[2], 9, 5);
    f.tex_elems[6] = field(g[2], 14, 1); f.tex_fmt[6] = field(g[2], 15, 3); f.tex_frac[6] = field(g[2], 18, 5);
    f.tex_elems[7] = field(g[2], 23, 1); f.tex_fmt[7] = field(g[2], 24, 3); f.tex_frac[7] = field(g[2], 27, 5);
    return f;
}

bool CpState::load(u8 reg, u32 value, std::string& error) {
    const u32 sub = reg & 0xF0U, n = reg & 0x0FU;
    char b[80];
    switch (sub) {
    case 0x00: case 0x10: case 0x20: return true;
    case 0x30: matrix_index_a = value; return true;
    case 0x40: matrix_index_b = value; return true;
    case 0x50: vcd_lo = value; return true;
    case 0x60: vcd_hi = value; return true;
    case 0x70: case 0x80: case 0x90:
        if (n >= 8U) {
            std::snprintf(b, sizeof b, "CP load %02X: vertex format %u does not exist", reg, n);
            error = b;
            return false;
        }
        vat[n][(sub - 0x70U) >> 4] = value;
        return true;
    case 0xA0: array_base[n] = value; return true;
    case 0xB0: array_stride[n] = value & 0xFFU; return true;
    default:
        std::snprintf(b, sizeof b, "CP load %02X (value %08X) is not handled", reg, value);
        error = b;
        return false;
    }
}

bool CpState::vertex_size(unsigned n, u32& size, std::string& error) const {
    const VatFields f = decode_vat(vat[n & 7U]);
    u32 s = static_cast<u32>(std::popcount(vcd_lo & 0x1FFU));
    const u32 pd = pos_desc();
    if (pd == vcf::DIRECT) s += element_size(f.pos_fmt) * (f.pos_elems != 0U ? 3U : 2U);
    else if (pd != vcf::NONE) s += index_size(pd);
    const u32 nd = normal_desc();
    const u32 vectors = f.nrm_elems != 0U ? 3U : 1U;
    if (nd == vcf::DIRECT) s += element_size(f.nrm_fmt) * 3U * vectors;
    else if (nd != vcf::NONE) s += index_size(nd) * ((vectors == 3U && f.normal_index3) ? 3U : 1U);
    static constexpr u32 kColorSize[6] = {2, 3, 4, 2, 3, 4};
    for (unsigned i = 0; i < 2; ++i) {
        const u32 cd = color_desc(i);
        if (cd == vcf::NONE) continue;
        if (f.col_fmt[i] > 5U) {
            char b[80];
            std::snprintf(b, sizeof b, "vertex format %u: colour %u uses invalid format %u", n, i, f.col_fmt[i]);
            error = b;
            return false;
        }
        s += cd == vcf::DIRECT ? kColorSize[f.col_fmt[i]] : index_size(cd);
    }
    for (unsigned i = 0; i < 8; ++i) {
        const u32 td = tex_desc(i);
        if (td == vcf::DIRECT) s += element_size(f.tex_fmt[i]) * (f.tex_elems[i] != 0U ? 2U : 1U);
        else if (td != vcf::NONE) s += index_size(td);
    }
    size = s;
    return true;
}

} // namespace orca::gpu
