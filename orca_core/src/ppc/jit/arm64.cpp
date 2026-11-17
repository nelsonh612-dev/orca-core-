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
constexpr u32 udiv_w(unsigned rd, unsigned rn, unsigned rm) { return 0x1AC00800U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 sdiv_w(unsigned rd, unsigned rn, unsigned rm) { return 0x1AC00C00U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 smull_x(unsigned rd,unsigned rn,unsigned rm) { return 0x9B207C00U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 umull_x(unsigned rd,unsigned rn,unsigned rm) { return 0x9BA07C00U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 lsr_x(unsigned rd,unsigned rn,unsigned shift) { return 0xD3400000U | (shift << 16U) | (63U << 10U) | (rn << 5U) | rd; }
constexpr u32 adds_w(unsigned rd, unsigned rn, unsigned rm) { return 0x2B000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 sub_w(unsigned rd,unsigned rn,unsigned rm) { return 0x4B000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 rorv_w(unsigned rd,unsigned rn,unsigned rm) { return 0x1AC02C00U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 lslv_w(unsigned rd,unsigned rn,unsigned rm) { return 0x1AC02000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 lsrv_w(unsigned rd,unsigned rn,unsigned rm) { return 0x1AC02400U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 asrv_w(unsigned rd,unsigned rn,unsigned rm) { return 0x1AC02800U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 mvn_w(unsigned rd, unsigned rm) { return 0x2A2003E0U | (rm << 16U) | rd; }
constexpr u32 subs_w(unsigned rd, unsigned rn, unsigned rm) { return 0x6B000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 mrs_nzcv_x(unsigned rt) { return 0xD53B4200U | rt; }
constexpr u32 msr_nzcv_x(unsigned rt) { return 0xD51B4200U | rt; }
constexpr u32 adcs_w(unsigned rd, unsigned rn, unsigned rm) { return 0x3A000000U | (rm << 16U) | (rn << 5U) | rd; }
constexpr u32 ubfx_w(unsigned rd, unsigned rn, unsigned lsb, unsigned width) {
    return 0x53000000U | (lsb << 16U) | ((lsb + width - 1U) << 10U) | (rn << 5U) | rd;
}
constexpr u32 lsl_w(unsigned rd, unsigned rn, unsigned shift) {
    return 0x53000000U | ((32U - shift) << 16U) | ((31U - shift) << 10U) | (rn << 5U) | rd;
}
constexpr u32 lsr_w(unsigned rd,unsigned rn,unsigned shift) {
    return 0x53000000U | (shift << 16U) | (31U << 10U) | (rn << 5U) | rd;
}
constexpr u32 bic_w(unsigned rd, unsigned rn, unsigned rm) {
    return 0x0A200000U | (rm << 16U) | (rn << 5U) | rd;
}
constexpr u32 cmp_zero_w(unsigned rn) { return 0x7100001FU | (rn << 5U); }
constexpr u32 sxtb_w(unsigned rd,unsigned rn) { return 0x13001C00U | (rn << 5U) | rd; }
constexpr u32 sxth_w(unsigned rd,unsigned rn) { return 0x13003C00U | (rn << 5U) | rd; }
constexpr u32 clz_w(unsigned rd,unsigned rn) { return 0x5AC01000U | (rn << 5U) | rd; }
constexpr u32 asr_w(unsigned rd,unsigned rn,unsigned shift) { return 0x13000000U | (shift << 16U) | (31U << 10U) | (rn << 5U) | rd; }
constexpr u32 csel_w(unsigned rd,unsigned rn,unsigned rm,unsigned condition) {
    return 0x1A800000U | (rm << 16U) | (condition << 12U) | (rn << 5U) | rd;
}
constexpr u32 cset_w(unsigned rd, unsigned condition) {
    return 0x1A9F07E0U | ((condition ^ 1U) << 12U) | rd;
}
constexpr u32 ret() { return 0xD65F03C0U; }
void load_imm(std::vector<u32>& out, unsigned reg, u32 value) {
    out.push_back(movz_w(reg, value & 0xFFFFU));
    if ((value >> 16U) != 0U) out.push_back(movk_w(reg, value >> 16U));
}

void emit_xer_overflow(std::vector<u32>& out,unsigned nzcv_reg) {
    out.push_back(ubfx_w(13,nzcv_reg,28U,1U));
    out.push_back(lsl_w(13,13,30U));
    out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
    out.push_back(ubfx_w(14,9,31U,1U));
    out.push_back(ubfx_w(15,nzcv_reg,28U,1U));
    out.push_back(orr_w(14,14,15));
    out.push_back(lsl_w(14,14,31U));
    load_imm(out,15,~(XER_OV|XER_SO));
    out.push_back(and_w(9,9,15));
    out.push_back(orr_w(9,9,13));
    out.push_back(orr_w(9,9,14));
    out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
}

void emit_cr0(std::vector<u32>& out,unsigned result_reg) {
    out.push_back(cmp_zero_w(result_reg));
    out.push_back(cset_w(12,0xBU)); // LT
    out.push_back(lsl_w(12,12,31));
    out.push_back(cset_w(14,0xCU)); // GT
    out.push_back(lsl_w(14,14,30));
    out.push_back(cset_w(15,0x0U)); // EQ
    out.push_back(lsl_w(15,15,29));
    out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
    out.push_back(ubfx_w(13,9,31,1));
    out.push_back(lsl_w(13,13,28));
    out.push_back(orr_w(12,12,14));
    out.push_back(orr_w(12,12,15));
    out.push_back(orr_w(12,12,13));
    out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
    load_imm(out,14,0x0FFFFFFFU);
    out.push_back(and_w(9,9,14));
    out.push_back(orr_w(9,9,12));
    out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
}

void emit_cr_compare(std::vector<u32>& out,unsigned bf,unsigned lt_condition,
                     unsigned gt_condition,unsigned eq_condition) {
    out.push_back(cset_w(12,lt_condition));
    out.push_back(lsl_w(12,12,3U));
    out.push_back(cset_w(13,gt_condition));
    out.push_back(lsl_w(13,13,2U));
    out.push_back(cset_w(14,eq_condition));
    out.push_back(lsl_w(14,14,1U));
    out.push_back(orr_w(12,12,13));
    out.push_back(orr_w(12,12,14));
    out.push_back(ldr_w(15,0,static_cast<unsigned>(offsetof(CpuState,xer))));
    out.push_back(ubfx_w(15,15,31U,1U));
    out.push_back(orr_w(12,12,15));
    const unsigned shift=28U-bf*4U;
    if(shift==0U) out.push_back(orr_w(13,12,12));
    else out.push_back(lsl_w(13,12,shift));
    load_imm(out,15,~(0xFU<<shift));
    out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
    out.push_back(and_w(9,9,15));
    out.push_back(orr_w(9,9,13));
    out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
}

bool translate_one(u32 ins, u32 guest_pc, std::vector<u32>& out, bool& terminates) {
    const unsigned op = ins >> 26U;
    const unsigned rt = (ins >> 21U) & 31U;
    const unsigned ra = (ins >> 16U) & 31U;
    const unsigned rb = (ins >> 11U) & 31U;
    const u32 imm = ins & 0xFFFFU;

    if(op==18U) { // b: direct unconditional branch
        const s32 displacement=sign_extend((ins>>2U)&0x00FFFFFFU,24U)*4;
        const u32 target=(ins&2U)!=0U ? static_cast<u32>(displacement)
                                      : guest_pc+static_cast<u32>(displacement);
        if((ins&1U)!=0U) {
            load_imm(out,9,guest_pc+4U);
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,lr))));
        }
        load_imm(out,9,target);
        out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,pc))));
        terminates=true;
        return true;
    }


    if(op==19U) {
        const unsigned xo10=(ins>>1U)&0x3FFU;

        if(xo10==257U || xo10==129U || xo10==289U || xo10==225U ||
           xo10==33U || xo10==449U || xo10==417U || xo10==193U) {
            const unsigned bt=(ins>>21U)&31U;
            const unsigned ba=(ins>>16U)&31U;
            const unsigned bb=(ins>>11U)&31U;
            const unsigned position=31U-bt;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            out.push_back(ubfx_w(10,9,31U-ba,1U));
            out.push_back(ubfx_w(11,9,31U-bb,1U));
            if(xo10==257U) out.push_back(and_w(12,10,11)); // crand
            else if(xo10==129U) { out.push_back(mvn_w(11,11)); out.push_back(and_w(12,10,11)); }
            else if(xo10==289U) { out.push_back(eor_w(12,10,11)); out.push_back(mvn_w(12,12)); }
            else if(xo10==225U) { out.push_back(and_w(12,10,11)); out.push_back(mvn_w(12,12)); }
            else if(xo10==33U) { out.push_back(orr_w(12,10,11)); out.push_back(mvn_w(12,12)); }
            else if(xo10==449U) out.push_back(orr_w(12,10,11));
            else if(xo10==417U) { out.push_back(mvn_w(11,11)); out.push_back(orr_w(12,10,11)); }
            else out.push_back(eor_w(12,10,11)); // crxor
            // Inverted boolean operations produce all-ones words. CR
            // destinations are single bits, so discard every bit but bit 0
            // before shifting or unrelated CR fields would be overwritten.
            out.push_back(ubfx_w(12,12,0U,1U));
            if(position!=0U) out.push_back(lsl_w(12,12,position));
            load_imm(out,13,~(u32{1}<<position));
            out.push_back(and_w(9,9,13));
            out.push_back(orr_w(9,9,12));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            return true;
        }
        if(xo10==0U) { // mcrf BF,BFA: copy one CR nibble without disturbing others
            const unsigned bf=(ins>>23U)&7U;
            const unsigned bfa=(ins>>18U)&7U;
            const unsigned source_shift=28U-bfa*4U;
            const unsigned dest_shift=28U-bf*4U;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            out.push_back(ubfx_w(11,9,source_shift,4U));
            if(dest_shift==0U) out.push_back(orr_w(12,11,11));
            else out.push_back(lsl_w(12,11,dest_shift));
            load_imm(out,13,~(0xFU<<dest_shift));
            out.push_back(and_w(9,9,13));
            out.push_back(orr_w(9,9,12));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            return true;
        }
        const unsigned bo=(ins>>21U)&31U;
        const unsigned bi=(ins>>16U)&31U;
        // Common function-return / indirect-call forms. More complex BO/BI
        // combinations remain on the reference interpreter.
        if((xo10==16U || xo10==528U) && bo==20U && bi==0U) {
            const unsigned target_offset=static_cast<unsigned>(
                xo10==16U ? offsetof(CpuState,lr) : offsetof(CpuState,ctr));
            out.push_back(ldr_w(9,0,target_offset));
            load_imm(out,10,0xFFFFFFFCU);
            out.push_back(and_w(11,9,10));
            if((ins&1U)!=0U) {
                load_imm(out,9,guest_pc+4U);
                out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,lr))));
            }
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,pc))));
            terminates=true;
            return true;
        }
    }
    if(op==16U) { // bc: CTR/CR conditional branch
        const unsigned bo=(ins>>21U)&31U;
        const unsigned bi=(ins>>16U)&31U;
        const s32 displacement=sign_extend((ins>>2U)&0x3FFFU,14U)*4;
        const u32 target=(ins&2U)!=0U ? static_cast<u32>(displacement)
                                      : guest_pc+static_cast<u32>(displacement);
        // BO[2] disables CTR decrement/check; BO[3] inverts CTR!=0.
        if((bo&0x04U)!=0U) {
            load_imm(out,12,1U);
        } else {
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,ctr))));
            load_imm(out,10,1U);
            out.push_back(subs_w(9,9,10));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,ctr))));
            out.push_back(cmp_zero_w(9));
            out.push_back(cset_w(12,1U)); // CTR != 0
            if((bo&0x02U)!=0U) {
                load_imm(out,15,1U);
                out.push_back(eor_w(12,12,15));
            }
        }
        // BO[0] disables CR testing; BO[1] selects expected CR[BI] value.
        if((bo&0x10U)!=0U) {
            load_imm(out,13,1U);
        } else {
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            out.push_back(ubfx_w(9,9,31U-bi,1U));
            load_imm(out,10,(bo&0x08U)!=0U?1U:0U);
            out.push_back(subs_w(14,9,10));
            out.push_back(cset_w(13,0U)); // CR[BI] == BO[1]
        }
        out.push_back(and_w(14,12,13));
        out.push_back(cmp_zero_w(14));
        if((ins&1U)!=0U) {
            load_imm(out,9,guest_pc+4U);
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,lr))));
        }
        load_imm(out,9,target);
        load_imm(out,10,guest_pc+4U);
        out.push_back(csel_w(11,9,10,1U)); // NE: branch target, EQ: fall through
        out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,pc))));
        terminates=true;
        return true;
    }
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



    if(op==23U) { // rlwnm: rotate by GPR[RB] & 31, then mask
        const unsigned mb=(ins>>6U)&31U;
        const unsigned me=(ins>>1U)&31U;
        u32 mask=0U;
        for(unsigned bit=mb;;bit=(bit+1U)&31U) {
            mask|=u32{1}<<(31U-bit);
            if(bit==me) break;
        }
        out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
        out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
        load_imm(out,13,31U);
        out.push_back(and_w(10,10,13));
        out.push_back(sub_w(10,31U,10)); // negative shift = 32 - (RB & 31)
        out.push_back(rorv_w(11,9,10));
        load_imm(out,12,mask);
        out.push_back(and_w(11,11,12));
        out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
        if((ins&1U)!=0U) emit_cr0(out,11);
        return true;
    }
    if(op==20U || op==21U) { // rlwimi / rlwinm (immediate rotate and mask)
        const unsigned shift=(ins>>11U)&31U;
        const unsigned mb=(ins>>6U)&31U;
        const unsigned me=(ins>>1U)&31U;
        u32 mask=0U;
        for(unsigned bit=mb;;bit=(bit+1U)&31U) {
            mask|=u32{1}<<(31U-bit);
            if(bit==me) break;
        }
        out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
        if(shift==0U) out.push_back(orr_w(11,9,9));
        else {
            out.push_back(lsl_w(12,9,shift));
            out.push_back(lsr_w(13,9,32U-shift));
            out.push_back(orr_w(11,12,13));
        }
        load_imm(out,10,mask);
        out.push_back(and_w(11,11,10));
        if(op==20U) {
            out.push_back(ldr_w(12,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            load_imm(out,13,~mask);
            out.push_back(and_w(12,12,13));
            out.push_back(orr_w(11,11,12));
        }
        out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
        if((ins&1U)!=0U) emit_cr0(out,11);
        return true;
    }
    if(op==28U || op==29U) { // andi./andis.
        out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
        load_imm(out,10,op==29U ? (imm<<16U) : imm);
        out.push_back(and_w(11,9,10));
        out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
        emit_cr0(out,11);
        return true;
    }

    if(op==10U || op==11U) { // cmpli / cmpi
        if((ins&(1U<<21U))!=0U) return false;
        const unsigned bf=(ins>>23U)&7U;
        out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
        const u32 immediate=op==11U ? static_cast<u32>(static_cast<s32>(static_cast<s16>(ins&0xFFFFU)))
                                    : (ins&0xFFFFU);
        load_imm(out,10,immediate);
        out.push_back(subs_w(31U,9,10));
        emit_cr_compare(out,bf,op==11U?0xBU:0x3U,op==11U?0xCU:0x8U,0U);
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
        const bool rc = (ins & 1U) != 0U;
        if (xo9 == 138U || xo9 == 136U || xo9 == 202U ||
            xo9 == 234U || xo9 == 200U || xo9 == 232U) {
            // Carry-consuming arithmetic. ADCS computes the exact PPC
            // addend forms; subtraction forms add the one's-complement RA.
            out.push_back(ldr_w(12, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            out.push_back(ubfx_w(12, 12, 29, 1));
            out.push_back(lsl_w(12, 12, 29));
            out.push_back(msr_nzcv_x(12));
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            if (xo9 == 138U || xo9 == 136U) {
                out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
                if (xo9 == 138U) out.push_back(adcs_w(11, 9, 10));
                else {
                    out.push_back(mvn_w(9,9));
                    out.push_back(adcs_w(11,9,10)); // subfe = ~RA + RB + CA
                }
            } else {
                if (xo9 == 200U || xo9 == 232U) out.push_back(mvn_w(9,9));
                if (xo9 == 234U || xo9 == 232U) out.push_back(mvn_w(10,31U));
                else out.push_back(orr_w(10,31U,31U));
                out.push_back(adcs_w(11,9,10)); // addze/addme/subfze/subfme
            }
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            out.push_back(mrs_nzcv_x(16));
            out.push_back(ubfx_w(12, 16, 29, 1));
            out.push_back(lsl_w(12, 12, 29));
            load_imm(out, 13, XER_CA);
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            out.push_back(bic_w(9, 9, 13));
            out.push_back(orr_w(9, 9, 12));
            out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            if (oe) emit_xer_overflow(out,16);
            if (rc) emit_cr0(out,11);
            return true;
        }
        if (xo9 == 266U || xo9 == 10U || xo9 == 40U || xo9 == 8U) {
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            if (xo9 == 266U) out.push_back(oe ? adds_w(11,9,10) : add_w(11,9,10));
            else if (xo9 == 10U) out.push_back(adds_w(11,9,10));
            else if (xo9 == 40U) {
                if (oe) out.push_back(subs_w(11,10,9));
                else {
                    out.push_back(mvn_w(9,9));
                    load_imm(out,12,1U);
                    out.push_back(add_w(11,9,10));
                    out.push_back(add_w(11,11,12));
                }
            } else {
                // subfc RT,RA,RB = RB - RA; SUBS sets C=1 when no borrow.
                out.push_back(subs_w(11, 10, 9));
            }
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            if (xo9 == 10U || xo9 == 8U) {
                out.push_back(mrs_nzcv_x(16));
                out.push_back(ubfx_w(12, 16, 29, 1));
                out.push_back(lsl_w(12, 12, 29));
                load_imm(out, 13, XER_CA);
                out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
                out.push_back(bic_w(9, 9, 13));
                out.push_back(orr_w(9, 9, 12));
                out.push_back(str_w(9, 0, static_cast<unsigned>(offsetof(CpuState, xer))));
            } else if (oe) {
                out.push_back(mrs_nzcv_x(16));
            }
            if (oe) emit_xer_overflow(out,16);
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


    if(op==31U) {
        const unsigned xo10=(ins>>1U)&0x3FFU;
        const unsigned xo9=(ins>>1U)&0x1FFU;

        if(xo10==512U) { // mcrxr BF: move XER[SO:CA] nibble into CR and clear XER
            const unsigned bf=(ins>>23U)&7U;
            const unsigned shift=28U-bf*4U;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
            out.push_back(ubfx_w(11,9,28U,4U));
            if(shift==0U) out.push_back(orr_w(12,11,11));
            else out.push_back(lsl_w(12,11,shift));
            load_imm(out,13,~(0xFU<<shift));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            out.push_back(and_w(10,10,13));
            out.push_back(orr_w(10,10,12));
            out.push_back(str_w(10,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            load_imm(out,13,0x0FFFFFFFU);
            out.push_back(and_w(9,9,13));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
            return true;
        }
        if(xo10==19U) { // mfcr RT
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            return true;
        }
        if(xo10==371U) { // mftb: read the architecturally modelled time-base halves
            const unsigned tbr=(((ins>>11U)&31U)<<5U)|((ins>>16U)&31U);
            if(tbr!=268U && tbr!=269U) return false;
            const unsigned offset=static_cast<unsigned>(offsetof(CpuState,tb)+(tbr==269U?4U:0U));
            out.push_back(ldr_w(9,0,offset));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            return true;
        }
        if(xo10==339U || xo10==467U) { // mfspr/mtspr: directly resident LR/CTR/XER state
            // PowerPC encodes the two five-bit SPR halves in reversed order.
            const unsigned spr_number=(((ins>>11U)&31U)<<5U)|((ins>>16U)&31U);
            unsigned state_offset=0U;
            if(spr_number==1U) {
                if(xo10==467U) return false; // XER writes require architectural reserved-bit masking.
                state_offset=static_cast<unsigned>(offsetof(CpuState,xer));
            } else if(spr_number==8U) {
                state_offset=static_cast<unsigned>(offsetof(CpuState,lr));
            } else if(spr_number==9U) {
                state_offset=static_cast<unsigned>(offsetof(CpuState,ctr));
            } else return false;
            if(xo10==339U) {
                out.push_back(ldr_w(9,0,state_offset));
                out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            } else {
                out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
                out.push_back(str_w(9,0,state_offset));
            }
            return true;
        }
        if(xo10==144U) { // mtcrf FXM,RS
            const unsigned fxm=(ins>>12U)&0xFFU;
            if(fxm==0U) return true;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            for(unsigned bf=0;bf<8U;++bf) {
                if((fxm&(1U<<(7U-bf)))==0U) continue;
                const unsigned shift=28U-bf*4U;
                out.push_back(ubfx_w(11,9,shift,4U));
                if(shift==0U) out.push_back(orr_w(12,11,11));
                else out.push_back(lsl_w(12,11,shift));
                load_imm(out,13,~(0xFU<<shift));
                out.push_back(and_w(10,10,13));
                out.push_back(orr_w(10,10,12));
            }
            out.push_back(str_w(10,0,static_cast<unsigned>(offsetof(CpuState,cr))));
            return true;
        }
        if((xo10==0U || xo10==32U) && (ins&(1U<<21U))==0U) { // cmp / cmpl
            const unsigned bf=(ins>>23U)&7U;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            out.push_back(subs_w(31U,9,10));
            emit_cr_compare(out,bf,xo10==0U?0xBU:0x3U,xo10==0U?0xCU:0x8U,0U);
            return true;
        }
        if(xo9==104U) { // neg RT,RA
            const bool oe=(ins&0x400U)!=0U;
            const bool record=(ins&1U)!=0U;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            out.push_back(oe?subs_w(11,31U,9):sub_w(11,31U,9));
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            if(oe) { out.push_back(mrs_nzcv_x(16)); emit_xer_overflow(out,16); }
            if(record) emit_cr0(out,11);
            return true;
        }
        if(xo10==459U) { // divwu RT,RA,RB (OE=0)
            if((ins&0x400U)!=0U) return false;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            // AArch64 UDIV returns zero for a zero divisor, matching the
            // interpreter's currently modelled PowerPC result.
            out.push_back(udiv_w(11,9,10));
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            if((ins&1U)!=0U) emit_cr0(out,11);
            return true;
        }
        if(xo10==491U) { // divw RT,RA,RB (OE=0)
            if((ins&0x400U)!=0U) return false;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            out.push_back(sdiv_w(11,9,10));
            // The interpreter models the 750-family undefined cases as
            // sign-fill: divisor zero or INT_MIN / -1.
            out.push_back(cmp_zero_w(10));
            out.push_back(cset_w(12,0U)); // divisor == 0
            load_imm(out,13,0x80000000U);
            out.push_back(subs_w(31U,9,13));
            out.push_back(cset_w(14,0U)); // numerator == INT_MIN
            load_imm(out,15,0xFFFFFFFFU);
            out.push_back(subs_w(31U,10,15));
            out.push_back(cset_w(15,0U)); // divisor == -1
            out.push_back(and_w(14,14,15));
            out.push_back(orr_w(12,12,14));
            out.push_back(asr_w(15,9,31U)); // fallback: -1 for negative, 0 otherwise
            out.push_back(cmp_zero_w(12));
            out.push_back(csel_w(11,11,15,0U)); // normal quotient if no special case
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            if((ins&1U)!=0U) emit_cr0(out,11);
            return true;
        }
        const bool rc=(ins&1U)!=0U;





        if(xo10==75U || xo10==11U) { // mulhw / mulhwu
            if((ins&0x400U)!=0U) return false; // OE/overflow semantics not emitted
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            out.push_back(xo10==75U ? smull_x(12,9,10) : umull_x(12,9,10));
            out.push_back(lsr_x(12,12,32U));
            out.push_back(str_w(12,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            if(rc) emit_cr0(out,12);
            return true;
        }
        if(xo10==235U) { // mullw, OE=0
            if((ins&0x400U)!=0U) return false; // OE must update XER[OV]/[SO]
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            out.push_back(mul_w(11,9,10));
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            if(rc) emit_cr0(out,11);
            return true;
        }
        if(xo10==792U) { // sraw: variable arithmetic shift with XER[CA]
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            load_imm(out,13,31U);
            out.push_back(and_w(11,10,13)); // shift modulo 32
            out.push_back(ubfx_w(12,10,5U,1U)); // shift >= 32
            out.push_back(asrv_w(13,9,11));
            out.push_back(asr_w(14,9,31U)); // sign-fill result for shift >= 32
            out.push_back(cmp_zero_w(12));
            out.push_back(csel_w(13,13,14,0U)); // EQ: <32, NE: >=32
            out.push_back(str_w(13,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));

            // CA = source_negative && any discarded bit; for shift >=32,
            // every source bit is discarded, so CA equals the sign bit.
            load_imm(out,14,1U);
            out.push_back(lslv_w(14,14,11));
            load_imm(out,15,1U);
            out.push_back(sub_w(14,14,15));
            out.push_back(and_w(15,9,14));
            out.push_back(cmp_zero_w(15));
            out.push_back(cset_w(15,1U)); // any low discarded bit
            out.push_back(ubfx_w(14,9,31U,1U)); // source sign
            out.push_back(and_w(15,15,14)); // CA for shift <32
            out.push_back(cmp_zero_w(12));
            out.push_back(csel_w(15,14,15,1U)); // NE: shift >=32 -> sign bit
            out.push_back(lsl_w(15,15,29U));
            load_imm(out,14,XER_CA);
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
            out.push_back(bic_w(9,9,14));
            out.push_back(orr_w(9,9,15));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
            if(rc) emit_cr0(out,13);
            return true;
        }
        if(xo10==24U || xo10==536U) { // slw / srw
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            out.push_back(xo10==24U ? lslv_w(11,9,10) : lsrv_w(11,9,10));
            out.push_back(ubfx_w(12,10,5U,1U)); // PPC treats shifts >=32 as zero
            out.push_back(cmp_zero_w(12));
            load_imm(out,13,0U);
            out.push_back(csel_w(11,11,13,0U)); // EQ: variable shift, NE: zero
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            if(rc) emit_cr0(out,11);
            return true;
        }
        if(xo10==824U) { // srawi: arithmetic shift with XER[CA] update
            const unsigned shift=rb&31U;
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            out.push_back(asr_w(11,9,shift));
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            if(shift==0U) {
                load_imm(out,13,0U);
            } else {
                const u32 discarded_mask=(u32{1}<<shift)-1U;
                load_imm(out,10,discarded_mask);
                out.push_back(and_w(12,9,10));
                out.push_back(cmp_zero_w(12));
                out.push_back(cset_w(13,1U)); // any discarded bit
                out.push_back(ubfx_w(14,9,31U,1U)); // source sign
                out.push_back(and_w(13,13,14));
            }
            out.push_back(lsl_w(13,13,29U));
            load_imm(out,14,XER_CA);
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
            out.push_back(bic_w(9,9,14));
            out.push_back(orr_w(9,9,13));
            out.push_back(str_w(9,0,static_cast<unsigned>(offsetof(CpuState,xer))));
            if(rc) emit_cr0(out,11);
            return true;
        }
        if(xo10==954U || xo10==922U || xo10==26U) { // extsb/extsh/cntlzw
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            if(xo10==954U) out.push_back(sxtb_w(11,9));
            else if(xo10==922U) out.push_back(sxth_w(11,9));
            else out.push_back(clz_w(11,9));
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            if(rc) emit_cr0(out,11);
            return true;
        }
        if(xo10==28U || xo10==444U || xo10==316U || xo10==60U ||
           xo10==412U || xo10==476U || xo10==124U || xo10==284U) {
            out.push_back(ldr_w(9,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rt*4U)));
            out.push_back(ldr_w(10,0,static_cast<unsigned>(offsetof(CpuState,gpr)+rb*4U)));
            if(xo10==28U) out.push_back(and_w(11,9,10));
            else if(xo10==444U) out.push_back(orr_w(11,9,10));
            else if(xo10==316U) out.push_back(eor_w(11,9,10));
            else if(xo10==60U) { out.push_back(mvn_w(10,10)); out.push_back(and_w(11,9,10)); }
            else if(xo10==412U) { out.push_back(mvn_w(10,10)); out.push_back(orr_w(11,9,10)); }
            else if(xo10==476U) { out.push_back(and_w(11,9,10)); out.push_back(mvn_w(11,11)); }
            else if(xo10==124U) { out.push_back(orr_w(11,9,10)); out.push_back(mvn_w(11,11)); }
            else { out.push_back(eor_w(11,9,10)); out.push_back(mvn_w(11,11)); }
            out.push_back(str_w(11,0,static_cast<unsigned>(offsetof(CpuState,gpr)+ra*4U)));
            if(rc) emit_cr0(out,11);
            return true;
        }
    }
    if (op == 31U && (ins & 1U) == 0U) {
        const unsigned xo = (ins >> 1U) & 0x3FFU;
        if (xo == 28U || xo == 444U || xo == 316U) { // and/or/xor (Rc=0 only)
            out.push_back(ldr_w(9, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rt * 4U)));
            out.push_back(ldr_w(10, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + rb * 4U)));
            out.push_back(xo == 28U ? and_w(11, 9, 10) : (xo == 444U ? orr_w(11, 9, 10) : eor_w(11, 9, 10)));
            out.push_back(str_w(11, 0, static_cast<unsigned>(offsetof(CpuState, gpr) + ra * 4U)));
            return true;
        }
    }
    return false;
}
} // namespace

Block compile_arm64(std::span<const u32> words, u32 start_pc) {
    Block b; b.start_pc = start_pc; b.end_pc = start_pc;
    // Bound blocks to a single 4 KiB guest code page and terminate at every
    // translated control-flow instruction; unsupported memory ops fall back.
    if ((start_pc & 3U) != 0U) return b;
    bool terminated=false;
    for (u32 ins : words) {
        if ((b.end_pc >> 12U) != (start_pc >> 12U)) break;
        bool instruction_terminates=false;
        if(!translate_one(ins,b.end_pc,b.host_words,instruction_terminates)) break;
        ++b.guest_instructions;
        b.end_pc += 4U;
        if(instruction_terminates) { terminated=true; break; }
    }
    if (b.guest_instructions != 0U) {
        // Entry ABI: x0 initially points to ExecutionContext; load its first
        // member so the existing generated instruction stream sees CpuState*.
        b.host_words.insert(b.host_words.begin(), 0xF9400000U); // ldr x0, [x0]
        if(!terminated) {
            // Load immediate end_pc into w9 then store PC.
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

bool ExecutableBlock::execute(CpuState& state, Cpu* cpu) const noexcept {
#if defined(__aarch64__)
    if (memory_ == nullptr) return false;
    using Fn = void (*)(ExecutionContext*);
    static_assert(sizeof(Fn) == sizeof(memory_));
    Fn fn = nullptr;
    std::memcpy(&fn, &memory_, sizeof(fn));
    ExecutionContext context{&state, cpu};
    fn(&context);
    return true;
#else
    (void)state;
    (void)cpu;
    return false;
#endif
}

bool execute_arm64(const Block& block, CpuState& state, Cpu* cpu) noexcept {
    ExecutableBlock executable(block);
    return executable.execute(state, cpu);
}

} // namespace orca::ppc::jit
