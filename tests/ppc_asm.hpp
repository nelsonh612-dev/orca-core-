// Tiny PowerPC encoder used by the tests. Every encoder here is verified
// against LLVM's PowerPC assembler by tests/encoding_check.cpp.
#pragma once
#include <cstdint>

namespace orca::test::ppc_asm {

using u32 = std::uint32_t;

constexpr u32 D(u32 op, u32 rt, u32 ra, u32 imm) { return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFFu); }
constexpr u32 X(u32 xo, u32 rt, u32 ra, u32 rb, bool rc = false) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | (rc ? 1u : 0u);
}
constexpr u32 XO(u32 xo, u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) {
    return (31u << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (oe ? 0x400u : 0u) | (xo << 1) | (rc ? 1u : 0u);
}
constexpr u32 XL(u32 xo, u32 bt, u32 ba, u32 bb, bool lk = false) {
    return (19u << 26) | (bt << 21) | (ba << 16) | (bb << 11) | (xo << 1) | (lk ? 1u : 0u);
}
constexpr u32 M(u32 op, u32 rs, u32 ra, u32 sh, u32 mb, u32 me, bool rc = false) {
    return (op << 26) | (rs << 21) | (ra << 16) | (sh << 11) | (mb << 6) | (me << 1) | (rc ? 1u : 0u);
}
constexpr u32 spr_field(u32 n) { return ((n & 31u) << 16) | (((n >> 5) & 31u) << 11); }

// D-form arithmetic / logical
constexpr u32 addi(u32 rt, u32 ra, int imm) { return D(14, rt, ra, static_cast<u32>(imm)); }
constexpr u32 addis(u32 rt, u32 ra, int imm) { return D(15, rt, ra, static_cast<u32>(imm)); }
constexpr u32 addic(u32 rt, u32 ra, int imm) { return D(12, rt, ra, static_cast<u32>(imm)); }
constexpr u32 addic_(u32 rt, u32 ra, int imm) { return D(13, rt, ra, static_cast<u32>(imm)); }
constexpr u32 subfic(u32 rt, u32 ra, int imm) { return D(8, rt, ra, static_cast<u32>(imm)); }
constexpr u32 mulli(u32 rt, u32 ra, int imm) { return D(7, rt, ra, static_cast<u32>(imm)); }
constexpr u32 ori(u32 ra, u32 rs, u32 imm) { return D(24, rs, ra, imm); }
constexpr u32 oris(u32 ra, u32 rs, u32 imm) { return D(25, rs, ra, imm); }
constexpr u32 xori(u32 ra, u32 rs, u32 imm) { return D(26, rs, ra, imm); }
constexpr u32 xoris(u32 ra, u32 rs, u32 imm) { return D(27, rs, ra, imm); }
constexpr u32 andi_(u32 ra, u32 rs, u32 imm) { return D(28, rs, ra, imm); }
constexpr u32 andis_(u32 ra, u32 rs, u32 imm) { return D(29, rs, ra, imm); }
constexpr u32 cmpwi(u32 bf, u32 ra, int imm) { return D(11, bf << 2, ra, static_cast<u32>(imm)); }
constexpr u32 cmplwi(u32 bf, u32 ra, u32 imm) { return D(10, bf << 2, ra, imm); }
constexpr u32 twi(u32 to, u32 ra, int imm) { return D(3, to, ra, static_cast<u32>(imm)); }

// XO-form
constexpr u32 add(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(266, rt, ra, rb, oe, rc); }
constexpr u32 addc(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(10, rt, ra, rb, oe, rc); }
constexpr u32 adde(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(138, rt, ra, rb, oe, rc); }
constexpr u32 addze(u32 rt, u32 ra, bool oe = false, bool rc = false) { return XO(202, rt, ra, 0, oe, rc); }
constexpr u32 addme(u32 rt, u32 ra, bool oe = false, bool rc = false) { return XO(234, rt, ra, 0, oe, rc); }
constexpr u32 subf(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(40, rt, ra, rb, oe, rc); }
constexpr u32 subfc(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(8, rt, ra, rb, oe, rc); }
constexpr u32 subfe(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(136, rt, ra, rb, oe, rc); }
constexpr u32 subfze(u32 rt, u32 ra, bool oe = false, bool rc = false) { return XO(200, rt, ra, 0, oe, rc); }
constexpr u32 subfme(u32 rt, u32 ra, bool oe = false, bool rc = false) { return XO(232, rt, ra, 0, oe, rc); }
constexpr u32 neg(u32 rt, u32 ra, bool oe = false, bool rc = false) { return XO(104, rt, ra, 0, oe, rc); }
constexpr u32 mullw(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(235, rt, ra, rb, oe, rc); }
constexpr u32 mulhw(u32 rt, u32 ra, u32 rb, bool rc = false) { return XO(75, rt, ra, rb, false, rc); }
constexpr u32 mulhwu(u32 rt, u32 ra, u32 rb, bool rc = false) { return XO(11, rt, ra, rb, false, rc); }
constexpr u32 divw(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(491, rt, ra, rb, oe, rc); }
constexpr u32 divwu(u32 rt, u32 ra, u32 rb, bool oe = false, bool rc = false) { return XO(459, rt, ra, rb, oe, rc); }

// X-form logical / shifts / compares (note: RS is the first register field)
constexpr u32 and_(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(28, rs, ra, rb, rc); }
constexpr u32 andc(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(60, rs, ra, rb, rc); }
constexpr u32 or_(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(444, rs, ra, rb, rc); }
constexpr u32 orc(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(412, rs, ra, rb, rc); }
constexpr u32 xor_(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(316, rs, ra, rb, rc); }
constexpr u32 nand(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(476, rs, ra, rb, rc); }
constexpr u32 nor(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(124, rs, ra, rb, rc); }
constexpr u32 eqv(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(284, rs, ra, rb, rc); }
constexpr u32 slw(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(24, rs, ra, rb, rc); }
constexpr u32 srw(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(536, rs, ra, rb, rc); }
constexpr u32 sraw(u32 ra, u32 rs, u32 rb, bool rc = false) { return X(792, rs, ra, rb, rc); }
constexpr u32 srawi(u32 ra, u32 rs, u32 sh, bool rc = false) { return X(824, rs, ra, sh, rc); }
constexpr u32 cntlzw(u32 ra, u32 rs, bool rc = false) { return X(26, rs, ra, 0, rc); }
constexpr u32 extsb(u32 ra, u32 rs, bool rc = false) { return X(954, rs, ra, 0, rc); }
constexpr u32 extsh(u32 ra, u32 rs, bool rc = false) { return X(922, rs, ra, 0, rc); }
constexpr u32 cmpw(u32 bf, u32 ra, u32 rb) { return X(0, bf << 2, ra, rb); }
constexpr u32 cmplw(u32 bf, u32 ra, u32 rb) { return X(32, bf << 2, ra, rb); }
constexpr u32 tw(u32 to, u32 ra, u32 rb) { return X(4, to, ra, rb); }

// Rotates
constexpr u32 rlwinm(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, bool rc = false) { return M(21, rs, ra, sh, mb, me, rc); }
constexpr u32 rlwimi(u32 ra, u32 rs, u32 sh, u32 mb, u32 me, bool rc = false) { return M(20, rs, ra, sh, mb, me, rc); }
constexpr u32 rlwnm(u32 ra, u32 rs, u32 rb, u32 mb, u32 me, bool rc = false) { return M(23, rs, ra, rb, mb, me, rc); }

// Loads / stores (D-form: rt, displacement, ra)
constexpr u32 lwz(u32 rt, int d, u32 ra) { return D(32, rt, ra, static_cast<u32>(d)); }
constexpr u32 lwzu(u32 rt, int d, u32 ra) { return D(33, rt, ra, static_cast<u32>(d)); }
constexpr u32 lbz(u32 rt, int d, u32 ra) { return D(34, rt, ra, static_cast<u32>(d)); }
constexpr u32 lbzu(u32 rt, int d, u32 ra) { return D(35, rt, ra, static_cast<u32>(d)); }
constexpr u32 stw(u32 rs, int d, u32 ra) { return D(36, rs, ra, static_cast<u32>(d)); }
constexpr u32 stwu(u32 rs, int d, u32 ra) { return D(37, rs, ra, static_cast<u32>(d)); }
constexpr u32 stb(u32 rs, int d, u32 ra) { return D(38, rs, ra, static_cast<u32>(d)); }
constexpr u32 stbu(u32 rs, int d, u32 ra) { return D(39, rs, ra, static_cast<u32>(d)); }
constexpr u32 lhz(u32 rt, int d, u32 ra) { return D(40, rt, ra, static_cast<u32>(d)); }
constexpr u32 lhzu(u32 rt, int d, u32 ra) { return D(41, rt, ra, static_cast<u32>(d)); }
constexpr u32 lha(u32 rt, int d, u32 ra) { return D(42, rt, ra, static_cast<u32>(d)); }
constexpr u32 lhau(u32 rt, int d, u32 ra) { return D(43, rt, ra, static_cast<u32>(d)); }
constexpr u32 sth(u32 rs, int d, u32 ra) { return D(44, rs, ra, static_cast<u32>(d)); }
constexpr u32 sthu(u32 rs, int d, u32 ra) { return D(45, rs, ra, static_cast<u32>(d)); }
constexpr u32 lmw(u32 rt, int d, u32 ra) { return D(46, rt, ra, static_cast<u32>(d)); }
constexpr u32 stmw(u32 rs, int d, u32 ra) { return D(47, rs, ra, static_cast<u32>(d)); }
constexpr u32 lfs(u32 frt, int d, u32 ra) { return D(48, frt, ra, static_cast<u32>(d)); }

// Loads / stores (X-form: rt, ra, rb)
constexpr u32 lwzx(u32 rt, u32 ra, u32 rb) { return X(23, rt, ra, rb); }
constexpr u32 lwzux(u32 rt, u32 ra, u32 rb) { return X(55, rt, ra, rb); }
constexpr u32 lbzx(u32 rt, u32 ra, u32 rb) { return X(87, rt, ra, rb); }
constexpr u32 lhax(u32 rt, u32 ra, u32 rb) { return X(343, rt, ra, rb); }
constexpr u32 stwx(u32 rs, u32 ra, u32 rb) { return X(151, rs, ra, rb); }
constexpr u32 stbux(u32 rs, u32 ra, u32 rb) { return X(247, rs, ra, rb); }
constexpr u32 lwbrx(u32 rt, u32 ra, u32 rb) { return X(534, rt, ra, rb); }
constexpr u32 lhbrx(u32 rt, u32 ra, u32 rb) { return X(790, rt, ra, rb); }
constexpr u32 stwbrx(u32 rs, u32 ra, u32 rb) { return X(662, rs, ra, rb); }
constexpr u32 sthbrx(u32 rs, u32 ra, u32 rb) { return X(918, rs, ra, rb); }
constexpr u32 lwarx(u32 rt, u32 ra, u32 rb) { return X(20, rt, ra, rb); }
constexpr u32 stwcx_(u32 rs, u32 ra, u32 rb) { return X(150, rs, ra, rb, true); }
constexpr u32 lswi(u32 rt, u32 ra, u32 nb) { return X(597, rt, ra, nb); }
constexpr u32 stswi(u32 rs, u32 ra, u32 nb) { return X(725, rs, ra, nb); }
constexpr u32 lswx(u32 rt, u32 ra, u32 rb) { return X(533, rt, ra, rb); }
constexpr u32 stswx(u32 rs, u32 ra, u32 rb) { return X(661, rs, ra, rb); }

// Branches
constexpr u32 b(int disp, bool aa = false, bool lk = false) {
    return (18u << 26) | (static_cast<u32>(disp) & 0x03FFFFFCu) | (aa ? 2u : 0u) | (lk ? 1u : 0u);
}
constexpr u32 bc(u32 bo, u32 bi, int disp, bool aa = false, bool lk = false) {
    return (16u << 26) | (bo << 21) | (bi << 16) | (static_cast<u32>(disp) & 0xFFFCu) | (aa ? 2u : 0u) | (lk ? 1u : 0u);
}
constexpr u32 bclr(u32 bo, u32 bi, bool lk = false) { return XL(16, bo, bi, 0, lk); }
constexpr u32 bcctr(u32 bo, u32 bi, bool lk = false) { return XL(528, bo, bi, 0, lk); }

// Condition register
constexpr u32 crand(u32 bt, u32 ba, u32 bb) { return XL(257, bt, ba, bb); }
constexpr u32 cror(u32 bt, u32 ba, u32 bb) { return XL(449, bt, ba, bb); }
constexpr u32 crxor(u32 bt, u32 ba, u32 bb) { return XL(193, bt, ba, bb); }
constexpr u32 crnand(u32 bt, u32 ba, u32 bb) { return XL(225, bt, ba, bb); }
constexpr u32 crnor(u32 bt, u32 ba, u32 bb) { return XL(33, bt, ba, bb); }
constexpr u32 creqv(u32 bt, u32 ba, u32 bb) { return XL(289, bt, ba, bb); }
constexpr u32 crandc(u32 bt, u32 ba, u32 bb) { return XL(129, bt, ba, bb); }
constexpr u32 crorc(u32 bt, u32 ba, u32 bb) { return XL(417, bt, ba, bb); }
constexpr u32 mcrf(u32 bf, u32 bfa) { return XL(0, bf << 2, bfa << 2, 0); }
constexpr u32 mfcr(u32 rt) { return X(19, rt, 0, 0); }
constexpr u32 mtcrf(u32 fxm, u32 rs) { return (31u << 26) | (rs << 21) | (fxm << 12) | (144u << 1); }
constexpr u32 mcrxr(u32 bf) { return X(512, bf << 2, 0, 0); }

// System
constexpr u32 mfspr(u32 rt, u32 n) { return (31u << 26) | (rt << 21) | spr_field(n) | (339u << 1); }
constexpr u32 mtspr(u32 n, u32 rs) { return (31u << 26) | (rs << 21) | spr_field(n) | (467u << 1); }
constexpr u32 mftb(u32 rt, u32 n = 268) { return (31u << 26) | (rt << 21) | spr_field(n) | (371u << 1); }
constexpr u32 mfmsr(u32 rt) { return X(83, rt, 0, 0); }
constexpr u32 mtmsr(u32 rs) { return X(146, rs, 0, 0); }
constexpr u32 mtsr(u32 sr, u32 rs) { return X(210, rs, sr, 0); }
constexpr u32 mfsr(u32 rt, u32 sr) { return X(595, rt, sr, 0); }
constexpr u32 mtsrin(u32 rs, u32 rb) { return X(242, rs, 0, rb); }
constexpr u32 mfsrin(u32 rt, u32 rb) { return X(659, rt, 0, rb); }
constexpr u32 sc() { return 0x44000002u; }
constexpr u32 rfi() { return XL(50, 0, 0, 0); }
constexpr u32 isync() { return XL(150, 0, 0, 0); }
constexpr u32 sync() { return X(598, 0, 0, 0); }
constexpr u32 eieio() { return X(854, 0, 0, 0); }
constexpr u32 tlbie(u32 rb) { return X(306, 0, 0, rb); }
constexpr u32 dcbz(u32 ra, u32 rb) { return X(1014, 0, ra, rb); }
constexpr u32 dcbf(u32 ra, u32 rb) { return X(86, 0, ra, rb); }
constexpr u32 dcbst(u32 ra, u32 rb) { return X(54, 0, ra, rb); }
constexpr u32 dcbt(u32 ra, u32 rb) { return X(278, 0, ra, rb); }
constexpr u32 dcbi(u32 ra, u32 rb) { return X(470, 0, ra, rb); }
constexpr u32 icbi(u32 ra, u32 rb) { return X(982, 0, ra, rb); }

// Floating point (A-form operand order follows the assembler: frD, frA, frC, frB)
constexpr u32 FA(u32 op, u32 d, u32 a, u32 b, u32 c, u32 xo, bool rc = false) {
    return (op << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (xo << 1) | (rc ? 1u : 0u);
}
constexpr u32 FX63(u32 xo, u32 d, u32 a, u32 b, bool rc = false) { return X(xo, d, a, b, rc) & ~(63u << 26) | (63u << 26); }
constexpr u32 fadd(u32 d, u32 a, u32 b, bool rc = false) { return FA(63, d, a, b, 0, 21, rc); }
constexpr u32 fadds(u32 d, u32 a, u32 b, bool rc = false) { return FA(59, d, a, b, 0, 21, rc); }
constexpr u32 fsub(u32 d, u32 a, u32 b, bool rc = false) { return FA(63, d, a, b, 0, 20, rc); }
constexpr u32 fsubs(u32 d, u32 a, u32 b, bool rc = false) { return FA(59, d, a, b, 0, 20, rc); }
constexpr u32 fmul(u32 d, u32 a, u32 c, bool rc = false) { return FA(63, d, a, 0, c, 25, rc); }
constexpr u32 fmuls(u32 d, u32 a, u32 c, bool rc = false) { return FA(59, d, a, 0, c, 25, rc); }
constexpr u32 fdiv(u32 d, u32 a, u32 b, bool rc = false) { return FA(63, d, a, b, 0, 18, rc); }
constexpr u32 fdivs(u32 d, u32 a, u32 b, bool rc = false) { return FA(59, d, a, b, 0, 18, rc); }
constexpr u32 fmadd(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(63, d, a, b, c, 29, rc); }
constexpr u32 fmadds(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(59, d, a, b, c, 29, rc); }
constexpr u32 fmsub(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(63, d, a, b, c, 28, rc); }
constexpr u32 fmsubs(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(59, d, a, b, c, 28, rc); }
constexpr u32 fnmadd(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(63, d, a, b, c, 31, rc); }
constexpr u32 fnmadds(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(59, d, a, b, c, 31, rc); }
constexpr u32 fnmsub(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(63, d, a, b, c, 30, rc); }
constexpr u32 fnmsubs(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(59, d, a, b, c, 30, rc); }
constexpr u32 fsel(u32 d, u32 a, u32 c, u32 b, bool rc = false) { return FA(63, d, a, b, c, 23, rc); }
constexpr u32 fres(u32 d, u32 b, bool rc = false) { return FA(59, d, 0, b, 0, 24, rc); }
constexpr u32 frsqrte(u32 d, u32 b, bool rc = false) { return FA(63, d, 0, b, 0, 26, rc); }
constexpr u32 fsqrt(u32 d, u32 b) { return FA(63, d, 0, b, 0, 22); }
constexpr u32 fcmpu(u32 bf, u32 a, u32 b) { return FX63(0, bf << 2, a, b); }
constexpr u32 fcmpo(u32 bf, u32 a, u32 b) { return FX63(32, bf << 2, a, b); }
constexpr u32 frsp(u32 d, u32 b, bool rc = false) { return FX63(12, d, 0, b, rc); }
constexpr u32 fctiw(u32 d, u32 b, bool rc = false) { return FX63(14, d, 0, b, rc); }
constexpr u32 fctiwz(u32 d, u32 b, bool rc = false) { return FX63(15, d, 0, b, rc); }
constexpr u32 fneg(u32 d, u32 b, bool rc = false) { return FX63(40, d, 0, b, rc); }
constexpr u32 fmr(u32 d, u32 b, bool rc = false) { return FX63(72, d, 0, b, rc); }
constexpr u32 fnabs(u32 d, u32 b, bool rc = false) { return FX63(136, d, 0, b, rc); }
constexpr u32 fabs_(u32 d, u32 b, bool rc = false) { return FX63(264, d, 0, b, rc); }
constexpr u32 mffs(u32 d, bool rc = false) { return FX63(583, d, 0, 0, rc); }
constexpr u32 mtfsf(u32 fm, u32 b, bool rc = false) { return (63u << 26) | (fm << 17) | (b << 11) | (711u << 1) | (rc ? 1u : 0u); }
constexpr u32 mtfsfi(u32 bf, u32 imm, bool rc = false) { return (63u << 26) | (bf << 23) | (imm << 12) | (134u << 1) | (rc ? 1u : 0u); }
constexpr u32 mtfsb0(u32 bt, bool rc = false) { return FX63(70, bt, 0, 0, rc); }
constexpr u32 mtfsb1(u32 bt, bool rc = false) { return FX63(38, bt, 0, 0, rc); }
constexpr u32 mcrfs(u32 bf, u32 bfa) { return (63u << 26) | (bf << 23) | (bfa << 18) | (64u << 1); }
constexpr u32 lfsu(u32 d, int off, u32 a) { return D(49, d, a, static_cast<u32>(off)); }
constexpr u32 lfd(u32 d, int off, u32 a) { return D(50, d, a, static_cast<u32>(off)); }
constexpr u32 lfdu(u32 d, int off, u32 a) { return D(51, d, a, static_cast<u32>(off)); }
constexpr u32 stfs(u32 s, int off, u32 a) { return D(52, s, a, static_cast<u32>(off)); }
constexpr u32 stfsu(u32 s, int off, u32 a) { return D(53, s, a, static_cast<u32>(off)); }
constexpr u32 stfd(u32 s, int off, u32 a) { return D(54, s, a, static_cast<u32>(off)); }
constexpr u32 stfdu(u32 s, int off, u32 a) { return D(55, s, a, static_cast<u32>(off)); }
constexpr u32 lfsx(u32 d, u32 a, u32 b) { return X(535, d, a, b); }
constexpr u32 lfsux(u32 d, u32 a, u32 b) { return X(567, d, a, b); }
constexpr u32 lfdx(u32 d, u32 a, u32 b) { return X(599, d, a, b); }
constexpr u32 lfdux(u32 d, u32 a, u32 b) { return X(631, d, a, b); }
constexpr u32 stfsx(u32 s, u32 a, u32 b) { return X(663, s, a, b); }
constexpr u32 stfsux(u32 s, u32 a, u32 b) { return X(695, s, a, b); }
constexpr u32 stfdx(u32 s, u32 a, u32 b) { return X(727, s, a, b); }
constexpr u32 stfdux(u32 s, u32 a, u32 b) { return X(759, s, a, b); }
constexpr u32 stfiwx(u32 s, u32 a, u32 b) { return X(983, s, a, b); }

// Gekko paired singles (opcode 4) and quantized loads/stores. Not known to
// LLVM; encodings follow the 750CL User's Manual opcode tables.
constexpr u32 psA(u32 d, u32 a, u32 b, u32 c, u32 xo, bool rc = false) { return FA(4, d, a, b, c, xo, rc); }
constexpr u32 psX(u32 xo, u32 d, u32 a, u32 b, bool rc = false) { return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1) | (rc ? 1u : 0u); }
constexpr u32 ps_add(u32 d, u32 a, u32 b, bool rc = false) { return psA(d, a, b, 0, 21, rc); }
constexpr u32 ps_sub(u32 d, u32 a, u32 b) { return psA(d, a, b, 0, 20); }
constexpr u32 ps_mul(u32 d, u32 a, u32 c) { return psA(d, a, 0, c, 25); }
constexpr u32 ps_div(u32 d, u32 a, u32 b) { return psA(d, a, b, 0, 18); }
constexpr u32 ps_madd(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 29); }
constexpr u32 ps_msub(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 28); }
constexpr u32 ps_nmadd(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 31); }
constexpr u32 ps_nmsub(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 30); }
constexpr u32 ps_sel(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 23); }
constexpr u32 ps_res(u32 d, u32 b) { return psA(d, 0, b, 0, 24); }
constexpr u32 ps_rsqrte(u32 d, u32 b) { return psA(d, 0, b, 0, 26); }
constexpr u32 ps_sum0(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 10); }
constexpr u32 ps_sum1(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 11); }
constexpr u32 ps_muls0(u32 d, u32 a, u32 c) { return psA(d, a, 0, c, 12); }
constexpr u32 ps_muls1(u32 d, u32 a, u32 c) { return psA(d, a, 0, c, 13); }
constexpr u32 ps_madds0(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 14); }
constexpr u32 ps_madds1(u32 d, u32 a, u32 c, u32 b) { return psA(d, a, b, c, 15); }
constexpr u32 ps_cmpu0(u32 bf, u32 a, u32 b) { return psX(0, bf << 2, a, b); }
constexpr u32 ps_cmpo0(u32 bf, u32 a, u32 b) { return psX(32, bf << 2, a, b); }
constexpr u32 ps_cmpu1(u32 bf, u32 a, u32 b) { return psX(64, bf << 2, a, b); }
constexpr u32 ps_cmpo1(u32 bf, u32 a, u32 b) { return psX(96, bf << 2, a, b); }
constexpr u32 ps_neg(u32 d, u32 b) { return psX(40, d, 0, b); }
constexpr u32 ps_mr(u32 d, u32 b) { return psX(72, d, 0, b); }
constexpr u32 ps_nabs(u32 d, u32 b) { return psX(136, d, 0, b); }
constexpr u32 ps_abs(u32 d, u32 b) { return psX(264, d, 0, b); }
constexpr u32 ps_merge00(u32 d, u32 a, u32 b) { return psX(528, d, a, b); }
constexpr u32 ps_merge01(u32 d, u32 a, u32 b) { return psX(560, d, a, b); }
constexpr u32 ps_merge10(u32 d, u32 a, u32 b) { return psX(592, d, a, b); }
constexpr u32 ps_merge11(u32 d, u32 a, u32 b) { return psX(624, d, a, b); }
constexpr u32 dcbz_l(u32 a, u32 b) { return psX(1014, 0, a, b); }
constexpr u32 psq_l(u32 d, int off, u32 a, u32 w, u32 i) { return (56u << 26) | (d << 21) | (a << 16) | (w << 15) | (i << 12) | (static_cast<u32>(off) & 0xFFFu); }
constexpr u32 psq_lu(u32 d, int off, u32 a, u32 w, u32 i) { return (57u << 26) | (d << 21) | (a << 16) | (w << 15) | (i << 12) | (static_cast<u32>(off) & 0xFFFu); }
constexpr u32 psq_st(u32 s, int off, u32 a, u32 w, u32 i) { return (60u << 26) | (s << 21) | (a << 16) | (w << 15) | (i << 12) | (static_cast<u32>(off) & 0xFFFu); }
constexpr u32 psq_stu(u32 s, int off, u32 a, u32 w, u32 i) { return (61u << 26) | (s << 21) | (a << 16) | (w << 15) | (i << 12) | (static_cast<u32>(off) & 0xFFFu); }
constexpr u32 psq_lx(u32 d, u32 a, u32 b, u32 w, u32 i) { return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | (w << 10) | (i << 7) | (6u << 1); }
constexpr u32 psq_stx(u32 s, u32 a, u32 b, u32 w, u32 i) { return (4u << 26) | (s << 21) | (a << 16) | (b << 11) | (w << 10) | (i << 7) | (7u << 1); }
constexpr u32 psq_lux(u32 d, u32 a, u32 b, u32 w, u32 i) { return (4u << 26) | (d << 21) | (a << 16) | (b << 11) | (w << 10) | (i << 7) | (38u << 1); }
constexpr u32 psq_stux(u32 s, u32 a, u32 b, u32 w, u32 i) { return (4u << 26) | (s << 21) | (a << 16) | (b << 11) | (w << 10) | (i << 7) | (39u << 1); }

} // namespace orca::test::ppc_asm
