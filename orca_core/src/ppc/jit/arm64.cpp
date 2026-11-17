#include "orca/ppc/jit/arm64.hpp"
#include "orca/ppc/registers.hpp"
#include <limits>
#include <cstring>
#if defined(__aarch64__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace orca::ppc::jit {
namespace {
constexpr u32 ldr_w(unsigned rt, unsigned rn, unsigned byte_offset) {
    return 0xB9400000U | ((byte_offset / 4U) << 10U) | (rn << 5U) | rt;
}
constexpr u32 str_w(unsigned rt, unsigned rn, unsigned byte_offset) {
    return 0xB9000000U | ((byte_offset / 4U) << 10U) | (rn << 5U) | rt;
}
constexpr u32 movz_w(unsigned rd, u32 imm16) { return 0x52800000U | ((imm16 & 0xFFFFU) << 5U) | rd; }
constexpr u32 movk_w(unsigned rd, u32 imm16) { return 0x72A00000U | ((imm16 & 0xFFFFU) << 5U) | rd; }
constexpr u32 add_w(unsigned rd, unsigned rn, unsigned rm) { return 0x0B000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 orr_w(unsigned rd, unsigned rn, unsigned rm) { return 0x2A000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 eor_w(unsigned rd, unsigned rn, unsigned rm) { return 0x4A000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 and_w(unsigned rd, unsigned rn, unsigned rm) { return 0x0A000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 mul_w(unsigned rd, unsigned rn, unsigned rm) { return 0x1B007C00U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 adds_w(unsigned rd, unsigned rn, unsigned rm) { return 0x2B000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 mvn_w(unsigned rd, unsigned rm) { return 0x2A2003E0U | (rm << 16U) | rd; }
constexpr u32 subs_w(unsigned rd, unsigned rn, unsigned rm) { return 0x6B000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 mrs_nzcv_x(unsigned rt) { return 0xD53B4200U | rt; }
constexpr u32 msr_nzcv_x(unsigned rt) { return 0xD51B4200U | rt; }
constexpr u32 adcs_w(unsigned rd, unsigned rn, unsigned rm) { return 0x3A000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 sbcs_w(unsigned rd, unsigned rn, unsigned rm) { return 0x7A000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 ubfx_w(unsigned rd, unsigned rn, unsigned lsb, unsigned width) {
    return 0x53000000U | (lsb << 16U) | ((lsb + width - 1U) << 10U) | (rn << 5U) | rd;
}
constexpr u32 lsl_w(unsigned rd, unsigned rn, unsigned shift) {
    return 0x53000000U | ((32U - shift) << 16U) | ((31U - shift) << 10U) | (rn << 5U) | rd;
}
constexpr u32 bic_w(unsigned rd, unsigned rn, unsigned rm) {
    return 0x0A200000U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 cmp_zero_w(unsigned rn) { return 0x7100001FU | (rn << 5U); }
constexpr u32 csel_w(unsigned rd, unsigned rn, unsigned rm, unsigned condition) {
    return 0x1A800000U | (rm << 16U) | (condition << 12U) | (rn << 5U) | rd;
}

constexpr u32 cmp_w(unsigned rn, unsigned rm) {
    return 0x6B00001FU | (rm << 16U) | (rn << 5U);
}

constexpr u32 cset_w(unsigned rd, unsigned condition) {
    return 0x1A9F07E0U | ((condition ^ 1U) << 12U) | rd;
}
constexpr u32 ret() { return 0xD65F03C0U; }

constexpr u32 lslv_x(unsigned rd, unsigned rn, unsigned rm) {
    return 0x9AC02000U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 lsrv_x(unsigned rd, unsigned rn, unsigned rm) {
    return 0x9AC02400U | (rm << 16U) | (rn << 5U) | rd;
}

constexpr u32 sub_x(unsigned rd, unsigned rn, unsigned rm) {
    return 0xCB000000U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 asrv_x(unsigned rd, unsigned rn, unsigned rm) {
    return 0x9AC02800U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 sxtw_x(unsigned rd, unsigned rn) {
    return 0x93407C00U | (rn << 5U) | rd;
}

constexpr u32 clz_w(unsigned rd, unsigned rn) {
    return 0x5AC01000U | (rn << 5U) | rd;
}
constexpr u32 sxtb_w(unsigned rd, unsigned rn) {
    return 0x13001C00U | (rn << 5U) | rd;
}
constexpr u32 sxth_w(unsigned rd, unsigned rn) {
    return 0x13003C00U | (rn << 5U) | rd;
}


constexpr u32 rorv_w(unsigned rd, unsigned rn, unsigned rm) {
    return 0x1AC02C00U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 sub_w(unsigned rd, unsigned rn, unsigned rm) {
    return 0x4B000000U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 mask_mb_me(unsigned mb, unsigned me) {
    u32 mask = 0;
    for (unsigned bit = 0; bit < 32U; ++bit) {
        const bool selected = mb <= me ? (bit >= mb && bit <= me)
                                       : (bit >= mb || bit <= me);
        if (selected) mask |= (1U << (31U - bit));
    }
    return mask;
}

void load_imm(std::vector<u32>& out, unsigned reg, u32 value) {
    out.push_back(movz_w(reg, value & 0xFFFFU));
    if ((value >> 16U) != 0U) out.push_back(movk_w(reg, value >> 16U));
}
void emit_cr0(std::vector<u32>& out, unsigned result_reg) {
    out.push_back(cmp_zero_w(result_reg));
    out.push_back(cset_w(12, 0xBU)); // LT
    out.push_back(lsl_w(12, 12, 31));
    out.push_back(cset_w(14, 0xCU)); // GT
    out.push_back(lsl_w(14, 14, 30));
    out.push_back(cset_w(15, 0x0U)); // EQ
    out.push_back(lsl_w(15, 15, 29));
    out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
    out.push_back(ubfx_w(13, 9, 31, 1)); // sticky XER[SO]
    out.push_back(lsl_w(13, 13, 28));
    out.push_back(orr_w(12, 12, 14));
    out.push_back(orr_w(12, 12, 15));
    out.push_back(orr_w(12, 12, 13));
    out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
    load_imm(out, 14, 0x0FFFFFFFU);
    out.push_back(and_w(9, 9, 14));
    out.push_back(orr_w(9, 9, 12));
    out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
}


void emit_cr_field(std::vector<u32>& out, unsigned field, bool is_signed) {
    const unsigned lt_cond = is_signed ? 0xBU : 0x3U; // LT / LO
    const unsigned gt_cond = is_signed ? 0xCU : 0x8U; // GT / HI
    const unsigned shift = 28U - (field & 7U) * 4U;
    out.push_back(cset_w(12, lt_cond));
    out.push_back(lsl_w(12, 12, 3));
    out.push_back(cset_w(14, gt_cond));
    out.push_back(lsl_w(14, 14, 2));
    out.push_back(cset_w(15, 0x0U)); // EQ
    out.push_back(lsl_w(15, 15, 1));
    out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
    out.push_back(ubfx_w(13, 9, 31, 1)); // XER[SO]
    out.push_back(orr_w(12, 12, 14));
    out.push_back(orr_w(12, 12, 15));
    out.push_back(orr_w(12, 12, 13));
    if (shift != 0U) out.push_back(lsl_w(12, 12, shift));
    out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
    load_imm(out, 14, ~(0xFU << shift));
    out.push_back(and_w(9, 9, 14));
    out.push_back(orr_w(9, 9, 12));
    out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
}

bool translate_one(u32 ins, std::vector<u32>& out) {
    const unsigned op = ins >> 26U;
    const unsigned rt = (ins >> 21U) & 31U;
    const unsigned ra = (ins >> 16U) & 31U;
    const unsigned rb = (ins >> 11U) & 31U;
    const u32 imm = ins & 0xFFFFU;
    // x0 = CpuState*, w9/w10/w11 are scratch. Architectural r0 is ordinary
    // storage except where the ISA explicitly defines RA=0 as literal zero.
    if (op == 8U || op == 12U || op == 13U) { // subfic / addic / addic.
        // Both instructions read architectural GPR[RA], including RA=0.
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        load_imm(out, 10, static_cast<u32>(static_cast<s32>(static_cast<s16>(imm))));
        if (op != 8U) out.push_back(adds_w(11, 9, 10)); // unsigned carry-out
        else out.push_back(subs_w(11, 10, 9));         // C=1 means no borrow
        out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
        out.push_back(mrs_nzcv_x(12));
        out.push_back(ubfx_w(12, 12, 29, 1));
        out.push_back(lsl_w(12, 12, 29));
        load_imm(out, 13, XER_CA);
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
        out.push_back(bic_w(9, 9, 13));
        out.push_back(orr_w(9, 9, 12));
        out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
        if (op == 13U) {
            // addic. updates CR0 from the signed result and the post-op XER[SO].
            out.push_back(cmp_zero_w(11));
            out.push_back(cset_w(12, 0xBU)); // LT
            out.push_back(lsl_w(12, 12, 31));
            out.push_back(cset_w(14, 0xCU)); // GT
            out.push_back(lsl_w(14, 14, 30));
            out.push_back(cset_w(15, 0x0U)); // EQ
            out.push_back(lsl_w(15, 15, 29));
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            out.push_back(ubfx_w(13, 9, 31, 1));
            out.push_back(lsl_w(13, 13, 28));
            out.push_back(orr_w(12, 12, 14));
            out.push_back(orr_w(12, 12, 15));
            out.push_back(orr_w(12, 12, 13));
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            load_imm(out, 14, 0x0FFFFFFFU);
            out.push_back(and_w(9, 9, 14));
            out.push_back(orr_w(9, 9, 12));
            out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
        }
        return true;
    }

    if ((op == 10U || op == 11U) && (rt & 3U) == 0U) { // cmplwi / cmpwi, L/reserved bits clear
        const unsigned bf = (rt >> 2U) & 7U;
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        const u32 value = op == 11U ? static_cast<u32>(static_cast<s32>(static_cast<s16>(imm))) : imm;
        load_imm(out, 10, value);
        out.push_back(cmp_w(9, 10));
        emit_cr_field(out, bf, op == 11U);
        return true;
    }
    if (op == 7U) { // mulli: low 32 bits of signed product
        // Unlike addi/addis, RA=0 names architectural GPR0 here.
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        load_imm(out, 10, static_cast<u32>(static_cast<s32>(static_cast<s16>(imm))));
        out.push_back(mul_w(11, 9, 10));
        out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
        return true;
    }
    if (op == 14U || op == 15U) { // addi / addis
        if (ra == 0U) load_imm(out, 9, 0);
        else out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        const u32 addend = op == 15U ? (imm << 16U) : static_cast<u32>(static_cast<s32>(static_cast<s16>(imm)));
        load_imm(out, 10, addend);
        out.push_back(add_w(11, 9, 10));
        out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
        return true;
    }

    if (op == 20U || op == 21U || op == 23U) { // rlwimi / rlwinm / rlwnm
        const unsigned shift = op == 23U ? rb : ((ins >> 11U) & 31U);
        const unsigned mb = (ins >> 6U) & 31U;
        const unsigned me = (ins >> 1U) & 31U;
        const u32 mask = mask_mb_me(mb, me);
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
        if (op == 23U) {
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            load_imm(out, 12, 0U);
            out.push_back(sub_w(12, 12, 10)); // -(RB & 31), equivalent to 32-sh modulo 32
            load_imm(out, 13, 31U);
            out.push_back(and_w(12, 12, 13));
            out.push_back(rorv_w(11, 9, 12));
        } else {
            const unsigned ror = (32U - shift) & 31U;
            load_imm(out, 10, ror);
            out.push_back(rorv_w(11, 9, 10));
        }
        load_imm(out, 10, mask);
        out.push_back(and_w(11, 11, 10));
        if (op == 20U) {
            out.push_back(ldr_w(12, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            load_imm(out, 13, ~mask);
            out.push_back(and_w(12, 12, 13));
            out.push_back(orr_w(11, 11, 12));
        }
        out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        if ((ins & 1U) != 0U) emit_cr0(out, 11);
        return true;
    }
    if (op == 19U && (ins & 1U) == 0U) {
        const unsigned xo = (ins >> 1U) & 0x3FFU;
        if (xo == 0U) { // mcrf: copy one CR field, preserve the other seven
            const unsigned dst = (ins >> 23U) & 7U;
            const unsigned src = (ins >> 18U) & 7U;
            const unsigned src_shift = 28U - src * 4U;
            const unsigned dst_shift = 28U - dst * 4U;
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            out.push_back(ubfx_w(10, 9, src_shift, 4U));
            if (dst_shift != 0U) out.push_back(lsl_w(10, 10, dst_shift));
            load_imm(out, 11, ~(0xFU << dst_shift));
            out.push_back(and_w(9, 9, 11));
            out.push_back(orr_w(9, 9, 10));
            out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            return true;
        }
        if (xo == 33U || xo == 129U || xo == 193U || xo == 225U ||
            xo == 257U || xo == 289U || xo == 417U || xo == 449U) {
            const unsigned bt = (ins >> 21U) & 31U;
            const unsigned ba = (ins >> 16U) & 31U;
            const unsigned bb = (ins >> 11U) & 31U;
            const unsigned bit_a = 31U - ba;
            const unsigned bit_b = 31U - bb;
            const unsigned bit_t = 31U - bt;
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            out.push_back(ubfx_w(10, 9, bit_a, 1U));
            out.push_back(ubfx_w(11, 9, bit_b, 1U));
            if (xo == 33U) { // crnor
                out.push_back(orr_w(12, 10, 11));
                out.push_back(mvn_w(12, 12));
            } else if (xo == 129U) { // crandc
                out.push_back(mvn_w(11, 11));
                out.push_back(and_w(12, 10, 11));
            } else if (xo == 193U) { // crxor
                out.push_back(eor_w(12, 10, 11));
            } else if (xo == 225U) { // crnand
                out.push_back(and_w(12, 10, 11));
                out.push_back(mvn_w(12, 12));
            } else if (xo == 257U) { // crand
                out.push_back(and_w(12, 10, 11));
            } else if (xo == 289U) { // creqv
                out.push_back(eor_w(12, 10, 11));
                out.push_back(mvn_w(12, 12));
            } else if (xo == 417U) { // crorc
                out.push_back(mvn_w(11, 11));
                out.push_back(orr_w(12, 10, 11));
            } else { // cror
                out.push_back(orr_w(12, 10, 11));
            }
            // Complement operations produce all-one upper bits in W registers;
            // CR logical instructions consume only the low boolean bit.
            load_imm(out, 13, 1U);
            out.push_back(and_w(12, 12, 13));
            if (bit_t != 0U) out.push_back(lsl_w(12, 12, bit_t));
            load_imm(out, 13, ~(1U << bit_t));
            out.push_back(and_w(9, 9, 13));
            out.push_back(orr_w(9, 9, 12));
            out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            return true;
        }
    }

    if (op == 28U || op == 29U) { // andi. / andis. always update CR0
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
        load_imm(out, 10, op == 29U ? (imm << 16U) : imm);
        out.push_back(and_w(11, 9, 10));
        out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        emit_cr0(out, 11);
        return true;
    }
    if (op == 24U || op == 25U || op == 26U || op == 27U) { // ori/oris/xori/xoris
        out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
        load_imm(out, 10, op == 25U || op == 27U ? (imm << 16U) : imm);
        out.push_back((op == 24U || op == 25U) ? orr_w(11, 9, 10) : eor_w(11, 9, 10));
        out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
        return true;
    }
    if (op == 31U) {
        // Conservative XO subset: OE=0 only. Rc is supported by rebuilding
        // CR0 from the signed result and the sticky XER[SO] bit.
        const unsigned xo9 = (ins >> 1U) & 0x1FFU;
        const bool oe = (ins & 0x400U) != 0U;
        if (xo9 == 19U && (ins & 1U) == 0U) { // mfcr
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            return true;
        }
        if (xo9 == 144U && (ins & 1U) == 0U) { // mtcrf
            const unsigned fxm = (ins >> 12U) & 0xFFU;
            u32 mask = 0U;
            for (unsigned field = 0; field < 8U; ++field)
                if ((fxm & (0x80U >> field)) != 0U) mask |= 0xFU << (28U - field * 4U);
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            load_imm(out, 11, mask);
            out.push_back(and_w(9, 9, 11));
            load_imm(out, 12, ~mask);
            out.push_back(and_w(10, 10, 12));
            out.push_back(orr_w(10, 10, 9));
            out.push_back(str_w(10, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            return true;
        }

        const bool rc = (ins & 1U) != 0U;
        if (!oe && (xo9 == 138U || xo9 == 136U)) {
            // adde/subfe consume the architectural XER[CA] as the carry-in.
            out.push_back(ldr_w(12, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            out.push_back(ubfx_w(12, 12, 29, 1));
            out.push_back(lsl_w(12, 12, 29));
            out.push_back(msr_nzcv_x(12));
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            if (xo9 == 138U) out.push_back(adcs_w(11, 9, 10));
            else out.push_back(sbcs_w(11, 10, 9)); // RB - RA - (1 - CA)
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            out.push_back(mrs_nzcv_x(12));
            out.push_back(ubfx_w(12, 12, 29, 1));
            out.push_back(lsl_w(12, 12, 29));
            load_imm(out, 13, XER_CA);
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            out.push_back(bic_w(9, 9, 13));
            out.push_back(orr_w(9, 9, 12));
            out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            if (rc) {
                out.push_back(cmp_zero_w(11));
                out.push_back(cset_w(12, 0xBU));
                out.push_back(lsl_w(12, 12, 31));
                out.push_back(cset_w(14, 0xCU));
                out.push_back(lsl_w(14, 14, 30));
                out.push_back(cset_w(15, 0x0U));
                out.push_back(lsl_w(15, 15, 29));
                out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
                out.push_back(ubfx_w(13, 9, 31, 1));
                out.push_back(lsl_w(13, 13, 28));
                out.push_back(orr_w(12, 12, 14));
                out.push_back(orr_w(12, 12, 15));
                out.push_back(orr_w(12, 12, 13));
                out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
                load_imm(out, 14, 0x0FFFFFFFU);
                out.push_back(and_w(9, 9, 14));
                out.push_back(orr_w(9, 9, 12));
                out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            }
            return true;
        }
        if (!oe && (xo9 == 266U || xo9 == 10U || xo9 == 40U || xo9 == 8U)) {
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            if (xo9 == 266U) out.push_back(add_w(11, 9, 10));
            else if (xo9 == 10U) out.push_back(adds_w(11, 9, 10));
            else if (xo9 == 40U) {
                // subf RT,RA,RB = RB - RA = RB + ~RA + 1.
                out.push_back(mvn_w(9, 9));
                load_imm(out, 12, 1U);
                out.push_back(add_w(11, 9, 10));
                out.push_back(add_w(11, 11, 12));
            } else {
                // subfc RT,RA,RB = RB - RA; SUBS sets C=1 when no borrow.
                out.push_back(subs_w(11, 10, 9));
            }
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            if (xo9 == 10U || xo9 == 8U) {
                out.push_back(mrs_nzcv_x(12));
                out.push_back(ubfx_w(12, 12, 29, 1));
                out.push_back(lsl_w(12, 12, 29));
                load_imm(out, 13, XER_CA);
                out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
                out.push_back(bic_w(9, 9, 13));
                out.push_back(orr_w(9, 9, 12));
                out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            }
            if (rc) {
                out.push_back(cmp_zero_w(11));
                out.push_back(cset_w(12, 0xBU));
                out.push_back(lsl_w(12, 12, 31));
                out.push_back(cset_w(14, 0xCU));
                out.push_back(lsl_w(14, 14, 30));
                out.push_back(cset_w(15, 0x0U));
                out.push_back(lsl_w(15, 15, 29));
                out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
                out.push_back(ubfx_w(13, 9, 31, 1));
                out.push_back(lsl_w(13, 13, 28));
                out.push_back(orr_w(12, 12, 14));
                out.push_back(orr_w(12, 12, 15));
                out.push_back(orr_w(12, 12, 13));
                out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
                load_imm(out, 14, 0x0FFFFFFFU);
                out.push_back(and_w(9, 9, 14));
                out.push_back(orr_w(9, 9, 12));
                out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
            }
            return true;
        }
    }
    if (op == 31U) {
        const unsigned xo = (ins >> 1U) & 0x3FFU;
        const bool rc = (ins & 1U) != 0U;


        if ((xo == 0U || xo == 32U) && (rt & 3U) == 0U && (ins & 1U) == 0U) { // cmpw / cmplw
            const unsigned bf = (rt >> 2U) & 7U;
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            out.push_back(cmp_w(9, 10));
            emit_cr_field(out, bf, xo == 0U);
            return true;
        }
        if (xo == 24U || xo == 536U || xo == 792U || xo == 824U || xo == 26U || xo == 954U || xo == 922U) {
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            if (xo == 24U || xo == 536U) {
                out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
                // PPC uses six shift bits. The X-register form preserves the
                // 32..63 zero-result cases for zero-extended source operands.
                out.push_back(xo == 24U ? lslv_x(11, 9, 10) : lsrv_x(11, 9, 10));
            } else if (xo == 792U) { // sraw: signed 32-bit arithmetic shift and XER[CA]
                out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
                load_imm(out, 12, 63U);
                out.push_back(and_w(10, 10, 12));
                out.push_back(sxtw_x(9, 9));
                out.push_back(asrv_x(11, 9, 10));
                // CA = negative source AND at least one discarded one bit.
                load_imm(out, 12, 1U);
                out.push_back(lslv_x(12, 12, 10));
                load_imm(out, 14, 1U);
                out.push_back(sub_x(12, 12, 14));
                out.push_back(and_w(13, 9, 12));
                out.push_back(cmp_zero_w(13));
                out.push_back(cset_w(14, 0x1U)); // discarded bits nonzero
                out.push_back(ubfx_w(15, 9, 31, 1));
                out.push_back(and_w(14, 14, 15));
                out.push_back(lsl_w(14, 14, 29));
                load_imm(out, 15, XER_CA);
                out.push_back(ldr_w(13, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
                out.push_back(bic_w(13, 13, 15));
                out.push_back(orr_w(13, 13, 14));
                out.push_back(str_w(13, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            } else if (xo == 824U) { // srawi: fixed shift and XER[CA]
                const unsigned sh = rb;
                out.push_back(0x13000000U | (sh << 16U) | (31U << 10U) | (9U << 5U) | 11U);
                load_imm(out, 10, sh == 0U ? 0U : ((1U << sh) - 1U));
                out.push_back(and_w(13, 9, 10));
                out.push_back(cmp_zero_w(13));
                out.push_back(cset_w(14, 0x1U));
                out.push_back(ubfx_w(15, 9, 31, 1));
                out.push_back(and_w(14, 14, 15));
                out.push_back(lsl_w(14, 14, 29));
                load_imm(out, 15, XER_CA);
                out.push_back(ldr_w(13, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
                out.push_back(bic_w(13, 13, 15));
                out.push_back(orr_w(13, 13, 14));
                out.push_back(str_w(13, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            } else if (xo == 26U) {
                out.push_back(clz_w(11, 9));
            } else if (xo == 954U) {
                out.push_back(sxtb_w(11, 9));
            } else {
                out.push_back(sxth_w(11, 9));
            }
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            if (rc) emit_cr0(out, 11);
            return true;
        }
        if (xo == 28U || xo == 60U || xo == 124U || xo == 284U ||
            xo == 316U || xo == 412U || xo == 444U || xo == 476U) {
            // X-form logical operations: RS is encoded in the RT field and RA is the destination.
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            if (xo == 28U) out.push_back(and_w(11, 9, 10));                 // and
            else if (xo == 60U) { out.push_back(mvn_w(10, 10)); out.push_back(and_w(11, 9, 10)); } // andc
            else if (xo == 124U) { out.push_back(orr_w(11, 9, 10)); out.push_back(mvn_w(11, 11)); } // nor
            else if (xo == 284U) { out.push_back(eor_w(11, 9, 10)); out.push_back(mvn_w(11, 11)); } // eqv
            else if (xo == 316U) out.push_back(eor_w(11, 9, 10));          // xor
            else if (xo == 412U) { out.push_back(mvn_w(10, 10)); out.push_back(orr_w(11, 9, 10)); } // orc
            else if (xo == 444U) out.push_back(orr_w(11, 9, 10));          // or
            else { out.push_back(and_w(11, 9, 10)); out.push_back(mvn_w(11, 11)); } // nand
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            if (rc) emit_cr0(out, 11);
            return true;
        }
    }
    return false;
}
} // namespace

Block compile_arm64(std::span<const u32> words, u32 start_pc) {
    Block b; b.start_pc = start_pc; b.end_pc = start_pc;
    // Bound blocks to a single 4 KiB guest code page; no control flow or
    // memory operations are crossed in this initial translator.
    if ((start_pc & 3U) != 0U) return b;
    bool control_flow_exit = false;
    for (u32 ins : words) {
        if ((b.end_pc >> 12U) != (start_pc >> 12U)) break;

        // Conditional immediate branch (bc). Evaluate the architectural
        // BO/BI tests in generated code, including CTR decrement, then select
        // target or fall-through PC. This instruction always ends the block.
        if ((ins >> 26U) == 16U) {
            const unsigned bo = (ins >> 21U) & 31U;
            const unsigned bi = (ins >> 16U) & 31U;
            if ((bo & 4U) == 0U) {
                b.host_words.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, ctr))));
                load_imm(b.host_words, 10, 1U);
                b.host_words.push_back(sub_w(11, 9, 10));
                b.host_words.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, ctr))));
                b.host_words.push_back(cmp_zero_w(11));
                b.host_words.push_back(cset_w(12, (bo & 2U) != 0U ? 0x0U : 0x1U));
            } else {
                load_imm(b.host_words, 12, 1U);
            }
            if ((bo & 16U) == 0U) {
                b.host_words.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, cr))));
                b.host_words.push_back(ubfx_w(10, 9, 31U - bi, 1U));
                b.host_words.push_back(cmp_zero_w(10));
                b.host_words.push_back(cset_w(14, (bo & 8U) != 0U ? 0x1U : 0x0U));
            } else {
                load_imm(b.host_words, 14, 1U);
            }
            b.host_words.push_back(and_w(12, 12, 14));
            b.host_words.push_back(cmp_zero_w(12));
            const u32 encoded_disp = ins & 0x0000FFFCU;
            const s32 displacement = (encoded_disp & 0x00008000U) != 0U
                ? static_cast<s32>(encoded_disp | 0xFFFF0000U)
                : static_cast<s32>(encoded_disp);
            const u32 target = (ins & 2U) != 0U
                ? static_cast<u32>(displacement)
                : b.end_pc + static_cast<u32>(displacement);
            load_imm(b.host_words, 13, target);
            load_imm(b.host_words, 14, b.end_pc + 4U);
            b.host_words.push_back(csel_w(9, 13, 14, 0x1U)); // NE => taken
            b.host_words.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, pc))));
            if ((ins & 1U) != 0U) {
                load_imm(b.host_words, 9, b.end_pc + 4U);
                b.host_words.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, lr))));
            }
            ++b.guest_instructions;
            b.end_pc += 4U;
            control_flow_exit = true;
            break;
        }

        // Unconditional immediate branch (b / bl). Both branch types
        // terminate a block; the dispatcher resumes at the selected PC.
        // Indirect branches (bclr/bcctr) remain in the interpreter.
        if ((ins >> 26U) == 18U) {
            const u32 encoded_disp = ins & 0x03FFFFFCU;
            const s32 displacement = (encoded_disp & 0x02000000U) != 0U
                ? static_cast<s32>(encoded_disp | 0xFC000000U)
                : static_cast<s32>(encoded_disp);
            const u32 target = (ins & 2U) != 0U
                ? static_cast<u32>(displacement)
                : b.end_pc + static_cast<u32>(displacement);
            if ((ins & 1U) != 0U) {
                load_imm(b.host_words, 9, b.end_pc + 4U);
                b.host_words.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, lr))));
            }
            load_imm(b.host_words, 9, target);
            b.host_words.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, pc))));
            ++b.guest_instructions;
            b.end_pc += 4U; // sequential fall-through address, for block metadata
            control_flow_exit = true;
            break;
        }

        if (!translate_one(ins, b.host_words)) break;
        ++b.guest_instructions;
        b.end_pc += 4U;
    }
    if (b.guest_instructions != 0U) {
        if (!control_flow_exit) {
            // Load immediate sequential end_pc into w9 then store PC.
            load_imm(b.host_words, 9, b.end_pc);
            b.host_words.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, pc))));
        }
        b.host_words.push_back(ret());
    }
    return b;
}

ExecutableBlock::ExecutableBlock(const Block& block) noexcept {
#if defined(__aarch64__)
    if (block.guest_instructions == 0U || block.host_words.empty()) return;
    const long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return;
    const std::size_t bytes = block.host_words.size() * sizeof(u32);
    allocation_size_ = (bytes + static_cast<std::size_t>(page) - 1U) &
                       ~(static_cast<std::size_t>(page) - 1U);
    memory_ = mmap(nullptr, allocation_size_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory_ == MAP_FAILED) { memory_ = nullptr; allocation_size_ = 0; return; }
    auto* dst = static_cast<u32*>(memory_);
    for (std::size_t i = 0; i < block.host_words.size(); ++i) dst[i] = block.host_words[i];
    __builtin___clear_cache(static_cast<char*>(memory_), static_cast<char*>(memory_) + bytes);
    if (mprotect(memory_, allocation_size_, PROT_READ | PROT_EXEC) != 0) {
        munmap(memory_, allocation_size_); memory_ = nullptr; allocation_size_ = 0;
    }
#else
    (void)block;
#endif
}

ExecutableBlock::~ExecutableBlock() {
#if defined(__aarch64__)
    if (memory_ != nullptr) munmap(memory_, allocation_size_);
#endif
}

ExecutableBlock::ExecutableBlock(ExecutableBlock&& other) noexcept
    : memory_(other.memory_), allocation_size_(other.allocation_size_) {
    other.memory_ = nullptr;
    other.allocation_size_ = 0;
}

ExecutableBlock& ExecutableBlock::operator=(ExecutableBlock&& other) noexcept {
    if (this == &other) return *this;
#if defined(__aarch64__)
    if (memory_ != nullptr) munmap(memory_, allocation_size_);
#endif
    memory_ = other.memory_;
    allocation_size_ = other.allocation_size_;
    other.memory_ = nullptr;
    other.allocation_size_ = 0;
    return *this;
}

bool ExecutableBlock::execute(CpuState& state) const noexcept {
#if defined(__aarch64__)
    if (memory_ == nullptr) return false;
    using Fn = void (*)(CpuState*);
    static_assert(sizeof(Fn) == sizeof(memory_));
    Fn fn = nullptr;
    std::memcpy(&fn, &memory_, sizeof(fn));
    fn(&state);
    return true;
#else
    (void)state;
    return false;
#endif
}

bool execute_arm64(const Block& block, CpuState& state) noexcept {
    ExecutableBlock executable(block);
    return executable.execute(state);
}

} // namespace orca::ppc::jit
