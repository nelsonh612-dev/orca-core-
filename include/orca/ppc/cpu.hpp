// Orca Core — Gekko/Broadway reference interpreter (integer + system subset).
//
// This interpreter is the accuracy reference for the future ARM64 JIT. Design
// rules:
//   * An instruction either completes, raises an *architectural* exception
//     (delivered through SRR0/SRR1/MSR and the exception vector, exactly as
//     the hardware does), or produces a *host stop*.
//   * Host stops (UnsupportedInstruction, InvalidForm, MemoryFault, ...) leave
//     the whole architectural state untouched, PC included, so the caller can
//     diagnose them. Nothing unsupported is ever executed as a NOP.
//   * No architectural state is modified before all memory accesses of an
//     instruction have been translated and validated.
#pragma once
#include "orca/memory/bus.hpp"
#include "orca/ppc/mmu.hpp"
#include "orca/ppc/fpu.hpp"
#include "orca/ppc/registers.hpp"
#include "orca/ppc/state.hpp"

#include <array>
#include <functional>
#include <string>
#include <unordered_map>

namespace orca::ppc {

enum class StepResult : u8 {
    Executed,               // instruction completed normally
    ExceptionTaken,         // an architectural exception/interrupt was delivered
    Idle,                   // MSR[POW] power-saving state, waiting for an interrupt
    Halted,                 // CpuState::halted was set by the host
    UnsupportedInstruction, // valid instruction or mode not implemented yet (host stop)
    InvalidForm,            // architecturally invalid/undefined encoding (host stop)
    MemoryFault,            // physical bus error: unmapped or device error (host stop)
    AlignmentFault,         // host set a PC that is not word aligned (host stop)
};

const char* to_string(StepResult r) noexcept;

struct CpuConfig {
    // Processor Version Register value. Supplied by the console layer; the
    // CPU module does not hard-code a Gekko or Broadway revision.
    u32 pvr{0};
    // Broadway (Wii): IBAT4-7/DBAT4-7 (SPR 560-575) exist and are used for
    // translation when HID4[SBE] is set. On Gekko those SPRs stop the CPU.
    bool extended_bats{false};
};

// DSISR value the CPU writes on an alignment exception caused by `ins`.
u32 alignment_dsisr(u32 ins) noexcept;

struct StopInfo {
    StepResult result{StepResult::Executed};
    u32 pc{0};
    u32 instruction{0};
    u32 address{0}; // faulting physical/effective address when relevant
    std::string reason;
};

class Cpu {
public:
    friend class Jit; // reads state and translation helpers, adds retired instructions
    Cpu(mem::Bus& bus, CpuConfig config);
    ~Cpu();
    Cpu(const Cpu&) = delete;
    Cpu& operator=(const Cpu&) = delete;

    CpuState& state() noexcept { return state_; }
    const CpuState& state() const noexcept { return state_; }
    const CpuConfig& config() const noexcept { return config_; }

    StepResult step();
    // Runs up to `budget` steps. Continues across Executed and ExceptionTaken;
    // returns the first other result, or Executed when the budget is used up.
    StepResult run(u64 budget);

    // Level-sensitive external interrupt line from the processor interface.
    void set_external_interrupt(bool asserted) noexcept { external_line_ = asserted; }
    bool external_interrupt() const noexcept { return external_line_; }

    // Advances the time base and the decrementer by `ticks` time-base ticks.
    // The tick rate relative to the CPU clock is the scheduler's job.
    void advance_timebase(u64 ticks) noexcept;

    // Called with the physical 32-byte block address when icbi executes.
    using IcacheInvalidateHandler = std::function<void(u32 physical_block)>;
    void set_icache_invalidate_handler(IcacheInvalidateHandler h) { icbi_handler_ = std::move(h); }

    // Locked L1 data cache (HID2[LCE]): 32-byte lines indexed by physical address.
    static constexpr std::size_t kLockedCacheLines = 512; // 16 KiB
    std::size_t locked_cache_lines() const noexcept { return locked_cache_.size(); }
    bool locked_cache_line(u32 physical, std::array<u8, 32>& out) const;

    Exception last_exception() const noexcept { return last_exception_; }
    const StopInfo& last_stop() const noexcept { return last_stop_; }
    u64 instructions_retired() const noexcept { return retired_; }
    // Counts instructions the system skipped as exact repeats (System's idle-loop skip).
    void add_retired(u64 n) noexcept { retired_ += n; }

private:
    struct Outcome;

    Outcome execute(u32 ins);
    // One instruction at `pc` through the interpreter's decoder, for the JIT (which
    // has already checked that it cannot touch MMIO). Precise: a result other than
    // Done leaves the state unchanged.
    Outcome execute_one_for_jit(u32 ins, u32 pc);
    void deliver(Exception e, u32 srr0, u32 srr1_bits);

    // Memory helpers (no architectural side effects on failure).
    Outcome load(u32 ea, unsigned size, u32& value);
    Outcome store(u32 ea, unsigned size, u32 value);
    Outcome load_raw(u32 ea, unsigned size, u64& value);   // size 1, 2, 4 or 8
    Outcome store_raw(u32 ea, unsigned size, u64 value);
    // Physical accesses that honour the locked cache.
    bool phys_read(u32 pa, unsigned size, u64& value);
    bool phys_write(u32 pa, unsigned size, u64 value);
    Outcome translate_data(u32 ea, u32 size, bool write, u32& pa_lo, u32& pa_hi, u32& split, u8& wimg);

    // Instruction groups
    Outcome op_load_store_d(u32 ins, unsigned op);
    Outcome op_load_store_x(u32 ins, unsigned xo, bool& handled);
    Outcome op_multiple(u32 ins, bool store);
    Outcome op_string(u32 ins, unsigned xo);
    Outcome op_integer_x(u32 ins, unsigned xo, bool& handled);
    Outcome op_integer_xo(u32 ins, bool& handled);
    Outcome op_system_x(u32 ins, unsigned xo, bool& handled);
    Outcome op_cache_x(u32 ins, unsigned xo, bool& handled);
    Outcome op_19(u32 ins);
    Outcome branch_conditional(u32 ins, u32 target_kind);
    // Floating point / paired single (cpu_fpu.cpp)
    Outcome fp_check_available(bool paired, bool quantized);
    // `adjusted`: the result already carries the OE/UE exponent adjustment
    // (fpu::arith_enabled); otherwise enabled overflow/underflow stop.
    Outcome fp_status(u32 exceptions, bool fr, bool fi, u32 fprf, bool set_fprf, bool tiny, bool& write,
                      bool adjusted = false);
    // arith_enabled with this CPU's FPSCR; a result the PEM leaves undefined stops.
    Outcome fp_arith(fpu::Op op, u64 a, u64 b, u64 c, bool single, fpu::Result& r, bool& adjusted);
    // `computational`: the FPSCR update comes from an arithmetic, rounding,
    // conversion or compare instruction (PEM: the only ones that raise
    // floating-point enabled exceptions).
    // A new MSR value that enables FP exceptions while FPSCR[FEX] = 1 (PEM
    // Table 6-4 lists mtmsr/rfi for that case without fixing SRR0).
    bool fp_enable_with_fex(u32 new_msr) const noexcept {
        return (new_msr & (MSR_FE0 | MSR_FE1)) != 0U && (state_.fpscr & 0x40000000U) != 0U;
    }
    Outcome fp_finish_control(u32 new_fpscr, bool computational = false);
    Outcome fp_compare(unsigned crf, u64 a, u64 b, bool ordered);
    Outcome op_fp_load_store(u32 ins, unsigned kind, bool indexed);
    Outcome op_fp59(u32 ins);
    Outcome op_fp63(u32 ins);
    Outcome op_paired(u32 ins);
    Outcome op_psq(u32 ins, bool indexed, bool update, bool is_store);
    Outcome op_dcbz_l(u32 ins);
    Outcome locked_cache_dma(u32 dmal);
    void set_cr1() noexcept;

    Outcome read_spr(unsigned n, u32& value);
    Outcome write_spr(unsigned n, u32 value);

    void set_cr_field(unsigned field, u32 nibble) noexcept;
    void set_cr0(u32 result) noexcept;
    void set_ca(bool ca) noexcept;
    void set_ov(bool ov) noexcept;
    bool problem_state() const noexcept { return (state_.msr & MSR_PR) != 0U; }

    mem::Bus& bus_;
    CpuConfig config_;
    CpuState state_{};
    u32 cur_pc_{0};
    u32 next_pc_{0};
    bool external_line_{false};
    Exception last_exception_{Exception::None};
    StopInfo last_stop_{};
    u64 retired_{0};
    std::size_t observer_id_{0};
    IcacheInvalidateHandler icbi_handler_;
    std::unordered_map<u32, std::array<u8, 32>> locked_cache_;
    u64 locked_cache_generation_{0}; // changes whenever a line is added or the cache is discarded
    // Set when a computational FP instruction completes with FPSCR[FEX] = 1
    // and MSR[FE0|FE1] != 0: step() then takes the program exception.
    bool fp_program_pending_{false};
};

} // namespace orca::ppc
