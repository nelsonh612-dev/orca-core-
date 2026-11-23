// Orca Core — PowerPC floating-point engine (host independent).
//
// Pure functions that implement the PEM floating-point semantics on raw
// IEEE-754 bit patterns: NaN propagation priority, invalid-operation
// detection, rounding in the four FPSCR[RN] modes for double and single
// precision, and the status information the FPSCR needs (FR, FI, OX, UX,
// XX, ZX, VX*). Host rounding is driven through <cfenv>; single-precision
// results use round-to-odd to avoid double rounding. Underflow uses
// tininess-before-rounding and is computed in software, so it does not
// depend on how the host (x86 or ARM) detects tininess.
//
// This file does not touch CPU state: integration with FPRs, FPSCR, CR1 and
// exceptions lives in cpu.cpp.
#pragma once
#include "orca/core/bits.hpp"

namespace orca::ppc::fpu {

// FPSCR bits (host numbering; IBM bit in the comment)
inline constexpr u32 FX = 0x80000000U;     // 0
inline constexpr u32 FEX = 0x40000000U;    // 1
inline constexpr u32 VX = 0x20000000U;     // 2
inline constexpr u32 OX = 0x10000000U;     // 3
inline constexpr u32 UX = 0x08000000U;     // 4
inline constexpr u32 ZX = 0x04000000U;     // 5
inline constexpr u32 XX = 0x02000000U;     // 6
inline constexpr u32 VXSNAN = 0x01000000U; // 7
inline constexpr u32 VXISI = 0x00800000U;  // 8
inline constexpr u32 VXIDI = 0x00400000U;  // 9
inline constexpr u32 VXZDZ = 0x00200000U;  // 10
inline constexpr u32 VXIMZ = 0x00100000U;  // 11
inline constexpr u32 VXVC = 0x00080000U;   // 12
inline constexpr u32 FR = 0x00040000U;     // 13
inline constexpr u32 FI = 0x00020000U;     // 14
inline constexpr u32 FPRF_MASK = 0x0001F000U; // 15-19 (C, FL, FG, FE, FU)
inline constexpr u32 FPCC_MASK = 0x0000F000U; // 16-19
inline constexpr u32 VXSOFT = 0x00000400U; // 21
inline constexpr u32 VXSQRT = 0x00000200U; // 22
inline constexpr u32 VXCVI = 0x00000100U;  // 23
inline constexpr u32 VE = 0x00000080U;     // 24
inline constexpr u32 OE = 0x00000040U;     // 25
inline constexpr u32 UE = 0x00000020U;     // 26
inline constexpr u32 ZE = 0x00000010U;     // 27
inline constexpr u32 XE = 0x00000008U;     // 28
inline constexpr u32 NI = 0x00000004U;     // 29
inline constexpr u32 RN_MASK = 0x00000003U;// 30-31

inline constexpr u32 VX_ALL = VXSNAN | VXISI | VXIDI | VXZDZ | VXIMZ | VXVC | VXSOFT | VXSQRT | VXCVI;
inline constexpr u32 EXCEPTION_BITS = OX | UX | ZX | XX | VX_ALL; // sticky bits that raise FX

// FPRF result-class codes (C FL FG FE FU), PEM table 2-x; shift left by 12.
inline constexpr u32 CLASS_QNAN = 0x11, CLASS_NEG_INF = 0x09, CLASS_NEG_NORM = 0x08, CLASS_NEG_DENORM = 0x18,
                     CLASS_NEG_ZERO = 0x12, CLASS_POS_ZERO = 0x02, CLASS_POS_DENORM = 0x14,
                     CLASS_POS_NORM = 0x04, CLASS_POS_INF = 0x05;

inline constexpr u64 kSignBit = 0x8000000000000000ULL;
inline constexpr u64 kExpMask = 0x7FF0000000000000ULL;
inline constexpr u64 kFracMask = 0x000FFFFFFFFFFFFFULL;
inline constexpr u64 kQuietBit = 0x0008000000000000ULL;
inline constexpr u64 kDefaultQNaN = 0x7FF8000000000000ULL;

constexpr bool is_nan(u64 b) noexcept { return (b & kExpMask) == kExpMask && (b & kFracMask) != 0U; }
constexpr bool is_snan(u64 b) noexcept { return is_nan(b) && (b & kQuietBit) == 0U; }
constexpr bool is_inf(u64 b) noexcept { return (b & ~kSignBit) == kExpMask; }
constexpr bool is_zero(u64 b) noexcept { return (b & ~kSignBit) == 0U; }
constexpr bool sign_of(u64 b) noexcept { return (b & kSignBit) != 0U; }

enum class Op : u8 {
    Add, Sub, Mul, Div,          // A+B, A-B, A*C, A/B
    MAdd, MSub, NMAdd, NMSub,    // A*C+B, A*C-B, -(A*C+B), -(A*C-B)
    Round,                        // frsp: round B to the target precision
    ReciprocalEstimate,           // fres / ps_res (1/B)
    RsqrtEstimate,                // frsqrte / ps_rsqrte (1/sqrt(B))
};

struct Result {
    u64 bits{0};       // value to write (double format)
    u32 exceptions{0}; // subset of EXCEPTION_BITS raised by this operation
    bool fr{false};
    bool fi{false};
    u32 fprf{0};       // class code (unshifted) of the result
    bool tiny{false};  // exact result tiny (needed when FPSCR[UE] = 1)
};

// Executes `op` on double-format operands a, b, c. `single` rounds the result
// to single precision (the value stays in double format, as in an FPR).
// `rn` is FPSCR[RN]: 0 nearest-even, 1 toward zero, 2 toward +inf, 3 toward -inf.
Result arith(Op op, u64 a, u64 b, u64 c, bool single, unsigned rn);

// arith() plus the enabled overflow/underflow rules (PEM 3.3.6.2.1-2,
// tables 3-14 and 3-16): with `oe` an overflowing result, and with `ue` a
// tiny one, is replaced by the correctly rounded exact result with its
// exponent adjusted by -1536 / +1536 (double) or -192 / +192 (single). The
// adjusted value keeps FR/FI of that rounding; OX (or UX) is set, XX if
// inexact. `adjusted` reports that this happened. `unrepresentable` is set
// when the adjusted value still does not fit the target format (the PEM does
// not define that case) - the caller must stop.
Result arith_enabled(Op op, u64 a, u64 b, u64 c, bool single, unsigned rn, bool oe, bool ue,
                     bool& adjusted, bool& unrepresentable);

// FPRF class of a value; with `single` the denormal range is that of binary32.
u32 classify(u64 bits, bool single) noexcept;

// Memory format conversions defined in PEM appendix D (bit exact, no rounding,
// signalling NaNs preserved).
u64 load_single(u32 word) noexcept;
u32 store_single(u64 bits) noexcept;

// fctiw / fctiwz. `value` is the 32-bit integer; FPSCR effects in exceptions/fr/fi.
struct IntResult { u32 value{0}; u32 exceptions{0}; bool fr{false}; bool fi{false}; };
IntResult to_int32(u64 b, unsigned rn, bool round_toward_zero);

// Ordered comparison code for FPCC/CR: 8 = less, 4 = greater, 2 = equal, 1 = unordered.
u32 compare(u64 a, u64 b) noexcept;

double to_double(u64 b) noexcept;
u64 from_double(double d) noexcept;

} // namespace orca::ppc::fpu
