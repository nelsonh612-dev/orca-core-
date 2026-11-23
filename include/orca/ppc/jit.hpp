// Orca Core — PowerPC (Gekko/Broadway) to AArch64 JIT.
//
// Blocks of guest instructions are translated to native AArch64 code (emitter in
// orca/jit/a64_emitter.hpp, executable memory in orca/jit/code_buffer.hpp). The
// interpreter (Cpu) stays the reference and the fallback:
//   * A block contains only instructions the translator knows; it ends before the
//     first other instruction, which the interpreter then executes with Cpu::step().
//   * Translated code never raises a guest exception: a memory access that is not
//     plain RAM through a valid translation (MMIO, page crossing, locked cache, a
//     fault) leaves the block *before* that instruction, and the interpreter
//     executes it, exceptions included.
//   * run(max_steps) only enters a block whose length fits in max_steps, so the
//     System can keep scheduler events, the decrementer and interrupts exactly
//     where the interpreter would see them (see System::run).
//   * Guest stores into translated code invalidate the blocks of that page (bus
//     code-page tracking); a block that wrote into itself stops after the store.
// The generated code only runs on an AArch64 host (jit::host_can_execute()).
#pragma once
#include "orca/jit/a64_emitter.hpp"
#include "orca/jit/code_buffer.hpp"
#include "orca/ppc/cpu.hpp"

#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace orca::ppc {

// Written by translated code when it leaves a block.
struct JitExit {
    u32 steps{0};   // guest instructions completed in the block
    u32 value{0};   // scratch for memory helpers
};

class Jit {
public:
    Jit(Cpu& cpu, mem::Bus& bus);
    ~Jit();
    Jit(const Jit&) = delete;
    Jit& operator=(const Jit&) = delete;

    // True when translated code can run here (AArch64 host, executable memory).
    bool can_execute() const noexcept;

    struct Result {
        StepResult result{StepResult::Executed};
        u64 steps{0}; // interpreter-equivalent steps (each advances time by one instruction)
    };
    // Executes one translated block of at most max_steps instructions, or one
    // interpreter step when no block fits or applies. max_steps >= 1.
    Result run(u64 max_steps);

    // Drops every block.
    void clear();

    // Native floating point (default on). Off: FP and paired-single arithmetic go
    // through the in-block interpreter call only (comparisons and measurements).
    // Takes effect for blocks compiled afterwards (call clear() to recompile).
    void set_native_fp(bool on) noexcept { native_fp_ = on; }

    // Translation without execution (tests on any host): the AArch64 words of the
    // block at `pc` and how many guest instructions it covers (0 = none).
    bool translate_for_test(u32 pc, std::vector<u32>& words, u32& guest_instructions, std::string& error);

    struct Stats {
        u64 blocks_compiled{0}, block_runs{0}, block_steps{0}, interpreter_steps{0}, invalidations{0},
            fallback_exits{0};
        u64 helper_steps{0}; // block instructions executed by the in-block interpreter call (FP, PS, lmw/stmw, ...)
        u64 chained{0};      // blocks entered directly after another one inside the same run()
        u64 tlb_fills{0};    // translation-cache entries created by the load/store helpers
    };
    const Stats& stats() const noexcept { return stats_; }

    // Maximum guest instructions per block.
    static constexpr u32 kMaxBlock = 64;

    // Translation cache for loads and stores made by translated code (direct-mapped
    // by effective page). An entry maps a 4 KiB effective page that is decided
    // without the page table (translation off, or a BAT) onto RAM with no locked-cache
    // line; write entries also require a page without translated code. Tags are
    // (page | 1), 0 = empty; host address = base + EA.
    struct TlbEntry {
        u32 read_tag{0}, write_tag{0};
        u64 base{0};
        u64* epoch{nullptr}; // the page's write stamp in the bus (nullptr: not tracked)
        u32 phys_page{0}, unused{0};
    };
    static constexpr u32 kTlbEntries = 256;

private:
    struct Block {
        u32 ea{0}, pa{0}, count{0};
        const u32* code{nullptr};
        bool valid{true};
    };
    struct Compiler;
    friend struct Compiler;
    // Helpers called from translated code (AAPCS64, Jit* first).
    static u32 helper_load(Jit* self, u32 ea, u32 size, JitExit* out);
    static u32 helper_store(Jit* self, u32 ea, u32 size, u32 value);
    static u32 helper_exec(Jit* self, u32 ins, u32 pc);

    Result interpret();
    void tlb_fill(u32 ea, bool write);
    void tlb_flush() noexcept { tlb_.fill(TlbEntry{}); }
    void tlb_drop_writes_to(u32 phys_page) noexcept;
    void tlb_check_context();
    Block* lookup_or_compile(u32 pc, u32 pa, std::string& error);
    bool compile(u32 pc, u32 pa, std::vector<u32>& words, u32& count, std::string& error);
    void invalidate(u32 address, u32 size);

    // First members: translated code reaches them with small offsets from x21 (this).
    std::array<TlbEntry, kTlbEntries> tlb_{};
    u64* bus_write_epoch_{nullptr};
    // What the cached translations depend on (checked at every run())
    struct TlbContext {
        u32 msr{0}, hid4{0};
        std::array<BatPair, 8> dbat{};
        u64 locked{0};
        bool cpu_observers{false};
        bool operator==(const TlbContext&) const = default;
    } tlb_context_{};

    Cpu& cpu_;
    mem::Bus& bus_;
    std::unique_ptr<jit::CodeBuffer> code_;
    std::unordered_map<u32, std::unique_ptr<Block>> blocks_;     // by guest effective address
    std::unordered_map<u32, std::vector<Block*>> page_blocks_;   // physical page -> blocks
    // Addresses (effective -> physical) whose first instruction is not translated:
    // the interpreter runs them without a new translation attempt. Dropped with the
    // page's blocks when its code changes.
    std::unordered_map<u32, u32> no_block_;
    std::vector<std::unique_ptr<Block>> retired_blocks_;         // invalid, kept until clear()
    Block* running_{nullptr};
    bool running_invalidated_{false};
    bool native_fp_{true};
    JitExit exit_{};
    Stats stats_;
};

} // namespace orca::ppc
