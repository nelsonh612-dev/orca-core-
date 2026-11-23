// Orca Core — GameCube/Wii DSP core (16-bit Macronix DSP), interpreter.
//
// Written from the "GameCube DSP User's Manual" v0.1.8 (Duddie et al., GFDL),
// chapters Registers, Exceptions, Hardware interface and Opcodes. No emulator
// source code was used. The manual leaves several behaviours open; each one
// is either a clear host stop (failed() + error()) or a documented model
// choice (docs/ESTADO.md):
//   * generic reads/writes of $st0-$st3 (MRR, LRI, SR ... to registers 12-15):
//     host stop; CALL/RET/RTI/BLOOP/exceptions use the stacks as documented;
//   * stack overflow/underflow (STOVF): host stop;
//   * address arithmetic when the matching $wrN != 0xFFFF (wrapping is not
//     described): host stop;
//   * accelerator: ADPCM sample reads (FORMAT 0x0000: ADPCM from ARAM, 4-bit)
//     and 16-bit raw reads/writes are implemented; PCM formats and 8/4-bit raw
//     reads (formula / extension not documented) are host stops;
//   * IROM (0x8000-0x8FFF) and COEF (0x1000-0x17FF) need user dumps; reading or
//     executing them without one is a host stop;
//   * unknown opcodes, undocumented memory addresses and IFX registers: host
//     stop. Nothing unknown runs as a NOP.
// One instruction = one DSP cycle (timing model).
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <span>
#include <string>
#include <vector>

namespace orca::dsp {

inline constexpr u16 SR_C = 1U << 0, SR_O = 1U << 1, SR_Z = 1U << 2, SR_S = 1U << 3, SR_AS = 1U << 4,
                     SR_TB = 1U << 5, SR_LZ = 1U << 6, SR_OS = 1U << 7, SR_IE = 1U << 9, SR_EIE = 1U << 11,
                     SR_AM = 1U << 13, SR_SXM = 1U << 14, SR_SU = 1U << 15;

inline constexpr u16 kIramWords = 0x1000, kIromBase = 0x8000, kIromWords = 0x1000, kDramWords = 0x1000,
                     kCoefBase = 0x1000, kCoefWords = 0x0800;

// IFX registers (data memory 0xFFxx)
namespace ifx {
inline constexpr u16 DSCR = 0xFFC9, DSBL = 0xFFCB, DSPA = 0xFFCD, DSMAH = 0xFFCE, DSMAL = 0xFFCF, FORMAT = 0xFFD1,
                     ACUNK1 = 0xFFD2, ACDRAW = 0xFFD3, ACSAH = 0xFFD4, ACSAL = 0xFFD5, ACEAH = 0xFFD6, ACEAL = 0xFFD7,
                     ACCAH = 0xFFD8, ACCAL = 0xFFD9, PRED_SCALE = 0xFFDA, YN1 = 0xFFDB, YN2 = 0xFFDC, ACDSAMP = 0xFFDD,
                     GAIN = 0xFFDE, ACIN = 0xFFDF, AMDM_A = 0xFFED, AMDM_B = 0xFFEF, DIRQ = 0xFFFB, DMBH = 0xFFFC,
                     DMBL = 0xFFFD, CMBH = 0xFFFE, CMBL = 0xFFFF;
}

// Services the DSP needs from the console.
class DspHost {
public:
    virtual ~DspHost() = default;
    virtual bool dsp_dma_read(u32 physical, std::span<u8> out) = 0;
    virtual bool dsp_dma_write(u32 physical, std::span<const u8> in) = 0;
    virtual void dsp_interrupt_cpu() = 0; // DIRQ
    virtual void dsp_halted() = 0;        // HALT instruction
    // Accelerator access to ARAM (byte addresses). false = no ARAM / out of range.
    virtual bool dsp_aram_read8(u32 byte_address, u8& v) = 0;
    virtual bool dsp_aram_write16(u32 byte_address, u16 v) = 0;
};

struct DspStack {
    std::array<u16, 8> v{};
    u8 n{0};
    u8 cap{0};
    bool empty() const noexcept { return n == 0U; }
    u16 top() const noexcept { return n != 0U ? v[n - 1U] : 0U; }
};

struct DspState {
    std::array<u16, 4> ar{}, ix{}, wr{};
    std::array<DspStack, 4> st{}; // call, data, loop address, loop counter
    std::array<u8, 2> ac_h{};
    std::array<u16, 2> ac_m{}, ac_l{}, ax_h{}, ax_l{};
    u16 config{0}, sr{0};
    u16 prod_l{0}, prod_m1{0}, prod_h{0}, prod_m2{0};
    u16 pc{0};
};

class DspCore {
public:
    explicit DspCore(DspHost& host);

    // Clears the registers (see docs for the chosen reset values) and sets PC.
    void reset(u16 pc);
    bool load_irom(std::span<const u16> words);
    bool load_coef(std::span<const u16> words);
    std::array<u16, kIramWords>& iram() noexcept { return iram_; }
    std::array<u16, kDramWords>& dram() noexcept { return dram_; }
    DspState& state() noexcept { return s_; }

    bool halted() const noexcept { return halted_; }
    void set_halted(bool h) noexcept { halted_ = h; }
    bool failed() const noexcept { return failed_; }
    const std::string& error() const noexcept { return error_; }
    u64 cycles() const noexcept { return cycles_; }

    // Executes up to `max_instructions`; returns the number executed. Stops
    // early when halted or failed.
    u64 run(u64 max_instructions);
    bool step();

    // CPU side of the mailboxes. "CMB" carries mail CPU -> DSP, "DMB" DSP -> CPU.
    void cpu_write_cmb_high(u16 v) noexcept { cmb_ = (cmb_ & 0x0000FFFFU) | (static_cast<u32>(v) << 16); }
    void cpu_write_cmb_low(u16 v) noexcept { cmb_ = ((cmb_ & 0xFFFF0000U) | v) | 0x80000000U; }
    u16 cpu_read_cmb_high() const noexcept { return static_cast<u16>(cmb_ >> 16); }
    u16 cpu_read_cmb_low() const noexcept { return static_cast<u16>(cmb_); }
    u16 cpu_read_dmb_high() const noexcept { return static_cast<u16>(dmb_ >> 16); }
    u16 cpu_read_dmb_low() noexcept { const u16 v = static_cast<u16>(dmb_); dmb_ &= 0x7FFFFFFFU; return v; }
    void clear_mailboxes() noexcept { cmb_ = 0; dmb_ = 0; }
    void request_external_interrupt() noexcept { int_pending_ = true; }
    bool external_interrupt_pending() const noexcept { return int_pending_; }
    // Pending accelerator exceptions: bit 3 ACRROV, bit 4 ACRWOV, bit 5 ACSOV.
    u8 pending_exceptions() const noexcept { return exc_pending_; }

    // 40-bit helpers (public for tests)
    s64 acc(unsigned d) const noexcept;
    s64 prod() const noexcept;
    s32 ax(unsigned s) const noexcept;

private:
    using Handler = bool (DspCore::*)(u16 op);
    struct Op {
        u16 mask, value;
        Handler fn;
        u8 words;
        const char* name;
    };
    static const Op* decode(u16 op);

    // memory
    bool imem_read(u16 addr, u16& v);
    bool dmem_read(u16 addr, u16& v);
    bool dmem_write(u16 addr, u16 v);
    bool ifx_read(u16 addr, u16& v);
    bool ifx_write(u16 addr, u16 v);
    bool fetch(u16 addr, u16& v) { return imem_read(addr, v); }
    bool dsp_dma(u16 bytes);
    bool acc_read_sample(u16& v);
    bool acc_read_raw(u16& v);
    bool acc_write_raw(u16 v);
    u32 acc_ca() const noexcept { return (static_cast<u32>(accah_) << 16) | accal_; }
    u32 acc_sa() const noexcept { return (static_cast<u32>(acsah_) << 16) | acsal_; }
    u32 acc_ea() const noexcept { return (static_cast<u32>(aceah_) << 16) | aceal_; }
    void acc_set_ca(u32 v) noexcept { accah_ = static_cast<u16>(v >> 16); accal_ = static_cast<u16>(v); }
    // Increments ACCA; past ACEA it reloads ACSA and raises exception `level`.
    void acc_advance(u32 amount, unsigned level);

    // registers
    bool reg_read(unsigned r, u16& v, bool saturate);
    bool reg_write(unsigned r, u16 v, bool sign_extend);
    void set_acc(unsigned d, s64 v);
    void set_prod(s64 v);
    u16 acm_saturated(unsigned d) const noexcept;
    bool ar_add(unsigned r, s32 delta);
    bool push(unsigned stack, u16 v);
    bool pop(unsigned stack, u16& v);
    bool cond(unsigned cc) const noexcept;
    s64 multiply(u16 a, u16 b) const noexcept;

    // flags: spec letters per bit OS LZ TB AS S Z O C: 'X' computed, '0'/'1' forced, '-' kept
    void flags(const char* spec, s64 result, bool carry, bool overflow);
    void flags_add(const char* spec, s64 a, s64 b, s64 r);
    void flags_sub(const char* spec, s64 a, s64 b, s64 r);

    bool fail(std::string why);
    bool next_is_two_words(u16 at, bool& two);

    // extended opcode support
    struct ExtPlan {
        struct RegWrite { unsigned r; u16 v; bool sign_extend; };
        std::array<RegWrite, 4> regs{};
        unsigned nregs{0};
        std::array<std::pair<u16, u16>, 2> mem{};
        unsigned nmem{0};
        std::array<std::pair<unsigned, s32>, 2> ar{};
        unsigned nar{0};
    };
    bool plan_extension(u16 ext, ExtPlan& plan);
    bool apply_extension(const ExtPlan& plan);

    // main opcodes (alphabetical order of the manual)
#define ORCA_DSP_OP(name) bool op_##name(u16 op)
    ORCA_DSP_OP(nop); ORCA_DSP_OP(dar); ORCA_DSP_OP(iar); ORCA_DSP_OP(subarn); ORCA_DSP_OP(addarn); ORCA_DSP_OP(halt);
    ORCA_DSP_OP(loop); ORCA_DSP_OP(bloop); ORCA_DSP_OP(lri); ORCA_DSP_OP(lr); ORCA_DSP_OP(sr);
    ORCA_DSP_OP(ifcc); ORCA_DSP_OP(jcc); ORCA_DSP_OP(callcc); ORCA_DSP_OP(retcc); ORCA_DSP_OP(rticc);
    ORCA_DSP_OP(addi); ORCA_DSP_OP(xori); ORCA_DSP_OP(andi); ORCA_DSP_OP(ori); ORCA_DSP_OP(cmpi); ORCA_DSP_OP(andf);
    ORCA_DSP_OP(andcf); ORCA_DSP_OP(lsrn); ORCA_DSP_OP(asrn); ORCA_DSP_OP(ilrr);
    ORCA_DSP_OP(addis); ORCA_DSP_OP(cmpis); ORCA_DSP_OP(lris);
    ORCA_DSP_OP(loopi); ORCA_DSP_OP(bloopi); ORCA_DSP_OP(sbclr); ORCA_DSP_OP(sbset);
    ORCA_DSP_OP(shift_imm); ORCA_DSP_OP(si); ORCA_DSP_OP(jrcc); ORCA_DSP_OP(callrcc);
    ORCA_DSP_OP(lrr); ORCA_DSP_OP(srr); ORCA_DSP_OP(mrr); ORCA_DSP_OP(lrs); ORCA_DSP_OP(srsh); ORCA_DSP_OP(srs);
    ORCA_DSP_OP(logic_ax); ORCA_DSP_OP(logic_ac); ORCA_DSP_OP(xorc); ORCA_DSP_OP(notop); ORCA_DSP_OP(shiftn_ax);
    ORCA_DSP_OP(shiftn_ac);
    ORCA_DSP_OP(addr); ORCA_DSP_OP(addax); ORCA_DSP_OP(add); ORCA_DSP_OP(addp);
    ORCA_DSP_OP(subr); ORCA_DSP_OP(subax); ORCA_DSP_OP(sub); ORCA_DSP_OP(subp);
    ORCA_DSP_OP(movr); ORCA_DSP_OP(movax); ORCA_DSP_OP(mov); ORCA_DSP_OP(movp);
    ORCA_DSP_OP(addaxl); ORCA_DSP_OP(incm); ORCA_DSP_OP(inc); ORCA_DSP_OP(decm); ORCA_DSP_OP(dec); ORCA_DSP_OP(neg);
    ORCA_DSP_OP(movnp);
    ORCA_DSP_OP(nx); ORCA_DSP_OP(clr); ORCA_DSP_OP(cmp); ORCA_DSP_OP(mulaxh); ORCA_DSP_OP(clrp); ORCA_DSP_OP(tstprod);
    ORCA_DSP_OP(tstaxh); ORCA_DSP_OP(srbits);
    ORCA_DSP_OP(mul_family); ORCA_DSP_OP(asr16); ORCA_DSP_OP(mulx_family); ORCA_DSP_OP(abs); ORCA_DSP_OP(tst);
    ORCA_DSP_OP(mulc_family); ORCA_DSP_OP(cmpaxh);
    ORCA_DSP_OP(maddx); ORCA_DSP_OP(maddc); ORCA_DSP_OP(lsl16); ORCA_DSP_OP(madd); ORCA_DSP_OP(lsr16);
    ORCA_DSP_OP(addpaxz); ORCA_DSP_OP(clrl); ORCA_DSP_OP(movpz);
#undef ORCA_DSP_OP

    // helpers for the multiply families: r = destination acc, mode = 0 (mul), 1 (mvz), 2 (ac), 3 (mv)
    bool mul_into(s64 product, unsigned mode, unsigned r);
    bool prod_internal_carry() const noexcept;
    s64 shift_by(s64 a, u16 amount, bool arithmetic) const noexcept;
    void write_m(unsigned d, u16 v);
    void ext_write_raw(unsigned r, u16 v);

    DspHost& host_;
    DspState s_{};
    std::array<u16, kIramWords> iram_{};
    std::array<u16, kDramWords> dram_{};
    std::vector<u16> irom_, coef_;
    // IFX state
    u16 dscr_{0}, dspa_{0}, dsmah_{0}, dsmal_{0};
    std::array<u16, 16> adpcm_coef_{};
    u16 format_{0}, acunk1_{0}, acsah_{0}, acsal_{0}, aceah_{0}, aceal_{0}, accah_{0}, accal_{0}, pred_scale_{0},
        yn1_{0}, yn2_{0}, gain_{0}, acin_{0}, amdm_a_{0}, amdm_b_{0};
    u32 cmb_{0}, dmb_{0};
    bool int_pending_{false};
    u8 exc_pending_{0};

    bool halted_{true};
    bool failed_{false};
    std::string error_;
    u64 cycles_{0};
    u16 cur_pc_{0};
    u32 written_{0}; // registers written by the current main opcode (bit per index)
    // LOOP / LOOPI repeat state
    u16 rep_count_{0};
    u16 rep_pc_{0};
};

} // namespace orca::dsp
