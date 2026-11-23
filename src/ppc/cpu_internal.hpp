// Internal helpers shared by the interpreter translation units.
#pragma once
#include "orca/ppc/cpu.hpp"

#include <climits>

namespace orca::ppc {

struct Cpu::Outcome {
    enum class Kind : u8 { Done, Exception, Host };
    Kind kind{Kind::Done};
    Exception exc{Exception::None};
    u32 srr1_bits{0};
    bool srr0_next{false};
    bool set_dar{false};
    u32 dar{0};
    u32 dsisr{0};
    StepResult host{StepResult::Executed};
    const char* reason{""};
    u32 address{0};

    bool ok() const noexcept { return kind == Kind::Done; }
    static Outcome done() { return {}; }
    static Outcome exception(Exception e, u32 srr1 = 0, bool next = false) {
        Outcome o; o.kind = Kind::Exception; o.exc = e; o.srr1_bits = srr1; o.srr0_next = next; return o;
    }
    static Outcome dsi(u32 ea, u32 dsisr) {
        Outcome o = exception(Exception::DSI); o.set_dar = true; o.dar = ea; o.dsisr = dsisr; return o;
    }
    // DSISR is filled in by Cpu::step from the instruction (alignment_dsisr).
    static Outcome alignment(u32 ea) {
        Outcome o = exception(Exception::Alignment); o.set_dar = true; o.dar = ea; o.dsisr = 0; return o;
    }
    static Outcome privileged() { return exception(Exception::Program, SRR1_PROGRAM_PRIVILEGED); }
    static Outcome stop(StepResult r, const char* why, u32 addr = 0) {
        Outcome o; o.kind = Kind::Host; o.host = r; o.reason = why; o.address = addr; return o;
    }
    static Outcome unsupported(const char* why) { return stop(StepResult::UnsupportedInstruction, why); }
    static Outcome invalid(const char* why) { return stop(StepResult::InvalidForm, why); }
};

namespace detail {

constexpr unsigned f_rt(u32 i) noexcept { return (i >> 21U) & 31U; }
constexpr unsigned f_ra(u32 i) noexcept { return (i >> 16U) & 31U; }
constexpr unsigned f_rb(u32 i) noexcept { return (i >> 11U) & 31U; }
constexpr bool f_rc(u32 i) noexcept { return (i & 1U) != 0U; }
constexpr bool f_oe(u32 i) noexcept { return (i & 0x400U) != 0U; }
constexpr s32 f_simm(u32 i) noexcept { return sign_extend(i & 0xFFFFU, 16); }
constexpr u32 f_uimm(u32 i) noexcept { return i & 0xFFFFU; }
constexpr unsigned f_spr(u32 i) noexcept { return ((i >> 16U) & 31U) | (((i >> 11U) & 31U) << 5U); }

constexpr u32 mask_mb_me(unsigned mb, unsigned me) noexcept {
    const u32 begin = 0xFFFFFFFFU >> mb;
    const u32 end = me >= 31U ? 0xFFFFFFFFU : ~(0xFFFFFFFFU >> (me + 1U));
    return mb <= me ? (begin & end) : (begin | end);
}

struct AddResult { u32 value; bool carry; bool overflow; };

// x + y + carry_in with carry-out and signed overflow, as defined for the
// PowerPC add/subtract family (subtraction is ~a + b + 1).
constexpr AddResult add3(u32 x, u32 y, u32 carry_in) noexcept {
    const u64 wide = static_cast<u64>(x) + y + carry_in;
    const auto r = static_cast<u32>(wide);
    return {r, (wide >> 32U) != 0U, (((x ^ r) & (y ^ r)) >> 31U) != 0U};
}

inline bool trap_condition(unsigned to, u32 a, u32 b) noexcept {
    const auto sa = static_cast<s32>(a);
    const auto sb = static_cast<s32>(b);
    return ((to & 0x10U) != 0U && sa < sb) || ((to & 0x08U) != 0U && sa > sb) ||
           ((to & 0x04U) != 0U && a == b) || ((to & 0x02U) != 0U && a < b) ||
           ((to & 0x01U) != 0U && a > b);
}

inline bool is_classic_fp_x(unsigned xo) noexcept {
    switch (xo) {
    case 535: case 567: case 599: case 631: // lfsx lfsux lfdx lfdux
    case 663: case 695: case 727: case 759: // stfsx stfsux stfdx stfdux
    case 983:                               // stfiwx
        return true;
    default: return false;
    }
}

} // namespace detail
using namespace detail;

} // namespace orca::ppc
