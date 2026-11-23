// Orca Core — AArch64 (ARMv8-A, A64) instruction emitter for the PowerPC JIT.
//
// Source: Arm Architecture Reference Manual for A-profile (instruction encodings).
// Every encoding below is checked by tests/test_jit_emitter.cpp against the LLVM
// assembler (llvm-mc -show-encoding) when it is available. Orca's own code.
//
// The emitter only appends 32-bit words to a buffer; it knows nothing about
// executable memory (see code_buffer.hpp) or PowerPC.
#pragma once
#include "orca/core/bits.hpp"

#include <optional>
#include <vector>

namespace orca::jit {

// General-purpose register number 0-30; 31 is ZR or SP depending on the instruction.
using Reg = u8;
inline constexpr Reg ZR = 31;
inline constexpr Reg SP = 31;
inline constexpr Reg LR = 30;

enum class Cond : u8 { EQ = 0, NE, HS, LO, MI, PL, VS, VC, HI, LS, GE, LT, GT, LE, AL };
constexpr Cond invert(Cond c) noexcept { return static_cast<Cond>(static_cast<u8>(c) ^ 1U); }

enum class Shift : u8 { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };

// Logical-immediate encoding (N:immr:imms) of `value` for a 32- or 64-bit operation,
// or nothing when the value is not a repeating rotated run of ones.
std::optional<u32> encode_logical_imm(u64 value, bool is64) noexcept;

class Emitter {
public:
    // A label is a position in the buffer (index of a 32-bit word).
    using Label = std::size_t;

    std::vector<u32>& words() noexcept { return w_; }
    const std::vector<u32>& words() const noexcept { return w_; }
    std::size_t size() const noexcept { return w_.size(); }
    Label here() const noexcept { return w_.size(); }
    void emit(u32 word) { w_.push_back(word); }

    // --- moves ---------------------------------------------------------------
    void movz(bool x, Reg d, u16 imm, unsigned shift = 0) { emit(0x52800000U | sf(x) | (shift / 16U) << 21 | u32{imm} << 5 | d); }
    void movk(bool x, Reg d, u16 imm, unsigned shift = 0) { emit(0x72800000U | sf(x) | (shift / 16U) << 21 | u32{imm} << 5 | d); }
    void movn(bool x, Reg d, u16 imm, unsigned shift = 0) { emit(0x12800000U | sf(x) | (shift / 16U) << 21 | u32{imm} << 5 | d); }
    void mov_imm(bool x, Reg d, u64 value); // shortest MOVZ/MOVN/ORR-imm + MOVK sequence
    void mov(bool x, Reg d, Reg n) { orr(x, d, ZR, n); } // not for SP

    // --- arithmetic ----------------------------------------------------------
    void add_imm(bool x, Reg d, Reg n, u32 imm12, bool lsl12 = false) { arith_imm(0x11000000U, x, d, n, imm12, lsl12); }
    void adds_imm(bool x, Reg d, Reg n, u32 imm12, bool lsl12 = false) { arith_imm(0x31000000U, x, d, n, imm12, lsl12); }
    void sub_imm(bool x, Reg d, Reg n, u32 imm12, bool lsl12 = false) { arith_imm(0x51000000U, x, d, n, imm12, lsl12); }
    void subs_imm(bool x, Reg d, Reg n, u32 imm12, bool lsl12 = false) { arith_imm(0x71000000U, x, d, n, imm12, lsl12); }
    void cmp_imm(bool x, Reg n, u32 imm12) { subs_imm(x, ZR, n, imm12); }
    void add(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x0B000000U, x, d, n, m, s, amount); }
    void adds(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x2B000000U, x, d, n, m, s, amount); }
    void sub(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x4B000000U, x, d, n, m, s, amount); }
    void subs(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x6B000000U, x, d, n, m, s, amount); }
    void cmp(bool x, Reg n, Reg m) { subs(x, ZR, n, m); }
    void neg(bool x, Reg d, Reg m) { sub(x, d, ZR, m); }
    void adc(bool x, Reg d, Reg n, Reg m) { emit(0x1A000000U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void adcs(bool x, Reg d, Reg n, Reg m) { emit(0x3A000000U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void sbc(bool x, Reg d, Reg n, Reg m) { emit(0x5A000000U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void sbcs(bool x, Reg d, Reg n, Reg m) { emit(0x7A000000U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void madd(bool x, Reg d, Reg n, Reg m, Reg a) { emit(0x1B000000U | sf(x) | u32{m} << 16 | u32{a} << 10 | u32{n} << 5 | d); }
    void msub(bool x, Reg d, Reg n, Reg m, Reg a) { emit(0x1B008000U | sf(x) | u32{m} << 16 | u32{a} << 10 | u32{n} << 5 | d); }
    void mul(bool x, Reg d, Reg n, Reg m) { madd(x, d, n, m, ZR); }
    void smull(Reg d, Reg n, Reg m) { emit(0x9B200000U | u32{m} << 16 | u32{ZR} << 10 | u32{n} << 5 | d); }
    void umull(Reg d, Reg n, Reg m) { emit(0x9BA00000U | u32{m} << 16 | u32{ZR} << 10 | u32{n} << 5 | d); }
    void sdiv(bool x, Reg d, Reg n, Reg m) { emit(0x1AC00C00U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void udiv(bool x, Reg d, Reg n, Reg m) { emit(0x1AC00800U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }

    // --- logical -------------------------------------------------------------
    void and_(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x0A000000U, x, d, n, m, s, amount); }
    void bic(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x0A200000U, x, d, n, m, s, amount); }
    void orr(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x2A000000U, x, d, n, m, s, amount); }
    void orn(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x2A200000U, x, d, n, m, s, amount); }
    void eor(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x4A000000U, x, d, n, m, s, amount); }
    void eon(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x4A200000U, x, d, n, m, s, amount); }
    void ands(bool x, Reg d, Reg n, Reg m, Shift s = Shift::LSL, unsigned amount = 0) { shifted(0x6A000000U, x, d, n, m, s, amount); }
    void tst(bool x, Reg n, Reg m) { ands(x, ZR, n, m); }
    void mvn(bool x, Reg d, Reg m) { orn(x, d, ZR, m); }
    // Immediate forms return false (and emit nothing) when the value is not encodable.
    bool and_imm(bool x, Reg d, Reg n, u64 imm) { return logical_imm(0x12000000U, x, d, n, imm); }
    bool orr_imm(bool x, Reg d, Reg n, u64 imm) { return logical_imm(0x32000000U, x, d, n, imm); }
    bool eor_imm(bool x, Reg d, Reg n, u64 imm) { return logical_imm(0x52000000U, x, d, n, imm); }
    bool ands_imm(bool x, Reg d, Reg n, u64 imm) { return logical_imm(0x72000000U, x, d, n, imm); }

    // --- shifts, bit fields --------------------------------------------------
    void lslv(bool x, Reg d, Reg n, Reg m) { emit(0x1AC02000U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void lsrv(bool x, Reg d, Reg n, Reg m) { emit(0x1AC02400U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void asrv(bool x, Reg d, Reg n, Reg m) { emit(0x1AC02800U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void rorv(bool x, Reg d, Reg n, Reg m) { emit(0x1AC02C00U | sf(x) | u32{m} << 16 | u32{n} << 5 | d); }
    void sbfm(bool x, Reg d, Reg n, unsigned immr, unsigned imms) { bitfield(0x13000000U, x, d, n, immr, imms); }
    void bfm(bool x, Reg d, Reg n, unsigned immr, unsigned imms) { bitfield(0x33000000U, x, d, n, immr, imms); }
    void ubfm(bool x, Reg d, Reg n, unsigned immr, unsigned imms) { bitfield(0x53000000U, x, d, n, immr, imms); }
    void lsl_imm(bool x, Reg d, Reg n, unsigned s) { const unsigned w = x ? 64U : 32U; ubfm(x, d, n, (w - s) % w, w - 1U - s); }
    void lsr_imm(bool x, Reg d, Reg n, unsigned s) { ubfm(x, d, n, s, x ? 63U : 31U); }
    void asr_imm(bool x, Reg d, Reg n, unsigned s) { sbfm(x, d, n, s, x ? 63U : 31U); }
    void ubfx(bool x, Reg d, Reg n, unsigned lsb, unsigned width) { ubfm(x, d, n, lsb, lsb + width - 1U); }
    void sxtb(bool x, Reg d, Reg n) { sbfm(x, d, n, 0, 7); }
    void sxth(bool x, Reg d, Reg n) { sbfm(x, d, n, 0, 15); }
    void sxtw(Reg d, Reg n) { sbfm(true, d, n, 0, 31); }
    void uxtb(Reg d, Reg n) { ubfm(false, d, n, 0, 7); }
    void uxth(Reg d, Reg n) { ubfm(false, d, n, 0, 15); }
    void bfi(bool x, Reg d, Reg n, unsigned lsb, unsigned width) { const unsigned w = x ? 64U : 32U; bfm(x, d, n, (w - lsb) % w, width - 1U); }
    void bfxil(bool x, Reg d, Reg n, unsigned lsb, unsigned width) { bfm(x, d, n, lsb, lsb + width - 1U); }
    void extr(bool x, Reg d, Reg n, Reg m, unsigned lsb) { emit(0x13800000U | sf(x) | (x ? 1U << 22 : 0U) | u32{m} << 16 | (lsb & 63U) << 10 | u32{n} << 5 | d); }
    void ror_imm(bool x, Reg d, Reg n, unsigned s) { extr(x, d, n, n, s); }
    void clz(bool x, Reg d, Reg n) { emit(0x5AC01000U | sf(x) | u32{n} << 5 | d); }
    void rev(bool x, Reg d, Reg n) { emit((x ? 0xDAC00C00U : 0x5AC00800U) | u32{n} << 5 | d); }
    void rev16(bool x, Reg d, Reg n) { emit(0x5AC00400U | sf(x) | u32{n} << 5 | d); }

    // --- conditional select --------------------------------------------------
    void csel(bool x, Reg d, Reg n, Reg m, Cond c) { emit(0x1A800000U | sf(x) | u32{m} << 16 | u32{static_cast<u8>(c)} << 12 | u32{n} << 5 | d); }
    void csinc(bool x, Reg d, Reg n, Reg m, Cond c) { emit(0x1A800400U | sf(x) | u32{m} << 16 | u32{static_cast<u8>(c)} << 12 | u32{n} << 5 | d); }
    void csinv(bool x, Reg d, Reg n, Reg m, Cond c) { emit(0x5A800000U | sf(x) | u32{m} << 16 | u32{static_cast<u8>(c)} << 12 | u32{n} << 5 | d); }
    void cset(bool x, Reg d, Cond c) { csinc(x, d, ZR, ZR, invert(c)); }
    void csetm(bool x, Reg d, Cond c) { csinv(x, d, ZR, ZR, invert(c)); }

    // --- loads / stores (unsigned scaled offset or register offset) ----------
    // size: 1, 2, 4 or 8 bytes; imm is a byte offset, a multiple of size, < 4096 * size.
    void ldr(unsigned size, Reg t, Reg n, u32 imm = 0) { ls_imm(size, 0x39400000U, t, n, imm); }
    void str(unsigned size, Reg t, Reg n, u32 imm = 0) { ls_imm(size, 0x39000000U, t, n, imm); }
    void ldrsb_w(Reg t, Reg n, u32 imm = 0) { ls_imm(1, 0x39C00000U, t, n, imm); }
    void ldrsh_w(Reg t, Reg n, u32 imm = 0) { ls_imm(2, 0x39C00000U, t, n, imm); }
    void ldr_reg(unsigned size, Reg t, Reg n, Reg m) { ls_reg(size, 0x38606800U, t, n, m); }
    void str_reg(unsigned size, Reg t, Reg n, Reg m) { ls_reg(size, 0x38206800U, t, n, m); }
    // 64-bit pairs: pre-index (writeback), post-index, signed offset
    void stp_pre(Reg t1, Reg t2, Reg n, s32 imm) { pair(0xA9800000U, t1, t2, n, imm); }
    void ldp_post(Reg t1, Reg t2, Reg n, s32 imm) { pair(0xA8C00000U, t1, t2, n, imm); }
    void stp(Reg t1, Reg t2, Reg n, s32 imm) { pair(0xA9000000U, t1, t2, n, imm); }
    void ldp(Reg t1, Reg t2, Reg n, s32 imm) { pair(0xA9400000U, t1, t2, n, imm); }

    // --- scalar floating point (double unless noted; V = SIMD/FP register 0-31) --
    using V = u8;
    void ldr_d(V t, Reg n, u32 imm = 0) { emit(0xFD400000U | (imm / 8U) << 10 | u32{n} << 5 | t); } // imm: multiple of 8, < 32768
    void str_d(V t, Reg n, u32 imm = 0) { emit(0xFD000000U | (imm / 8U) << 10 | u32{n} << 5 | t); }
    void fadd(V d, V n, V m) { emit(0x1E602800U | u32{m} << 16 | u32{n} << 5 | d); }
    void fsub(V d, V n, V m) { emit(0x1E603800U | u32{m} << 16 | u32{n} << 5 | d); }
    void fmul(V d, V n, V m) { emit(0x1E600800U | u32{m} << 16 | u32{n} << 5 | d); }
    void fdiv(V d, V n, V m) { emit(0x1E601800U | u32{m} << 16 | u32{n} << 5 | d); }
    void fmadd(V d, V n, V m, V a) { emit(0x1F400000U | u32{m} << 16 | u32{a} << 10 | u32{n} << 5 | d); }  // a + n*m
    void fmsub(V d, V n, V m, V a) { emit(0x1F408000U | u32{m} << 16 | u32{a} << 10 | u32{n} << 5 | d); }  // a - n*m
    void fnmsub(V d, V n, V m, V a) { emit(0x1F608000U | u32{m} << 16 | u32{a} << 10 | u32{n} << 5 | d); } // n*m - a
    void fneg(V d, V n) { emit(0x1E614000U | u32{n} << 5 | d); }
    void fmov_d(V d, V n) { emit(0x1E604000U | u32{n} << 5 | d); }
    void fcvt_s_from_d(V d, V n) { emit(0x1E624000U | u32{n} << 5 | d); }
    void fcvt_d_from_s(V d, V n) { emit(0x1E22C000U | u32{n} << 5 | d); }
    void fmov_to_x(Reg d, V n) { emit(0x9E660000U | u32{n} << 5 | d); }
    void fmov_from_x(V d, Reg n) { emit(0x9E670000U | u32{n} << 5 | d); }
    void fcmp(V n, V m) { emit(0x1E602000U | u32{m} << 16 | u32{n} << 5); }
    void fcmp_zero(V n) { emit(0x1E602008U | u32{n} << 5); }
    void fcsel(V d, V n, V m, Cond c) { emit(0x1E600C00U | u32{m} << 16 | u32{static_cast<u8>(c)} << 12 | u32{n} << 5 | d); }

    // --- branches ------------------------------------------------------------
    // Forward branches are emitted with a zero offset and patched with bind().
    Label b() { emit(0x14000000U); return here() - 1U; }
    Label b_cond(Cond c) { emit(0x54000000U | static_cast<u8>(c)); return here() - 1U; }
    Label cbz(bool x, Reg t) { emit(0x34000000U | sf(x) | t); return here() - 1U; }
    Label cbnz(bool x, Reg t) { emit(0x35000000U | sf(x) | t); return here() - 1U; }
    Label tbz(Reg t, unsigned bit) { emit(0x36000000U | ((bit >> 5) & 1U) << 31 | (bit & 31U) << 19 | t); return here() - 1U; }
    Label tbnz(Reg t, unsigned bit) { emit(0x37000000U | ((bit >> 5) & 1U) << 31 | (bit & 31U) << 19 | t); return here() - 1U; }
    // Point the branch at `from` to `to` (both word indices). False if out of range.
    bool bind(Label from, Label to);
    bool bind_here(Label from) { return bind(from, here()); }
    void b_to(Label to) { const Label l = b(); bind(l, to); }
    // ADR Xd, (this instruction + byte_offset), |byte_offset| < 1 MiB
    void adr(Reg d, s32 byte_offset) {
        const u32 imm = static_cast<u32>(byte_offset) & 0x1FFFFFU;
        emit(0x10000000U | (imm & 3U) << 29 | (imm >> 2) << 5 | d);
    }
    // B to `to_word` from the word `from_word` (both indexes in some buffer): the encoding
    static constexpr u32 b_encoding(s64 word_delta) noexcept { return 0x14000000U | (static_cast<u32>(word_delta) & 0x03FFFFFFU); }
    static constexpr u32 movz_encoding(bool x, Reg d, u16 imm) noexcept { return 0x52800000U | (x ? 0x80000000U : 0U) | u32{imm} << 5 | d; }
    void br(Reg n) { emit(0xD61F0000U | u32{n} << 5); }
    void blr(Reg n) { emit(0xD63F0000U | u32{n} << 5); }
    void ret() { emit(0xD65F03C0U); }
    void nop() { emit(0xD503201FU); }

private:
    static constexpr u32 sf(bool x) noexcept { return x ? 0x80000000U : 0U; }
    void arith_imm(u32 op, bool x, Reg d, Reg n, u32 imm12, bool lsl12) {
        emit(op | sf(x) | (lsl12 ? 1U << 22 : 0U) | (imm12 & 0xFFFU) << 10 | u32{n} << 5 | d);
    }
    void shifted(u32 op, bool x, Reg d, Reg n, Reg m, Shift s, unsigned amount) {
        emit(op | sf(x) | u32{static_cast<u8>(s)} << 22 | u32{m} << 16 | (amount & 63U) << 10 | u32{n} << 5 | d);
    }
    void bitfield(u32 op, bool x, Reg d, Reg n, unsigned immr, unsigned imms) {
        emit(op | sf(x) | (x ? 1U << 22 : 0U) | (immr & 63U) << 16 | (imms & 63U) << 10 | u32{n} << 5 | d);
    }
    bool logical_imm(u32 op, bool x, Reg d, Reg n, u64 imm);
    void ls_imm(unsigned size, u32 base, Reg t, Reg n, u32 imm);
    void ls_reg(unsigned size, u32 base, Reg t, Reg n, Reg m);
    void pair(u32 op, Reg t1, Reg t2, Reg n, s32 imm) {
        emit(op | (static_cast<u32>(imm / 8) & 0x7FU) << 15 | u32{t2} << 10 | u32{n} << 5 | t1);
    }
    std::vector<u32> w_;
};

} // namespace orca::jit
