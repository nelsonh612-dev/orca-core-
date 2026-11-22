// Orca Core — floating-point, paired-single and locked-cache instructions.
//
// Arithmetic semantics come from fpu.cpp. This file decodes the instructions,
// moves data between FPR slots and memory, and maintains FPSCR/CR1.
//
// Gekko-specific behaviour (750CL User's Manual, paired-single chapter):
//   * single-precision arithmetic (opcode 59), lfs/lfsu/lfsx/lfsux and frsp
//     write the result to both ps0 and ps1; double-precision results and lfd
//     only write ps0;
//   * paired-single instructions are illegal unless HID2[PSE] = 1, and psq_*
//     also need HID2[LSQE] = 1;
//   * quantized loads/stores convert through the GQR type/scale fields.
// Points that could not be verified against hardware are listed in
// docs/ESTADO.md ("pendiente de verificación en hardware").
#include "cpu_internal.hpp"
#include "orca/ppc/fpu.hpp"

#include <cmath>

namespace orca::ppc {
namespace F = fpu;

namespace {

constexpr u64 kOne = 0x3FF0000000000000ULL;
// High word written by fctiw/fctiwz and mffs. Architecturally undefined; this
// is the value publicly reported for 750-family hardware (PENDING verification).
constexpr u64 kUndefinedHigh = 0xFFF8000000000000ULL;

constexpr unsigned f_rcf(u32 i) noexcept { return (i >> 6U) & 31U; } // frC

u32 summarize(u32 f) noexcept {
    f &= ~(F::VX | F::FEX);
    if ((f & F::VX_ALL) != 0U) f |= F::VX;
    const bool fex = ((f & F::VX) != 0U && (f & F::VE) != 0U) || ((f & F::OX) != 0U && (f & F::OE) != 0U) ||
                     ((f & F::UX) != 0U && (f & F::UE) != 0U) || ((f & F::ZX) != 0U && (f & F::ZE) != 0U) ||
                     ((f & F::XX) != 0U && (f & F::XE) != 0U);
    if (fex) f |= F::FEX;
    return f;
}

bool select_ge_zero(u64 a) noexcept { return !F::is_nan(a) && F::to_double(a) >= 0.0; }

} // namespace

void Cpu::set_cr1() noexcept { set_cr_field(1, state_.fpscr >> 28U); }

Cpu::Outcome Cpu::fp_check_available(bool paired, bool quantized) {
    if (paired && (state_.hid2 & HID2_PSE) == 0U) return Outcome::exception(Exception::Program, SRR1_PROGRAM_ILLEGAL);
    if (quantized && (state_.hid2 & HID2_LSQE) == 0U) return Outcome::exception(Exception::Program, SRR1_PROGRAM_ILLEGAL);
    if ((state_.msr & MSR_FP) == 0U) return Outcome::exception(Exception::FloatingPointUnavailable);
    return Outcome::done();
}

Cpu::Outcome Cpu::fp_finish_control(u32 new_fpscr) {
    const u32 n = summarize(new_fpscr);
    if ((n & F::FEX) != 0U && (state_.msr & (MSR_FE0 | MSR_FE1)) != 0U)
        return Outcome::unsupported("floating-point enabled exception with MSR[FE0/FE1] != 0 (not implemented)");
    state_.fpscr = n;
    return Outcome::done();
}

Cpu::Outcome Cpu::fp_status(u32 exceptions, bool fr, bool fi, u32 fprf, bool set_fprf, bool tiny, bool& write) {
    const u32 f = state_.fpscr;
    if ((exceptions & F::OX) != 0U && (f & F::OE) != 0U)
        return Outcome::unsupported("overflow with FPSCR[OE]=1: exponent-adjusted result not implemented");
    if (tiny && (f & F::UE) != 0U)
        return Outcome::unsupported("underflow with FPSCR[UE]=1: exponent-adjusted result not implemented");
    // Enabled invalid-operation and zero-divide exceptions leave the target unchanged.
    const bool suppress = ((exceptions & F::VX_ALL) != 0U && (f & F::VE) != 0U) ||
                          ((exceptions & F::ZX) != 0U && (f & F::ZE) != 0U);
    u32 n = f | exceptions;
    if ((exceptions & ~f & F::EXCEPTION_BITS) != 0U) n |= F::FX;
    n &= ~(F::FR | F::FI);
    if (!suppress) {
        if (fr) n |= F::FR;
        if (fi) n |= F::FI;
        if (set_fprf) n = (n & ~F::FPRF_MASK) | (fprf << 12U);
    }
    Outcome o = fp_finish_control(n);
    if (!o.ok()) return o;
    write = !suppress;
    return o;
}

Cpu::Outcome Cpu::fp_compare(unsigned crf, u64 a, u64 b, bool ordered) {
    const u32 code = F::compare(a, b);
    u32 exc = 0;
    const bool snan = F::is_snan(a) || F::is_snan(b);
    if (snan) {
        exc |= F::VXSNAN;
        if (ordered && (state_.fpscr & F::VE) == 0U) exc |= F::VXVC;
    } else if (ordered && code == 1U) {
        exc |= F::VXVC;
    }
    u32 n = state_.fpscr | exc;
    if ((exc & ~state_.fpscr & F::EXCEPTION_BITS) != 0U) n |= F::FX;
    n = (n & ~F::FPCC_MASK) | (code << 12U);
    Outcome o = fp_finish_control(n);
    if (!o.ok()) return o;
    set_cr_field(crf, code);
    return o;
}

// ---------------------------------------------------------------------------
// Classic FP loads and stores
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_fp_load_store(u32 ins, unsigned kind, bool indexed) {
    Outcome o = fp_check_available(false, false);
    if (!o.ok()) return o;
    enum class K { LoadS, LoadD, StoreS, StoreD, StoreIW };
    K k; bool update = false;
    if (!indexed) {
        switch (kind) {
        case 48: k = K::LoadS; break;  case 49: k = K::LoadS; update = true; break;
        case 50: k = K::LoadD; break;  case 51: k = K::LoadD; update = true; break;
        case 52: k = K::StoreS; break; case 53: k = K::StoreS; update = true; break;
        case 54: k = K::StoreD; break; case 55: k = K::StoreD; update = true; break;
        default: return Outcome::unsupported("bad FP load/store opcode");
        }
    } else {
        switch (kind) {
        case 535: k = K::LoadS; break;  case 567: k = K::LoadS; update = true; break;
        case 599: k = K::LoadD; break;  case 631: k = K::LoadD; update = true; break;
        case 663: k = K::StoreS; break; case 695: k = K::StoreS; update = true; break;
        case 727: k = K::StoreD; break; case 759: k = K::StoreD; update = true; break;
        case 983: k = K::StoreIW; break;
        default: return Outcome::unsupported("bad FP indexed load/store");
        }
    }
    auto& g = state_.gpr;
    const unsigned ft = f_rt(ins), ra = f_ra(ins), rb = f_rb(ins);
    if (update && ra == 0U) return Outcome::invalid("FP load/store with update and RA=0");
    const u32 ea = (ra == 0U ? 0U : g[ra]) + (indexed ? g[rb] : static_cast<u32>(f_simm(ins)));
    if ((ea & 3U) != 0U) return Outcome::alignment(ea); // 750CL: FP accesses must be word aligned
    Fpr& fr = state_.fpr[ft];
    switch (k) {
    case K::LoadS: {
        u64 v = 0;
        o = load_raw(ea, 4, v);
        if (!o.ok()) return o;
        fr.ps0 = fr.ps1 = F::load_single(static_cast<u32>(v));
        break;
    }
    case K::LoadD: {
        u64 v = 0;
        o = load_raw(ea, 8, v);
        if (!o.ok()) return o;
        fr.ps0 = v;
        break;
    }
    case K::StoreS: o = store_raw(ea, 4, F::store_single(fr.ps0)); break;
    case K::StoreD: o = store_raw(ea, 8, fr.ps0); break;
    case K::StoreIW: o = store_raw(ea, 4, fr.ps0 & 0xFFFFFFFFULL); break;
    }
    if (!o.ok()) return o;
    if (update) g[ra] = ea;
    return o;
}

// ---------------------------------------------------------------------------
// Opcode 59: single-precision arithmetic
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_fp59(u32 ins) {
    Outcome o = fp_check_available(false, false);
    if (!o.ok()) return o;
    F::Op op;
    switch ((ins >> 1U) & 31U) {
    case 18: op = F::Op::Div; break;
    case 20: op = F::Op::Sub; break;
    case 21: op = F::Op::Add; break;
    case 24: op = F::Op::ReciprocalEstimate; break; // fres
    case 25: op = F::Op::Mul; break;
    case 28: op = F::Op::MSub; break;
    case 29: op = F::Op::MAdd; break;
    case 30: op = F::Op::NMSub; break;
    case 31: op = F::Op::NMAdd; break;
    case 22: return Outcome::unsupported("fsqrts is not implemented by the 750CL");
    default: return Outcome::unsupported("unknown opcode 59 instruction");
    }
    const auto& fp = state_.fpr;
    const F::Result r = F::arith(op, fp[f_ra(ins)].ps0, fp[f_rb(ins)].ps0, fp[f_rcf(ins)].ps0, true, state_.fpscr & F::RN_MASK);
    bool write = false;
    o = fp_status(r.exceptions, r.fr, r.fi, r.fprf, true, r.tiny, write);
    if (!o.ok()) return o;
    if (write) state_.fpr[f_rt(ins)].ps0 = state_.fpr[f_rt(ins)].ps1 = r.bits;
    if (f_rc(ins)) set_cr1();
    return o;
}

// ---------------------------------------------------------------------------
// Opcode 63: double-precision arithmetic, moves, conversions, FPSCR
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_fp63(u32 ins) {
    Outcome o = fp_check_available(false, false);
    if (!o.ok()) return o;
    const unsigned d = f_rt(ins);
    const Fpr A = state_.fpr[f_ra(ins)], B = state_.fpr[f_rb(ins)], C = state_.fpr[f_rcf(ins)];
    const unsigned rn = state_.fpscr & F::RN_MASK;
    Fpr& D = state_.fpr[d];

    const unsigned xo5 = (ins >> 1U) & 31U;
    bool a_form = true;
    F::Op op = F::Op::Add;
    switch (xo5) {
    case 18: op = F::Op::Div; break;
    case 20: op = F::Op::Sub; break;
    case 21: op = F::Op::Add; break;
    case 25: op = F::Op::Mul; break;
    case 26: op = F::Op::RsqrtEstimate; break;
    case 28: op = F::Op::MSub; break;
    case 29: op = F::Op::MAdd; break;
    case 30: op = F::Op::NMSub; break;
    case 31: op = F::Op::NMAdd; break;
    case 22: return Outcome::unsupported("fsqrt is not implemented by the 750CL");
    case 23: // fsel: no FPSCR effects
        D.ps0 = select_ge_zero(A.ps0) ? C.ps0 : B.ps0;
        if (f_rc(ins)) set_cr1();
        return o;
    default: a_form = false; break;
    }
    if (a_form) {
        const F::Result r = F::arith(op, A.ps0, B.ps0, C.ps0, false, rn);
        bool write = false;
        o = fp_status(r.exceptions, r.fr, r.fi, r.fprf, true, r.tiny, write);
        if (!o.ok()) return o;
        if (write) D.ps0 = r.bits;
        if (f_rc(ins)) set_cr1();
        return o;
    }

    const unsigned xo = (ins >> 1U) & 0x3FFU;
    const u32 f = state_.fpscr;
    switch (xo) {
    case 0: case 32: // fcmpu / fcmpo (Rc is reserved)
        return fp_compare((ins >> 23U) & 7U, A.ps0, B.ps0, xo == 32U);
    case 12: { // frsp
        const F::Result r = F::arith(F::Op::Round, 0, B.ps0, 0, true, rn);
        bool write = false;
        o = fp_status(r.exceptions, r.fr, r.fi, r.fprf, true, r.tiny, write);
        if (!o.ok()) return o;
        if (write) D.ps0 = D.ps1 = r.bits;
        break;
    }
    case 14: case 15: { // fctiw / fctiwz (FPRF undefined: left unchanged)
        const F::IntResult r = F::to_int32(B.ps0, rn, xo == 15U);
        bool write = false;
        o = fp_status(r.exceptions, r.fr, r.fi, 0, false, false, write);
        if (!o.ok()) return o;
        if (write) D.ps0 = kUndefinedHigh | r.value;
        break;
    }
    case 40: D.ps0 = B.ps0 ^ F::kSignBit; break;   // fneg
    case 72: D.ps0 = B.ps0; break;                 // fmr
    case 136: D.ps0 = B.ps0 | F::kSignBit; break;  // fnabs
    case 264: D.ps0 = B.ps0 & ~F::kSignBit; break; // fabs
    case 38: case 70: { // mtfsb1 / mtfsb0 (FEX and VX cannot be set or cleared explicitly)
        const unsigned bit = d;
        const u32 mask = 0x80000000U >> bit;
        if (bit == 1U || bit == 2U) break;
        u32 n = xo == 38U ? (f | mask) : (f & ~mask);
        if (xo == 38U && (mask & F::EXCEPTION_BITS) != 0U && (f & mask) == 0U) n |= F::FX;
        o = fp_finish_control(n);
        if (!o.ok()) return o;
        break;
    }
    case 64: { // mcrfs: copy a field to CR and clear its exception bits
        const unsigned crfs = (ins >> 18U) & 7U, shift = 28U - crfs * 4U;
        const u32 nibble = (f >> shift) & 0xFU;
        o = fp_finish_control(f & ~((0xFU << shift) & (F::FX | F::EXCEPTION_BITS)));
        if (!o.ok()) return o;
        set_cr_field((ins >> 23U) & 7U, nibble);
        return o; // Rc reserved
    }
    case 134: { // mtfsfi
        const unsigned shift = 28U - ((ins >> 23U) & 7U) * 4U;
        o = fp_finish_control((f & ~(0xFU << shift)) | (((ins >> 12U) & 0xFU) << shift));
        if (!o.ok()) return o;
        break;
    }
    case 583: D.ps0 = kUndefinedHigh | f; break; // mffs
    case 711: { // mtfsf
        const u32 fm = (ins >> 17U) & 0xFFU;
        u32 mask = 0;
        for (unsigned i = 0; i < 8U; ++i) if ((fm & (0x80U >> i)) != 0U) mask |= 0xF0000000U >> (i * 4U);
        o = fp_finish_control((f & ~mask) | (static_cast<u32>(B.ps0) & mask));
        if (!o.ok()) return o;
        break;
    }
    default:
        return Outcome::unsupported("unknown opcode 63 instruction");
    }
    if (f_rc(ins)) set_cr1();
    return o;
}

// ---------------------------------------------------------------------------
// Opcode 4: paired singles (Gekko/Broadway)
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_paired(u32 ins) {
    const unsigned xo10 = (ins >> 1U) & 0x3FFU, xo6 = (ins >> 1U) & 0x3FU, xo5 = (ins >> 1U) & 31U;
    if (xo10 == 1014U) return op_dcbz_l(ins);
    switch (xo6) {
    case 6: return op_psq(ins, true, false, false);   // psq_lx
    case 7: return op_psq(ins, true, false, true);    // psq_stx
    case 38: return op_psq(ins, true, true, false);   // psq_lux
    case 39: return op_psq(ins, true, true, true);    // psq_stux
    default: break;
    }
    Outcome o = fp_check_available(true, false);
    if (!o.ok()) return o;
    const Fpr A = state_.fpr[f_ra(ins)], B = state_.fpr[f_rb(ins)], C = state_.fpr[f_rcf(ins)];
    Fpr& D = state_.fpr[f_rt(ins)];
    const unsigned rn = state_.fpscr & F::RN_MASK;

    // Commits two single-precision slot results; FR/FI/FPRF come from slot `primary`.
    auto commit2 = [&](const F::Result& r0, const F::Result& r1, int primary) -> Outcome {
        const F::Result& p = primary == 0 ? r0 : r1;
        bool write = false;
        Outcome s = fp_status(r0.exceptions | r1.exceptions, p.fr, p.fi, p.fprf, true, r0.tiny || r1.tiny, write);
        if (!s.ok()) return s;
        if (write) { D.ps0 = r0.bits; D.ps1 = r1.bits; }
        return s;
    };
    auto slotwise = [&](F::Op op) {
        const F::Result r0 = F::arith(op, A.ps0, B.ps0, C.ps0, true, rn);
        const F::Result r1 = F::arith(op, A.ps1, B.ps1, C.ps1, true, rn);
        return commit2(r0, r1, 0);
    };

    bool handled = true;
    switch (xo5) {
    case 10: { // ps_sum0: ps0 = A0 + B1, ps1 = C1
        const F::Result r = F::arith(F::Op::Add, A.ps0, B.ps1, 0, true, rn);
        bool write = false;
        o = fp_status(r.exceptions, r.fr, r.fi, r.fprf, true, r.tiny, write);
        if (o.ok() && write) { D.ps0 = r.bits; D.ps1 = C.ps1; }
        break;
    }
    case 11: { // ps_sum1: ps0 = C0, ps1 = A0 + B1
        const F::Result r = F::arith(F::Op::Add, A.ps0, B.ps1, 0, true, rn);
        bool write = false;
        o = fp_status(r.exceptions, r.fr, r.fi, r.fprf, true, r.tiny, write);
        if (o.ok() && write) { D.ps0 = C.ps0; D.ps1 = r.bits; }
        break;
    }
    case 12: case 13: { // ps_muls0 / ps_muls1: scalar from C slot 0 / 1
        const u64 c = xo5 == 12U ? C.ps0 : C.ps1;
        o = commit2(F::arith(F::Op::Mul, A.ps0, 0, c, true, rn), F::arith(F::Op::Mul, A.ps1, 0, c, true, rn), 0);
        break;
    }
    case 14: case 15: { // ps_madds0 / ps_madds1
        const u64 c = xo5 == 14U ? C.ps0 : C.ps1;
        o = commit2(F::arith(F::Op::MAdd, A.ps0, B.ps0, c, true, rn), F::arith(F::Op::MAdd, A.ps1, B.ps1, c, true, rn), 0);
        break;
    }
    case 18: o = slotwise(F::Op::Div); break;
    case 20: o = slotwise(F::Op::Sub); break;
    case 21: o = slotwise(F::Op::Add); break;
    case 23: // ps_sel
        D.ps0 = select_ge_zero(A.ps0) ? C.ps0 : B.ps0;
        D.ps1 = select_ge_zero(A.ps1) ? C.ps1 : B.ps1;
        break;
    case 24: o = slotwise(F::Op::ReciprocalEstimate); break;
    case 25: o = slotwise(F::Op::Mul); break;
    case 26: o = slotwise(F::Op::RsqrtEstimate); break;
    case 28: o = slotwise(F::Op::MSub); break;
    case 29: o = slotwise(F::Op::MAdd); break;
    case 30: o = slotwise(F::Op::NMSub); break;
    case 31: o = slotwise(F::Op::NMAdd); break;
    default: handled = false; break;
    }
    if (!handled) {
        switch (xo10) {
        case 0: case 32: case 64: case 96: // ps_cmpu0 / ps_cmpo0 / ps_cmpu1 / ps_cmpo1 (Rc reserved)
            return fp_compare((ins >> 23U) & 7U, xo10 >= 64U ? A.ps1 : A.ps0, xo10 >= 64U ? B.ps1 : B.ps0, (xo10 & 32U) != 0U);
        case 40: D.ps0 = B.ps0 ^ F::kSignBit; D.ps1 = B.ps1 ^ F::kSignBit; break;     // ps_neg
        case 72: D.ps0 = B.ps0; D.ps1 = B.ps1; break;                                 // ps_mr
        case 136: D.ps0 = B.ps0 | F::kSignBit; D.ps1 = B.ps1 | F::kSignBit; break;    // ps_nabs
        case 264: D.ps0 = B.ps0 & ~F::kSignBit; D.ps1 = B.ps1 & ~F::kSignBit; break; // ps_abs
        case 528: D.ps0 = A.ps0; D.ps1 = B.ps0; break; // ps_merge00
        case 560: D.ps0 = A.ps0; D.ps1 = B.ps1; break; // ps_merge01
        case 592: D.ps0 = A.ps1; D.ps1 = B.ps0; break; // ps_merge10
        case 624: D.ps0 = A.ps1; D.ps1 = B.ps1; break; // ps_merge11
        default: return Outcome::unsupported("unknown paired-single instruction");
        }
    }
    if (!o.ok()) return o;
    if (f_rc(ins)) set_cr1();
    return o;
}

// ---------------------------------------------------------------------------
// Quantized loads/stores (psq_l, psq_lu, psq_lx, psq_lux, psq_st, ...)
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_psq(u32 ins, bool indexed, bool update, bool is_store) {
    Outcome o = fp_check_available(true, true);
    if (!o.ok()) return o;
    auto& g = state_.gpr;
    const unsigned ft = f_rt(ins), ra = f_ra(ins), rb = f_rb(ins);
    unsigned w, i;
    u32 ea;
    if (indexed) {
        w = (ins >> 10U) & 1U; i = (ins >> 7U) & 7U;
        ea = (ra == 0U ? 0U : g[ra]) + g[rb];
    } else {
        w = (ins >> 15U) & 1U; i = (ins >> 12U) & 7U;
        ea = (ra == 0U ? 0U : g[ra]) + static_cast<u32>(sign_extend(ins & 0xFFFU, 12));
    }
    if (update && ra == 0U) return Outcome::invalid("psq load/store with update and RA=0");
    const u32 gqr = state_.gqr[i];
    const unsigned type = is_store ? (gqr & 7U) : ((gqr >> 16U) & 7U);
    const int scale = sign_extend(is_store ? ((gqr >> 8U) & 0x3FU) : ((gqr >> 24U) & 0x3FU), 6);
    unsigned size;
    switch (type) {
    case 0: size = 4; break;           // single-precision float
    case 4: case 6: size = 1; break;   // u8 / s8
    case 5: case 7: size = 2; break;   // u16 / s16
    default: return Outcome::unsupported("GQR quantization type 1-3 is undefined");
    }
    const unsigned count = w != 0U ? 1U : 2U;
    const unsigned total = size * count;
    const u64 elem_mask = size == 4U ? 0xFFFFFFFFULL : ((1ULL << (size * 8U)) - 1U);
    Fpr& fr = state_.fpr[ft];

    if (!is_store) {
        u64 raw = 0;
        o = load_raw(ea, total, raw);
        if (!o.ok()) return o;
        u64 out[2] = {kOne, kOne};
        for (unsigned k = 0; k < count; ++k) {
            const u64 e = (raw >> ((count - 1U - k) * size * 8U)) & elem_mask;
            if (type == 0U) { out[k] = F::load_single(static_cast<u32>(e)); continue; }
            s64 iv;
            switch (type) {
            case 4: iv = static_cast<s64>(e); break;
            case 5: iv = static_cast<s64>(e); break;
            case 6: iv = sign_extend(static_cast<u32>(e), 8); break;
            default: iv = sign_extend(static_cast<u32>(e), 16); break;
            }
            // int * 2^-scale is exact in single precision for every type/scale.
            const auto value = static_cast<float>(std::ldexp(static_cast<double>(iv), -scale));
            out[k] = F::from_double(static_cast<double>(value));
        }
        fr.ps0 = out[0];
        fr.ps1 = w != 0U ? kOne : out[1];
    } else {
        const u64 slots[2] = {fr.ps0, fr.ps1};
        u64 raw = 0;
        for (unsigned k = 0; k < count; ++k) {
            u64 e;
            if (type == 0U) {
                e = F::store_single(slots[k]);
            } else {
                // value * 2^scale, saturated to the type range, truncated toward
                // zero; NaN stores 0. (PENDING hardware verification.)
                const double x = std::ldexp(F::to_double(slots[k]), scale);
                double lo = 0.0, hi = 0.0;
                switch (type) {
                case 4: lo = 0; hi = 255; break;
                case 5: lo = 0; hi = 65535; break;
                case 6: lo = -128; hi = 127; break;
                default: lo = -32768; hi = 32767; break;
                }
                const double c = std::isnan(x) ? 0.0 : (x < lo ? lo : (x > hi ? hi : x));
                e = static_cast<u64>(static_cast<s64>(c)) & elem_mask;
            }
            raw = (raw << (size * 8U)) | e;
        }
        o = store_raw(ea, total, raw);
        if (!o.ok()) return o;
    }
    if (update) g[ra] = ea;
    return o;
}

// ---------------------------------------------------------------------------
// Locked cache
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_dcbz_l(u32 ins) {
    if ((state_.hid2 & HID2_LCE) == 0U) return Outcome::exception(Exception::Program, SRR1_PROGRAM_ILLEGAL);
    const unsigned ra = f_ra(ins), rb = f_rb(ins);
    const u32 ea = (ra == 0U ? 0U : state_.gpr[ra]) + state_.gpr[rb];
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    Outcome o = translate_data(ea & ~31U, 32, true, lo, hi, split, wimg);
    if (!o.ok()) return o;
    auto it = locked_cache_.find(lo);
    if (it == locked_cache_.end()) {
        if (locked_cache_.size() >= kLockedCacheLines) return Outcome::unsupported("locked cache full (more than 16 KiB of lines)");
        locked_cache_.emplace(lo, std::array<u8, 32>{});
    } else {
        it->second.fill(0);
    }
    // The locked line is the CPU-visible copy of this physical block. Treat
    // the zero-fill as a write for reservation and code-coherency observers.
    bus_.notify_external_write(lo, 32U, mem::AccessSource::Cpu);
    return o;
}

// DMAU: MEM_ADDR[0:26] LEN_U[27:31]; DMAL: LC_ADDR[0:26] LD[27] LEN_L[28:29] T[30] F[31].
// Transfers complete immediately (no timing model yet), so DMAL reads back with T=0.
Cpu::Outcome Cpu::locked_cache_dma(u32 dmal) {
    auto& s = state_;
    if ((dmal & 2U) == 0U) { s.dmal = dmal; return Outcome::done(); }
    if ((s.hid2 & HID2_LCE) == 0U) return Outcome::unsupported("locked-cache DMA triggered with HID2[LCE]=0");
    if ((dmal & 1U) != 0U) { s.dmal = dmal & ~3U; return Outcome::done(); } // flush: queue already empty
    u32 lines = ((s.dmau & 0x1FU) << 2U) | ((dmal >> 2U) & 3U);
    if (lines == 0U) lines = 128U;
    const u32 mem_addr = s.dmau & ~31U, lc_addr = dmal & ~31U;
    const bool to_cache = ((dmal >> 4U) & 1U) != 0U;
    if (!bus_.is_ram(mem_addr, lines * 32U))
        return Outcome::stop(StepResult::MemoryFault, "locked-cache DMA memory range is not RAM", mem_addr);
    if (to_cache) {
        std::size_t new_lines = 0;
        for (u32 k = 0; k < lines; ++k) if (locked_cache_.find(lc_addr + k * 32U) == locked_cache_.end()) ++new_lines;
        if (locked_cache_.size() + new_lines > kLockedCacheLines) return Outcome::unsupported("locked-cache DMA overflows the 16 KiB locked cache");
        for (u32 k = 0; k < lines; ++k) {
            std::array<u8, 32> line{};
            (void)bus_.dma_read(mem_addr + k * 32U, line);
            locked_cache_[lc_addr + k * 32U] = line;
        }
    } else {
        for (u32 k = 0; k < lines; ++k)
            if (locked_cache_.find(lc_addr + k * 32U) == locked_cache_.end())
                return Outcome::unsupported("locked-cache DMA from a line that was never allocated");
        for (u32 k = 0; k < lines; ++k) (void)bus_.dma_write(mem_addr + k * 32U, locked_cache_[lc_addr + k * 32U]);
    }
    s.dmal = dmal & ~2U;
    return Outcome::done();
}

} // namespace orca::ppc
