// Orca Core — Gekko/Broadway reference interpreter.
//
// Instruction semantics follow the PowerPC Programming Environments Manual
// for 32-bit implementations (PEM) and the IBM PowerPC 750CL User's Manual.
// Encodings are cross-checked against LLVM's PowerPC assembler in the test
// suite (tests/encodings).
#include "cpu_internal.hpp"
#include "orca/ppc/jit/arm64.hpp"

#include <array>
#include <algorithm>
#include <vector>

namespace orca::ppc {

const char* to_string(StepResult r) noexcept {
    switch (r) {
    case StepResult::Executed: return "Executed";
    case StepResult::ExceptionTaken: return "ExceptionTaken";
    case StepResult::Idle: return "Idle";
    case StepResult::Halted: return "Halted";
    case StepResult::UnsupportedInstruction: return "UnsupportedInstruction";
    case StepResult::InvalidForm: return "InvalidForm";
    case StepResult::MemoryFault: return "MemoryFault";
    case StepResult::AlignmentFault: return "AlignmentFault";
    }
    return "?";
}


struct Cpu::JitBlockEntry {
    jit::Block block;
    jit::ExecutableBlock executable;
    u32 physical_start{};
    u32 physical_page{};
};

Cpu::Cpu(mem::Bus& bus, CpuConfig config) : bus_(bus), config_(config) {
    bus_.set_code_invalidation_handler([this](u32, u32) { invalidate_jit_cache(); });
    // Writes by other bus masters (DMA) to the reserved granule cancel a
    // lwarx reservation (PEM 5.1.2).
    observer_id_ = bus_.add_write_observer([this](const mem::WriteEvent& ev) {
        if (!state_.reserve || ev.source == mem::AccessSource::Cpu) return;
        const u64 lo = ev.address, hi = static_cast<u64>(ev.address) + ev.size;
        const u64 g_lo = state_.reserve_address, g_hi = g_lo + 32U;
        if (lo < g_hi && g_lo < hi) state_.reserve = false;
    });
}

Cpu::~Cpu() {
    bus_.remove_write_observer(observer_id_);
    bus_.set_code_invalidation_handler({});
    invalidate_jit_cache();
}

// ---------------------------------------------------------------------------
// Small state helpers
// ---------------------------------------------------------------------------
void Cpu::set_cr_field(unsigned field_index, u32 nibble) noexcept {
    const unsigned shift = 28U - (field_index & 7U) * 4U;
    state_.cr = (state_.cr & ~(0xFU << shift)) | ((nibble & 0xFU) << shift);
}

void Cpu::set_cr0(u32 result) noexcept {
    const auto s = static_cast<s32>(result);
    u32 f = s < 0 ? 8U : (s > 0 ? 4U : 2U);
    if ((state_.xer & XER_SO) != 0U) f |= 1U;
    set_cr_field(0, f);
}

void Cpu::set_ca(bool ca) noexcept { state_.xer = ca ? (state_.xer | XER_CA) : (state_.xer & ~XER_CA); }

void Cpu::set_ov(bool ov) noexcept {
    state_.xer = ov ? (state_.xer | XER_OV | XER_SO) : (state_.xer & ~XER_OV); // SO is sticky
}

void Cpu::advance_timebase(u64 ticks) noexcept {
    if (ticks == 0U) return;
    state_.tb += ticks;
    const auto before = static_cast<s32>(state_.dec);
    // Number of ticks until DEC goes from 0 to -1 (most significant bit 0 -> 1).
    const u64 distance = before >= 0 ? static_cast<u64>(before) + 1U
                                     : static_cast<u64>(static_cast<s64>(before) + (1LL << 32)) + 1U;
    if (ticks >= distance) state_.dec_pending = true;
    state_.dec = state_.dec - static_cast<u32>(ticks);
}

void Cpu::deliver(Exception e, u32 srr0, u32 srr1_bits) {
    state_.srr0 = srr0;
    state_.srr1 = (state_.msr & MSR_SAVE_MASK) | srr1_bits;
    const u32 kept = state_.msr & (MSR_IP | MSR_ME | MSR_ILE);
    state_.msr = kept | ((kept & MSR_ILE) != 0U ? MSR_LE : 0U);
    state_.pc = ((kept & MSR_IP) != 0U ? 0xFFF00000U : 0U) | static_cast<u32>(e);
    last_exception_ = e;
}

// ---------------------------------------------------------------------------
// Step / run
// ---------------------------------------------------------------------------
StepResult Cpu::step() {
    if (state_.halted) return StepResult::Halted;

    auto host_stop = [this](StepResult r, const char* why, u32 ins, u32 addr) {
        last_stop_ = StopInfo{r, state_.pc, ins, addr, why};
        return r;
    };

    if ((state_.msr & (MSR_SE | MSR_BE)) != 0U)
        return host_stop(StepResult::UnsupportedInstruction, "trace mode (MSR[SE]/MSR[BE]) not implemented", 0, 0);
    if ((state_.msr & MSR_LE) != 0U)
        return host_stop(StepResult::UnsupportedInstruction, "little-endian mode (MSR[LE]) not implemented", 0, 0);

    // Asynchronous interrupts are recognised between instructions.
    if ((state_.msr & MSR_EE) != 0U) {
        if (external_line_) { deliver(Exception::External, state_.pc, 0); return StepResult::ExceptionTaken; }
        if (state_.dec_pending) {
            state_.dec_pending = false;
            deliver(Exception::Decrementer, state_.pc, 0);
            return StepResult::ExceptionTaken;
        }
    }
    // MSR[POW] with a HID0 power-saving mode (DOZE/NAP/SLEEP) stops fetching
    // until an exception clears MSR[POW].
    if ((state_.msr & MSR_POW) != 0U && (state_.hid0 & 0x00E00000U) != 0U) return StepResult::Idle;

    cur_pc_ = state_.pc;
    if ((cur_pc_ & 3U) != 0U) return host_stop(StepResult::AlignmentFault, "PC not word aligned", 0, cur_pc_);

    const Translation t = translate(state_, bus_, cur_pc_, AccessType::Fetch);
    switch (t.status) {
    case TranslateStatus::Ok: break;
    case TranslateStatus::PageFault: deliver(Exception::ISI, cur_pc_, SRR1_ISI_NOT_FOUND); return StepResult::ExceptionTaken;
    case TranslateStatus::Protection: deliver(Exception::ISI, cur_pc_, SRR1_ISI_PROTECTION); return StepResult::ExceptionTaken;
    case TranslateStatus::NoExecute: deliver(Exception::ISI, cur_pc_, SRR1_ISI_NO_EXECUTE); return StepResult::ExceptionTaken;
    case TranslateStatus::DirectStore:
        return host_stop(StepResult::UnsupportedInstruction, "fetch from direct-store segment", 0, cur_pc_);
    case TranslateStatus::BusError:
        return host_stop(StepResult::MemoryFault, "page table read failed during fetch", 0, cur_pc_);
    }
    u32 ins = 0;
    if (bus_.read32(t.physical, ins) != mem::BusStatus::Ok)
        return host_stop(StepResult::MemoryFault, "instruction fetch from unmapped physical memory", 0, t.physical);

    next_pc_ = cur_pc_ + 4U;
    const Outcome o = execute(ins);
    switch (o.kind) {
    case Outcome::Kind::Done:
        state_.pc = next_pc_;
        ++retired_;
        return StepResult::Executed;
    case Outcome::Kind::Exception:
        if (o.set_dar) { state_.dar = o.dar; state_.dsisr = o.dsisr; }
        deliver(o.exc, o.srr0_next ? cur_pc_ + 4U : cur_pc_, o.srr1_bits);
        if (o.srr0_next) ++retired_; // sc completes before the exception
        return StepResult::ExceptionTaken;
    case Outcome::Kind::Host: {
        state_.pc = cur_pc_;
        const StepResult r = host_stop(o.host, o.reason, ins, o.address);
        if (o.host == StepResult::MemoryFault) {
            if (mem::MmioDevice* dev = bus_.mmio_device_at(o.address)) {
                last_stop_.reason += std::string(" [MMIO device ") + dev->mmio_name() + " rejected the access";
                const std::string why = dev->mmio_error();
                if (!why.empty()) last_stop_.reason += ": " + why;
                last_stop_.reason += "]";
            }
        }
        return r;
    }
    }
    return StepResult::UnsupportedInstruction;
}

StepResult Cpu::run(u64 budget) {
    for (u64 i = 0; i < budget; ++i) {
        const StepResult r = step();
        if (r != StepResult::Executed && r != StepResult::ExceptionTaken) return r;
    }
    return StepResult::Executed;
}

void Cpu::invalidate_jit_cache() noexcept {
    for (const u32 page : jit_code_pages_) bus_.unmark_code_page(page);
    jit_code_pages_.clear();
    jit_blocks_.clear();
}

StepResult Cpu::run_jit(u64 budget, u64* completed, const u32* stop_pc) {
    if (completed != nullptr) *completed = 0;
#if !defined(__aarch64__)
    // Host builds keep reference semantics and report completed instructions.
    for (u64 done = 0; done < budget; ++done) {
        if (stop_pc != nullptr && state_.pc == *stop_pc) {
            if (completed != nullptr) *completed = done;
            return StepResult::Executed;
        }
        const StepResult r = step();
        if (r != StepResult::Executed && r != StepResult::ExceptionTaken) {
            if (completed != nullptr) *completed = done;
            return r;
        }
        if (completed != nullptr) *completed = done + 1U;
    }
    return StepResult::Executed;
#else
    u64 done = 0;
    for (; done < budget;) {
        if (stop_pc != nullptr && state_.pc == *stop_pc) {
            if (completed != nullptr) *completed = done;
            return StepResult::Executed;
        }
        // Keep all architectural boundary checks in Cpu::step(). In particular,
        // do not bypass asynchronous interrupts, trace/LE modes, or POW idle.
        if (state_.halted || (state_.msr & (MSR_SE | MSR_BE | MSR_LE | MSR_POW)) != 0U ||
            (((state_.msr & MSR_EE) != 0U) && (external_line_ || state_.dec_pending))) {
            const StepResult r = step();
            if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
            ++done;
            continue;
        }
        const u32 pc = state_.pc;
        if ((pc & 3U) != 0U) {
            const StepResult r = step();
            if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
            ++done;
            continue;
        }

        const Translation first_translation = translate(state_, bus_, pc, AccessType::Fetch);
        if (first_translation.status != TranslateStatus::Ok || !bus_.is_ram(first_translation.physical, 4U)) {
            const StepResult r = step();
            if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
            ++done;
            continue;
        }
        auto it = jit_blocks_.find(pc);
        if (it != jit_blocks_.end() && it->second->physical_start != first_translation.physical) {
            // BAT/PTE remapped this effective entry point. Discard all cached
            // blocks so no stale physical mapping can be reused.
            invalidate_jit_cache();
            it = jit_blocks_.end();
        }
        if (it == jit_blocks_.end()) {
            std::vector<u32> words;
            const u64 remaining = budget - done;
            const std::size_t limit = static_cast<std::size_t>(std::min<u64>(remaining, 64U));
            words.reserve(limit);
            bool fetch_ok = true;
            for (std::size_t n = 0; n < limit; ++n) {
                const u32 ea = pc + static_cast<u32>(n * 4U);
                if ((ea >> mem::Bus::kPageShift) != (pc >> mem::Bus::kPageShift)) break;
                const Translation tr = n == 0U ? first_translation : translate(state_, bus_, ea, AccessType::Fetch);
                const u32 expected_pa = first_translation.physical + static_cast<u32>(n * 4U);
                if (tr.status != TranslateStatus::Ok || tr.physical != expected_pa ||
                    (tr.physical >> mem::Bus::kPageShift) != (first_translation.physical >> mem::Bus::kPageShift) ||
                    !bus_.is_ram(tr.physical, 4U)) {
                    if (n == 0U) fetch_ok = false;
                    break;
                }
                u32 ins = 0;
                if (bus_.read32(tr.physical, ins) != mem::BusStatus::Ok) { fetch_ok = false; break; }
                words.push_back(ins);
            }
            if (!fetch_ok || words.empty()) {
                const StepResult r = step();
                if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
                ++done;
                continue;
            }
            jit::Block block = jit::compile_arm64(words, pc);
            if (block.guest_instructions == 0U) {
                const StepResult r = step();
                if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
                ++done;
                continue;
            }
            auto entry = std::make_unique<JitBlockEntry>();
            entry->block = std::move(block);
            entry->executable = jit::ExecutableBlock(entry->block);
            entry->physical_start = first_translation.physical;
            entry->physical_page = first_translation.physical & ~(mem::Bus::kPageSize - 1U);
            bus_.mark_code_page(entry->physical_page);
            jit_code_pages_.insert(entry->physical_page);
            it = jit_blocks_.emplace(pc, std::move(entry)).first;
        }

        const std::size_t count = it->second->block.guest_instructions;
        bool mapping_valid = count != 0U && count <= budget - done;
        // A guest can rewrite segment/BAT/PTE mappings without writing the
        // translated code page. Revalidate every fetch translation before a
        // cached block is entered; otherwise a stale physical continuation
        // could execute bytes from the old mapping.
        for (std::size_t n = 0; mapping_valid && n < count; ++n) {
            const u32 ea = pc + static_cast<u32>(n * 4U);
            const Translation tr = translate(state_, bus_, ea, AccessType::Fetch);
            const u32 expected_pa = it->second->physical_start + static_cast<u32>(n * 4U);
            mapping_valid = tr.status == TranslateStatus::Ok && tr.physical == expected_pa &&
                            (tr.physical >> mem::Bus::kPageShift) == it->second->physical_page / mem::Bus::kPageSize &&
                            bus_.is_ram(tr.physical, 4U);
        }
        if (!mapping_valid) {
            invalidate_jit_cache();
            const StepResult r = step();
            if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
            ++done;
            continue;
        }
        if (!it->second->executable.execute(state_, this)) {
            const StepResult r = step();
            if (r != StepResult::Executed && r != StepResult::ExceptionTaken) { if (completed != nullptr) *completed = done; return r; }
            ++done;
            continue;
        }
        retired_ += count;
        done += count;
    }
    if (completed != nullptr) *completed = done;
    return StepResult::Executed;
#endif
}

// ---------------------------------------------------------------------------
// Data memory access
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::translate_data(u32 ea, u32 size, bool write, u32& pa_lo, u32& pa_hi, u32& split, u8& wimg) {
    const AccessType type = write ? AccessType::Write : AccessType::Read;
    auto fail = [&](const Translation& t, u32 addr) -> Outcome {
        switch (t.status) {
        case TranslateStatus::PageFault: return Outcome::dsi(addr, DSISR_NOT_FOUND | (write ? DSISR_STORE : 0U));
        case TranslateStatus::Protection: return Outcome::dsi(addr, DSISR_PROTECTION | (write ? DSISR_STORE : 0U));
        case TranslateStatus::DirectStore: return Outcome::unsupported("data access to direct-store segment");
        case TranslateStatus::BusError: return Outcome::stop(StepResult::MemoryFault, "page table read failed", addr);
        default: return Outcome::unsupported("unexpected translation status");
        }
    };
    const Translation first = translate(state_, bus_, ea, type);
    if (first.status != TranslateStatus::Ok) return fail(first, ea);
    pa_lo = first.physical;
    wimg = first.wimg;
    const u32 last = ea + (size - 1U); // effective addresses wrap modulo 2^32
    if ((ea >> 12U) == (last >> 12U)) { split = size; pa_hi = 0; return Outcome::done(); }
    split = 0x1000U - (ea & 0xFFFU);
    const Translation second = translate(state_, bus_, ea + split, type);
    if (second.status != TranslateStatus::Ok) return fail(second, ea);
    pa_hi = second.physical;
    return Outcome::done();
}

bool Cpu::locked_cache_line(u32 physical, std::array<u8, 32>& out) const {
    const auto it = locked_cache_.find(physical & ~31U);
    if (it == locked_cache_.end()) return false;
    out = it->second;
    return true;
}

namespace {
bool phys_accessible(const std::unordered_map<u32, std::array<u8, 32>>& lc, mem::Bus& bus, u32 pa, u32 n) {
    if (lc.empty()) return bus.is_mapped(pa, n);
    for (u32 i = 0; i < n; ++i)
        if (lc.find((pa + i) & ~31U) == lc.end() && !bus.is_mapped(pa + i, 1)) return false;
    return true;
}
} // namespace

bool Cpu::phys_read(u32 pa, unsigned size, u64& value) {
    if (locked_cache_.empty() ||
        (locked_cache_.find(pa & ~31U) == locked_cache_.end() &&
         locked_cache_.find((pa + size - 1U) & ~31U) == locked_cache_.end()))
        return bus_.read(pa, size, value) == mem::BusStatus::Ok;
    u64 v = 0;
    for (unsigned i = 0; i < size; ++i) {
        const u32 a = pa + i;
        const auto it = locked_cache_.find(a & ~31U);
        u8 b = 0;
        if (it != locked_cache_.end()) b = it->second[a & 31U];
        else if (bus_.read8(a, b) != mem::BusStatus::Ok) return false;
        v = (v << 8U) | b;
    }
    value = v;
    return true;
}

bool Cpu::phys_write(u32 pa, unsigned size, u64 value) {
    if (locked_cache_.empty() ||
        (locked_cache_.find(pa & ~31U) == locked_cache_.end() &&
         locked_cache_.find((pa + size - 1U) & ~31U) == locked_cache_.end()))
        return bus_.write(pa, size, value) == mem::BusStatus::Ok;
    if (!phys_accessible(locked_cache_, bus_, pa, size)) return false;
    for (unsigned i = 0; i < size; ++i) {
        const u32 a = pa + i;
        const auto b = static_cast<u8>(value >> ((size - 1U - i) * 8U));
        const auto it = locked_cache_.find(a & ~31U);
        if (it != locked_cache_.end()) it->second[a & 31U] = b;
        else if (bus_.write8(a, b) != mem::BusStatus::Ok) return false;
    }
    return true;
}

Cpu::Outcome Cpu::load_raw(u32 ea, unsigned size, u64& value) {
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    Outcome o = translate_data(ea, size, false, lo, hi, split, wimg);
    if (!o.ok()) return o;
    if (split == size) {
        if (!phys_read(lo, size, value)) return Outcome::stop(StepResult::MemoryFault, "load from unmapped physical memory", lo);
        return o;
    }
    if (!phys_accessible(locked_cache_, bus_, lo, split) || !phys_accessible(locked_cache_, bus_, hi, size - split))
        return Outcome::stop(StepResult::MemoryFault, "page-crossing load from unmapped physical memory", lo);
    u64 v = 0;
    for (u32 i = 0; i < size; ++i) {
        u64 b = 0;
        if (!phys_read(i < split ? lo + i : hi + (i - split), 1, b))
            return Outcome::stop(StepResult::MemoryFault, "page-crossing load failed", lo);
        v = (v << 8U) | b;
    }
    value = v;
    return o;
}

Cpu::Outcome Cpu::store_raw(u32 ea, unsigned size, u64 value) {
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    Outcome o = translate_data(ea, size, true, lo, hi, split, wimg);
    if (!o.ok()) return o;
    if (split == size) {
        if (!phys_write(lo, size, value)) return Outcome::stop(StepResult::MemoryFault, "store to unmapped physical memory", lo);
        return o;
    }
    if (!phys_accessible(locked_cache_, bus_, lo, split) || !phys_accessible(locked_cache_, bus_, hi, size - split))
        return Outcome::stop(StepResult::MemoryFault, "page-crossing store to unmapped physical memory", lo);
    for (u32 i = 0; i < size; ++i) {
        if (!phys_write(i < split ? lo + i : hi + (i - split), 1, (value >> ((size - 1U - i) * 8U)) & 0xFFU))
            return Outcome::stop(StepResult::MemoryFault, "page-crossing store failed", lo);
    }
    return o;
}

Cpu::Outcome Cpu::load(u32 ea, unsigned size, u32& value) {
    u64 v = 0;
    Outcome o = load_raw(ea, size, v);
    if (o.ok()) value = static_cast<u32>(v);
    return o;
}

Cpu::Outcome Cpu::store(u32 ea, unsigned size, u32 value) { return store_raw(ea, size, value); }

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::execute(u32 ins) {
    auto& g = state_.gpr;
    const unsigned op = ins >> 26U;
    const unsigned rt = f_rt(ins), ra = f_ra(ins);
    const s32 simm = f_simm(ins);
    const u32 uimm = f_uimm(ins);
    const u32 ra_or_0 = ra == 0U ? 0U : g[ra];

    switch (op) {
    case 3: // twi
        if (trap_condition(rt, g[ra], static_cast<u32>(simm))) return Outcome::exception(Exception::Program, SRR1_PROGRAM_TRAP);
        return Outcome::done();
    case 7: { // mulli
        const s64 p = static_cast<s64>(static_cast<s32>(g[ra])) * simm;
        g[rt] = static_cast<u32>(static_cast<u64>(p));
        return Outcome::done();
    }
    case 8: { // subfic
        const AddResult r = add3(~g[ra], static_cast<u32>(simm), 1U);
        g[rt] = r.value; set_ca(r.carry);
        return Outcome::done();
    }
    case 10: case 11: { // cmpli / cmpi
        if ((ins & (1U << 21U)) != 0U) return Outcome::invalid("cmpi/cmpli with L=1 (64-bit compare)");
        const u32 a = g[ra];
        u32 f;
        if (op == 10U) { f = a < uimm ? 8U : (a > uimm ? 4U : 2U); }
        else { const auto sa = static_cast<s32>(a); f = sa < simm ? 8U : (sa > simm ? 4U : 2U); }
        if ((state_.xer & XER_SO) != 0U) f |= 1U;
        set_cr_field((ins >> 23U) & 7U, f);
        return Outcome::done();
    }
    case 12: case 13: { // addic / addic.
        const AddResult r = add3(g[ra], static_cast<u32>(simm), 0U);
        g[rt] = r.value; set_ca(r.carry);
        if (op == 13U) set_cr0(r.value);
        return Outcome::done();
    }
    case 14: g[rt] = ra_or_0 + static_cast<u32>(simm); return Outcome::done();   // addi
    case 15: g[rt] = ra_or_0 + (uimm << 16U); return Outcome::done();            // addis
    case 16: return branch_conditional(ins, 0);
    case 17: // sc
        if ((ins & 2U) == 0U) return Outcome::invalid("sc without bit 30 set");
        return Outcome::exception(Exception::SystemCall, 0, true);
    case 18: { // b
        const s32 disp = sign_extend(ins & 0x03FFFFFCU, 26);
        if (f_rc(ins)) state_.lr = cur_pc_ + 4U;
        next_pc_ = (ins & 2U) != 0U ? static_cast<u32>(disp) : cur_pc_ + static_cast<u32>(disp);
        return Outcome::done();
    }
    case 19: return op_19(ins);
    case 20: case 21: case 23: { // rlwimi / rlwinm / rlwnm
        const unsigned sh = op == 23U ? (g[f_rb(ins)] & 31U) : f_rb(ins);
        const u32 m = mask_mb_me((ins >> 6U) & 31U, (ins >> 1U) & 31U);
        const u32 rot = rotl32(g[rt], sh);
        g[ra] = op == 20U ? ((rot & m) | (g[ra] & ~m)) : (rot & m);
        if (f_rc(ins)) set_cr0(g[ra]);
        return Outcome::done();
    }
    case 24: g[ra] = g[rt] | uimm; return Outcome::done();                       // ori
    case 25: g[ra] = g[rt] | (uimm << 16U); return Outcome::done();              // oris
    case 26: g[ra] = g[rt] ^ uimm; return Outcome::done();                       // xori
    case 27: g[ra] = g[rt] ^ (uimm << 16U); return Outcome::done();              // xoris
    case 28: g[ra] = g[rt] & uimm; set_cr0(g[ra]); return Outcome::done();       // andi.
    case 29: g[ra] = g[rt] & (uimm << 16U); set_cr0(g[ra]); return Outcome::done(); // andis.
    case 31: {
        const unsigned xo = (ins >> 1U) & 0x3FFU;
        bool handled = false;
        Outcome o = op_integer_x(ins, xo, handled);
        if (handled) return o;
        o = op_load_store_x(ins, xo, handled);
        if (handled) return o;
        o = op_system_x(ins, xo, handled);
        if (handled) return o;
        o = op_cache_x(ins, xo, handled);
        if (handled) return o;
        if (is_classic_fp_x(xo)) return op_fp_load_store(ins, xo, true);
        o = op_integer_xo(ins, handled);
        if (handled) return o;
        return Outcome::unsupported("unknown or unimplemented opcode 31 extended opcode");
    }
    case 32: case 33: case 34: case 35: case 36: case 37: case 38: case 39:
    case 40: case 41: case 42: case 43: case 44: case 45:
        return op_load_store_d(ins, op);
    case 46: return op_multiple(ins, false);
    case 47: return op_multiple(ins, true);
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
        return op_fp_load_store(ins, op, false);
    case 59: return op_fp59(ins);
    case 63: return op_fp63(ins);
    case 4: return op_paired(ins);
    case 56: return op_psq(ins, false, false, false); // psq_l
    case 57: return op_psq(ins, false, true, false);  // psq_lu
    case 60: return op_psq(ins, false, false, true);  // psq_st
    case 61: return op_psq(ins, false, true, true);   // psq_stu
    default:
        return Outcome::unsupported("unknown primary opcode");
    }
}

// ---------------------------------------------------------------------------
// Branches and condition register (opcode 16 / 19)
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::branch_conditional(u32 ins, u32 kind) {
    const unsigned bo = f_rt(ins), bi = f_ra(ins);
    if (kind == 2U && (bo & 4U) == 0U) return Outcome::invalid("bcctr with CTR decrement (BO[2]=0)");
    u32 target;
    if (kind == 0U) {
        const s32 disp = sign_extend(ins & 0xFFFCU, 16);
        target = (ins & 2U) != 0U ? static_cast<u32>(disp) : cur_pc_ + static_cast<u32>(disp);
    } else {
        target = (kind == 1U ? state_.lr : state_.ctr) & ~3U; // captured before LR/CTR update
    }
    bool ctr_ok = true;
    if ((bo & 4U) == 0U) {
        state_.ctr -= 1U;
        ctr_ok = (state_.ctr != 0U) != ((bo & 2U) != 0U);
    }
    const bool cond_ok = (bo & 16U) != 0U || (((state_.cr >> (31U - bi)) & 1U) != 0U) == ((bo & 8U) != 0U);
    if (f_rc(ins)) state_.lr = cur_pc_ + 4U;
    if (ctr_ok && cond_ok) next_pc_ = target;
    return Outcome::done();
}

Cpu::Outcome Cpu::op_19(u32 ins) {
    const unsigned xo = (ins >> 1U) & 0x3FFU;
    const unsigned bt = f_rt(ins), ba = f_ra(ins), bb = f_rb(ins);
    auto bit = [this](unsigned b) { return (state_.cr >> (31U - b)) & 1U; };
    auto cr_op = [&](u32 v) {
        state_.cr = (state_.cr & ~(1U << (31U - bt))) | ((v & 1U) << (31U - bt));
        return Outcome::done();
    };
    switch (xo) {
    case 0: // mcrf
        set_cr_field((ins >> 23U) & 7U, state_.cr >> (28U - ((ins >> 18U) & 7U) * 4U));
        return Outcome::done();
    case 16: return branch_conditional(ins, 1);   // bclr
    case 528: return branch_conditional(ins, 2);  // bcctr
    case 33: return cr_op(~(bit(ba) | bit(bb)));  // crnor
    case 129: return cr_op(bit(ba) & ~bit(bb));   // crandc
    case 193: return cr_op(bit(ba) ^ bit(bb));    // crxor
    case 225: return cr_op(~(bit(ba) & bit(bb))); // crnand
    case 257: return cr_op(bit(ba) & bit(bb));    // crand
    case 289: return cr_op(~(bit(ba) ^ bit(bb))); // creqv
    case 417: return cr_op(bit(ba) | ~bit(bb));   // crorc
    case 449: return cr_op(bit(ba) | bit(bb));    // cror
    case 150: return Outcome::done();             // isync: in-order interpreter, no prefetch to discard
    case 50: // rfi
        if (problem_state()) return Outcome::privileged();
        state_.msr = (state_.msr & ~MSR_SAVE_MASK) | (state_.srr1 & MSR_SAVE_MASK);
        next_pc_ = state_.srr0 & ~3U;
        return Outcome::done();
    default:
        return Outcome::unsupported("unknown opcode 19 extended opcode");
    }
}

// ---------------------------------------------------------------------------
// Integer X-form (opcode 31, 10-bit XO)
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_integer_x(u32 ins, unsigned xo, bool& handled) {
    auto& g = state_.gpr;
    const unsigned rs = f_rt(ins), ra = f_ra(ins), rb = f_rb(ins);
    handled = true;
    auto logical = [&](u32 v) { g[ra] = v; if (f_rc(ins)) set_cr0(v); return Outcome::done(); };
    switch (xo) {
    case 0: case 32: { // cmp / cmpl
        if ((ins & (1U << 21U)) != 0U) return Outcome::invalid("cmp/cmpl with L=1 (64-bit compare)");
        const u32 a = g[ra], b = g[rb];
        u32 f;
        if (xo == 0U) { const auto sa = static_cast<s32>(a), sb = static_cast<s32>(b); f = sa < sb ? 8U : (sa > sb ? 4U : 2U); }
        else f = a < b ? 8U : (a > b ? 4U : 2U);
        if ((state_.xer & XER_SO) != 0U) f |= 1U;
        set_cr_field((ins >> 23U) & 7U, f);
        return Outcome::done();
    }
    case 4: // tw
        if (trap_condition(rs, g[ra], g[rb])) return Outcome::exception(Exception::Program, SRR1_PROGRAM_TRAP);
        return Outcome::done();
    case 24: { const u32 sh = g[rb] & 0x3FU; return logical(sh >= 32U ? 0U : g[rs] << sh); }  // slw
    case 536: { const u32 sh = g[rb] & 0x3FU; return logical(sh >= 32U ? 0U : g[rs] >> sh); } // srw
    case 792: { // sraw
        const u32 sh = g[rb] & 0x3FU;
        const auto s = static_cast<s32>(g[rs]);
        u32 r; bool ca;
        if (sh >= 32U) { r = s < 0 ? 0xFFFFFFFFU : 0U; ca = s < 0; }
        else { r = static_cast<u32>(s >> sh); ca = s < 0 && (g[rs] & ((1U << sh) - 1U)) != 0U; }
        set_ca(ca);
        return logical(r);
    }
    case 824: { // srawi
        const u32 sh = rb;
        const auto s = static_cast<s32>(g[rs]);
        const u32 r = static_cast<u32>(s >> sh);
        set_ca(s < 0 && sh != 0U && (g[rs] & ((1U << sh) - 1U)) != 0U);
        return logical(r);
    }
    case 26: { // cntlzw
        u32 v = g[rs], n = 0;
        while (n < 32U && (v & 0x80000000U) == 0U) { v <<= 1U; ++n; }
        return logical(n);
    }
    case 28: return logical(g[rs] & g[rb]);    // and
    case 60: return logical(g[rs] & ~g[rb]);   // andc
    case 124: return logical(~(g[rs] | g[rb])); // nor
    case 284: return logical(~(g[rs] ^ g[rb])); // eqv
    case 316: return logical(g[rs] ^ g[rb]);   // xor
    case 412: return logical(g[rs] | ~g[rb]);  // orc
    case 444: return logical(g[rs] | g[rb]);   // or
    case 476: return logical(~(g[rs] & g[rb])); // nand
    case 922: return logical(static_cast<u32>(sign_extend(g[rs], 16))); // extsh
    case 954: return logical(static_cast<u32>(sign_extend(g[rs], 8)));  // extsb
    case 19: g[rs] = state_.cr; return Outcome::done(); // mfcr
    case 144: { // mtcrf
        const u32 fxm = (ins >> 12U) & 0xFFU;
        u32 mask = 0;
        for (unsigned i = 0; i < 8U; ++i) if ((fxm & (0x80U >> i)) != 0U) mask |= 0xF0000000U >> (i * 4U);
        state_.cr = (state_.cr & ~mask) | (g[rs] & mask);
        return Outcome::done();
    }
    case 512: // mcrxr
        set_cr_field((ins >> 23U) & 7U, state_.xer >> 28U);
        state_.xer &= 0x0FFFFFFFU;
        return Outcome::done();
    default:
        handled = false;
        return Outcome::done();
    }
}

// ---------------------------------------------------------------------------
// Integer XO-form (opcode 31, 9-bit XO + OE)
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_integer_xo(u32 ins, bool& handled) {
    auto& g = state_.gpr;
    const unsigned rt = f_rt(ins), ra = f_ra(ins), rb = f_rb(ins);
    const unsigned xo9 = (ins >> 1U) & 0x1FFU;
    const bool oe = f_oe(ins);
    const u32 ca_in = (state_.xer & XER_CA) != 0U ? 1U : 0U;
    handled = true;

    auto finish = [&](u32 value, bool ov) {
        g[rt] = value;
        if (oe) set_ov(ov);
        if (f_rc(ins)) set_cr0(value);
        return Outcome::done();
    };
    auto finish_carry = [&](const AddResult& r) { set_ca(r.carry); return finish(r.value, r.overflow); };

    switch (xo9) {
    case 266: return finish(add3(g[ra], g[rb], 0).value, add3(g[ra], g[rb], 0).overflow);  // add
    case 10: return finish_carry(add3(g[ra], g[rb], 0));                                  // addc
    case 138: return finish_carry(add3(g[ra], g[rb], ca_in));                             // adde
    case 202: return finish_carry(add3(g[ra], 0U, ca_in));                                // addze
    case 234: return finish_carry(add3(g[ra], 0xFFFFFFFFU, ca_in));                       // addme
    case 40: { const AddResult r = add3(~g[ra], g[rb], 1U); return finish(r.value, r.overflow); } // subf
    case 8: return finish_carry(add3(~g[ra], g[rb], 1U));                                 // subfc
    case 136: return finish_carry(add3(~g[ra], g[rb], ca_in));                            // subfe
    case 200: return finish_carry(add3(~g[ra], 0U, ca_in));                               // subfze
    case 232: return finish_carry(add3(~g[ra], 0xFFFFFFFFU, ca_in));                      // subfme
    case 104: { const AddResult r = add3(~g[ra], 0U, 1U); return finish(r.value, r.overflow); } // neg
    case 235: { // mullw
        const s64 p = static_cast<s64>(static_cast<s32>(g[ra])) * static_cast<s32>(g[rb]);
        return finish(static_cast<u32>(static_cast<u64>(p)), p < INT32_MIN || p > INT32_MAX);
    }
    case 75: case 11: { // mulhw / mulhwu (no OE form)
        if (oe) { handled = false; return Outcome::done(); }
        u32 hi;
        if (xo9 == 75U) hi = static_cast<u32>(static_cast<u64>(static_cast<s64>(static_cast<s32>(g[ra])) * static_cast<s32>(g[rb])) >> 32U);
        else hi = static_cast<u32>((static_cast<u64>(g[ra]) * g[rb]) >> 32U);
        return finish(hi, false);
    }
    case 491: { // divw
        const auto a = static_cast<s32>(g[ra]), b = static_cast<s32>(g[rb]);
        if (b == 0 || (a == INT32_MIN && b == -1)) {
            // Architecturally undefined result. Value follows publicly reported
            // 750-family hardware behaviour; PENDING verification on real HW.
            return finish(a < 0 ? 0xFFFFFFFFU : 0U, true);
        }
        return finish(static_cast<u32>(a / b), false);
    }
    case 459: { // divwu
        if (g[rb] == 0U) return finish(0U, true); // undefined result; PENDING hardware verification
        return finish(g[ra] / g[rb], false);
    }
    default:
        handled = false;
        return Outcome::done();
    }
}

// ---------------------------------------------------------------------------
// Loads and stores
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::op_load_store_d(u32 ins, unsigned op) {
    auto& g = state_.gpr;
    const unsigned rt = f_rt(ins), ra = f_ra(ins);
    const bool update = (op & 1U) != 0U;
    const bool is_load = op <= 35U || (op >= 40U && op <= 43U);
    const unsigned size = (op <= 33U || op == 36U || op == 37U) ? 4U : ((op >= 40U) ? 2U : 1U);
    if (update && ra == 0U) return Outcome::invalid("load/store with update and RA=0");
    if (update && is_load && ra == rt) return Outcome::invalid("load with update and RA=RT");
    const u32 ea = (ra == 0U ? 0U : g[ra]) + static_cast<u32>(f_simm(ins));
    if (is_load) {
        u32 v = 0;
        Outcome o = load(ea, size, v);
        if (!o.ok()) return o;
        if (op == 42U || op == 43U) v = static_cast<u32>(sign_extend(v, 16));
        g[rt] = v;
    } else {
        Outcome o = store(ea, size, g[rt]);
        if (!o.ok()) return o;
    }
    if (update) g[ra] = ea;
    return Outcome::done();
}

Cpu::Outcome Cpu::op_load_store_x(u32 ins, unsigned xo, bool& handled) {
    auto& g = state_.gpr;
    const unsigned rt = f_rt(ins), ra = f_ra(ins), rb = f_rb(ins);
    struct Desc { unsigned size; bool load; bool sign; bool update; bool reverse; };
    Desc d{};
    handled = true;
    switch (xo) {
    case 23:  d = {4, true, false, false, false}; break;  // lwzx
    case 55:  d = {4, true, false, true, false}; break;   // lwzux
    case 87:  d = {1, true, false, false, false}; break;  // lbzx
    case 119: d = {1, true, false, true, false}; break;   // lbzux
    case 279: d = {2, true, false, false, false}; break;  // lhzx
    case 311: d = {2, true, false, true, false}; break;   // lhzux
    case 343: d = {2, true, true, false, false}; break;   // lhax
    case 375: d = {2, true, true, true, false}; break;    // lhaux
    case 151: d = {4, false, false, false, false}; break; // stwx
    case 183: d = {4, false, false, true, false}; break;  // stwux
    case 215: d = {1, false, false, false, false}; break; // stbx
    case 247: d = {1, false, false, true, false}; break;  // stbux
    case 407: d = {2, false, false, false, false}; break; // sthx
    case 439: d = {2, false, false, true, false}; break;  // sthux
    case 534: d = {4, true, false, false, true}; break;   // lwbrx
    case 790: d = {2, true, false, false, true}; break;   // lhbrx
    case 662: d = {4, false, false, false, true}; break;  // stwbrx
    case 918: d = {2, false, false, false, true}; break;  // sthbrx
    case 20: { // lwarx
        const u32 ea = (ra == 0U ? 0U : g[ra]) + g[rb];
        if ((ea & 3U) != 0U) return Outcome::alignment(ea);
        u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
        Outcome o = translate_data(ea, 4, false, lo, hi, split, wimg);
        if (!o.ok()) return o;
        u32 v = 0;
        if (bus_.read32(lo, v) != mem::BusStatus::Ok) return Outcome::stop(StepResult::MemoryFault, "lwarx from unmapped physical memory", lo);
        g[rt] = v;
        state_.reserve = true;
        state_.reserve_address = lo & ~31U;
        return Outcome::done();
    }
    case 150: { // stwcx.
        if (!f_rc(ins)) return Outcome::invalid("stwcx without Rc=1");
        const u32 ea = (ra == 0U ? 0U : g[ra]) + g[rb];
        if ((ea & 3U) != 0U) return Outcome::alignment(ea);
        u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
        Outcome o = translate_data(ea, 4, true, lo, hi, split, wimg);
        if (!o.ok()) return o;
        const bool success = state_.reserve && (lo & ~31U) == state_.reserve_address;
        if (success && bus_.write32(lo, g[rt]) != mem::BusStatus::Ok)
            return Outcome::stop(StepResult::MemoryFault, "stwcx. to unmapped physical memory", lo);
        state_.reserve = false;
        set_cr_field(0, (success ? 2U : 0U) | ((state_.xer & XER_SO) != 0U ? 1U : 0U));
        return Outcome::done();
    }
    case 533: case 597: case 661: case 725:
        return op_string(ins, xo);
    default:
        handled = false;
        return Outcome::done();
    }
    if (d.update && ra == 0U) return Outcome::invalid("indexed load/store with update and RA=0");
    if (d.update && d.load && ra == rt) return Outcome::invalid("indexed load with update and RA=RT");
    const u32 ea = (ra == 0U ? 0U : g[ra]) + g[rb];
    if (d.load) {
        u32 v = 0;
        Outcome o = load(ea, d.size, v);
        if (!o.ok()) return o;
        if (d.reverse) v = d.size == 4U ? bswap32(v) : bswap16(static_cast<u16>(v));
        if (d.sign) v = static_cast<u32>(sign_extend(v, 16));
        g[rt] = v;
    } else {
        u32 v = g[rt];
        if (d.reverse) v = d.size == 4U ? bswap32(v) : bswap16(static_cast<u16>(v));
        Outcome o = store(ea, d.size, v);
        if (!o.ok()) return o;
    }
    if (d.update) g[ra] = ea;
    return Outcome::done();
}

Cpu::Outcome Cpu::op_multiple(u32 ins, bool is_store) {
    auto& g = state_.gpr;
    const unsigned rt = f_rt(ins), ra = f_ra(ins);
    if (!is_store && ra >= rt) return Outcome::invalid("lmw with RA in the range of loaded registers");
    const u32 ea = (ra == 0U ? 0U : g[ra]) + static_cast<u32>(f_simm(ins));
    if ((ea & 3U) != 0U) return Outcome::alignment(ea); // 750CL: lmw/stmw require word alignment
    const u32 bytes = (32U - rt) * 4U;
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    Outcome o = translate_data(ea, bytes, is_store, lo, hi, split, wimg);
    if (!o.ok()) return o;
    if (!bus_.is_mapped(lo, split) || (split < bytes && !bus_.is_mapped(hi, bytes - split)))
        return Outcome::stop(StepResult::MemoryFault, "lmw/stmw touches unmapped physical memory", lo);
    auto pa = [&](u32 off) { return off < split ? lo + off : hi + (off - split); };
    if (is_store) {
        for (unsigned r = rt; r < 32U; ++r)
            if (bus_.write32(pa((r - rt) * 4U), g[r]) != mem::BusStatus::Ok)
                return Outcome::stop(StepResult::MemoryFault, "stmw store failed", pa((r - rt) * 4U));
    } else {
        std::array<u32, 32> tmp{};
        for (unsigned r = rt; r < 32U; ++r)
            if (bus_.read32(pa((r - rt) * 4U), tmp[r]) != mem::BusStatus::Ok)
                return Outcome::stop(StepResult::MemoryFault, "lmw load failed", pa((r - rt) * 4U));
        for (unsigned r = rt; r < 32U; ++r) g[r] = tmp[r];
    }
    return Outcome::done();
}

Cpu::Outcome Cpu::op_string(u32 ins, unsigned xo) {
    auto& g = state_.gpr;
    const unsigned rt = f_rt(ins), ra = f_ra(ins), rb = f_rb(ins);
    const bool immediate = xo == 597U || xo == 725U;
    const bool is_store = xo == 661U || xo == 725U;
    u32 ea, n;
    if (immediate) { ea = ra == 0U ? 0U : g[ra]; n = rb == 0U ? 32U : rb; }
    else { ea = (ra == 0U ? 0U : g[ra]) + g[rb]; n = state_.xer & XER_BYTE_COUNT; }
    if (n == 0U) return Outcome::done(); // lswx/stswx with byte count 0 transfer nothing
    const unsigned nregs = (n + 3U) / 4U;
    if (!is_store) {
        for (unsigned i = 0; i < nregs; ++i) {
            const unsigned r = (rt + i) & 31U;
            if (r == ra || (!immediate && r == rb))
                return Outcome::invalid("lswi/lswx loads into RA or RB");
        }
    }
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    Outcome o = translate_data(ea, n, is_store, lo, hi, split, wimg);
    if (!o.ok()) return o;
    if (!bus_.is_mapped(lo, split) || (split < n && !bus_.is_mapped(hi, n - split)))
        return Outcome::stop(StepResult::MemoryFault, "string access touches unmapped physical memory", lo);
    auto pa = [&](u32 off) { return off < split ? lo + off : hi + (off - split); };
    if (is_store) {
        for (u32 i = 0; i < n; ++i) {
            const u32 reg = g[(rt + i / 4U) & 31U];
            const auto b = static_cast<u8>(reg >> (24U - (i % 4U) * 8U));
            if (bus_.write8(pa(i), b) != mem::BusStatus::Ok)
                return Outcome::stop(StepResult::MemoryFault, "string store failed", pa(i));
        }
        return Outcome::done();
    }
    std::array<u32, 32> tmp{};
    for (u32 i = 0; i < n; ++i) {
        u8 b = 0;
        if (bus_.read8(pa(i), b) != mem::BusStatus::Ok)
            return Outcome::stop(StepResult::MemoryFault, "string load failed", pa(i));
        tmp[i / 4U] |= static_cast<u32>(b) << (24U - (i % 4U) * 8U); // unfilled low bytes stay 0
    }
    for (unsigned i = 0; i < nregs; ++i) g[(rt + i) & 31U] = tmp[i];
    return Outcome::done();
}

// ---------------------------------------------------------------------------
// System instructions
// ---------------------------------------------------------------------------
Cpu::Outcome Cpu::read_spr(unsigned n, u32& v) {
    auto& s = state_;
    if (n >= spr::IBAT0U && n < spr::IBAT0U + 8U) { const BatPair& b = s.ibat[(n - spr::IBAT0U) / 2U]; v = (n & 1U) ? b.lower : b.upper; return Outcome::done(); }
    if (n >= spr::DBAT0U && n < spr::DBAT0U + 8U) { const BatPair& b = s.dbat[(n - spr::DBAT0U) / 2U]; v = (n & 1U) ? b.lower : b.upper; return Outcome::done(); }
    if (n >= spr::GQR0 && n < spr::GQR0 + 8U) { v = s.gqr[n - spr::GQR0]; return Outcome::done(); }
    if (n >= spr::SPRG0 && n <= spr::SPRG3) { v = s.sprg[n - spr::SPRG0]; return Outcome::done(); }
    switch (n) {
    case spr::XER: v = s.xer; return Outcome::done();
    case spr::LR: v = s.lr; return Outcome::done();
    case spr::CTR: v = s.ctr; return Outcome::done();
    case spr::DSISR: v = s.dsisr; return Outcome::done();
    case spr::DAR: v = s.dar; return Outcome::done();
    case spr::DEC: v = s.dec; return Outcome::done();
    case spr::SDR1: v = s.sdr1; return Outcome::done();
    case spr::SRR0: v = s.srr0; return Outcome::done();
    case spr::SRR1: v = s.srr1; return Outcome::done();
    case spr::EAR: v = s.ear; return Outcome::done();
    case spr::PVR: v = config_.pvr; return Outcome::done();
    case spr::HID0: v = s.hid0; return Outcome::done();
    case spr::HID1: v = s.hid1; return Outcome::done();
    case spr::HID2: v = s.hid2; return Outcome::done();
    case spr::HID4: v = s.hid4; return Outcome::done();
    case spr::IABR: v = s.iabr; return Outcome::done();
    case spr::DABR: v = s.dabr; return Outcome::done();
    case spr::L2CR: v = s.l2cr; return Outcome::done();
    case spr::ICTC: v = s.ictc; return Outcome::done();
    case spr::THRM1: case spr::THRM2: case spr::THRM3: v = s.thrm[n - spr::THRM1]; return Outcome::done();
    case spr::WPAR: v = s.wpar; return Outcome::done();
    case spr::DMAU: v = s.dmau; return Outcome::done();
    case spr::DMAL: v = s.dmal; return Outcome::done();
    case spr::MMCR0: case spr::UMMCR0: v = s.mmcr0; return Outcome::done();
    case spr::MMCR1: case spr::UMMCR1: v = s.mmcr1; return Outcome::done();
    case spr::SIA: case spr::USIA: v = s.sia; return Outcome::done();
    case spr::PMC1: case spr::UPMC1: v = s.pmc[0]; return Outcome::done();
    case spr::PMC2: case spr::UPMC2: v = s.pmc[1]; return Outcome::done();
    case spr::PMC3: case spr::UPMC3: v = s.pmc[2]; return Outcome::done();
    case spr::PMC4: case spr::UPMC4: v = s.pmc[3]; return Outcome::done();
    default: return Outcome::unsupported("mfspr of unknown/unimplemented SPR");
    }
}

Cpu::Outcome Cpu::write_spr(unsigned n, u32 v) {
    auto& s = state_;
    if (n >= spr::IBAT0U && n < spr::IBAT0U + 8U) { BatPair& b = s.ibat[(n - spr::IBAT0U) / 2U]; ((n & 1U) ? b.lower : b.upper) = v; return Outcome::done(); }
    if (n >= spr::DBAT0U && n < spr::DBAT0U + 8U) { BatPair& b = s.dbat[(n - spr::DBAT0U) / 2U]; ((n & 1U) ? b.lower : b.upper) = v; return Outcome::done(); }
    if (n >= spr::GQR0 && n < spr::GQR0 + 8U) { s.gqr[n - spr::GQR0] = v; return Outcome::done(); }
    if (n >= spr::SPRG0 && n <= spr::SPRG3) { s.sprg[n - spr::SPRG0] = v; return Outcome::done(); }
    switch (n) {
    case spr::XER: s.xer = v & (XER_SO | XER_OV | XER_CA | XER_BYTE_COUNT); return Outcome::done(); // bits 3-24 reserved
    case spr::LR: s.lr = v; return Outcome::done();
    case spr::CTR: s.ctr = v; return Outcome::done();
    case spr::DSISR: s.dsisr = v; return Outcome::done();
    case spr::DAR: s.dar = v; return Outcome::done();
    case spr::DEC: s.dec = v; return Outcome::done();
    case spr::SDR1: s.sdr1 = v; return Outcome::done();
    case spr::SRR0: s.srr0 = v; return Outcome::done();
    case spr::SRR1: s.srr1 = v; return Outcome::done();
    case spr::EAR: s.ear = v; return Outcome::done();
    case spr::TBL_WRITE: s.tb = (s.tb & 0xFFFFFFFF00000000ULL) | v; return Outcome::done();
    case spr::TBU_WRITE: s.tb = (s.tb & 0xFFFFFFFFULL) | (static_cast<u64>(v) << 32U); return Outcome::done();
    case spr::HID0: s.hid0 = v; return Outcome::done();
    case spr::HID1: s.hid1 = v; return Outcome::done();
    case spr::HID2:
        if ((v & HID2_LCE) == 0U) locked_cache_.clear(); // disabling the locked cache discards its lines
        s.hid2 = v; return Outcome::done();
    case spr::HID4: s.hid4 = v; return Outcome::done();
    case spr::IABR:
        if ((v & 2U) != 0U) return Outcome::unsupported("IABR breakpoint enable (not implemented)");
        s.iabr = v; return Outcome::done();
    case spr::DABR:
        if ((v & 3U) != 0U) return Outcome::unsupported("DABR breakpoint enable (not implemented)");
        s.dabr = v; return Outcome::done();
    case spr::L2CR: s.l2cr = v; return Outcome::done();
    case spr::ICTC: s.ictc = v; return Outcome::done();
    case spr::THRM1: case spr::THRM2: case spr::THRM3: s.thrm[n - spr::THRM1] = v; return Outcome::done();
    case spr::WPAR: s.wpar = v; return Outcome::done();
    case spr::DMAU: s.dmau = v; return Outcome::done();
    case spr::DMAL: return locked_cache_dma(v);
    case spr::MMCR0: s.mmcr0 = v; return Outcome::done();
    case spr::MMCR1: s.mmcr1 = v; return Outcome::done();
    case spr::SIA: s.sia = v; return Outcome::done();
    case spr::PMC1: s.pmc[0] = v; return Outcome::done();
    case spr::PMC2: s.pmc[1] = v; return Outcome::done();
    case spr::PMC3: s.pmc[2] = v; return Outcome::done();
    case spr::PMC4: s.pmc[3] = v; return Outcome::done();
    case spr::PVR: return Outcome::unsupported("mtspr PVR (read-only register)");
    default: return Outcome::unsupported("mtspr of unknown, read-only or unimplemented SPR");
    }
}

Cpu::Outcome Cpu::op_system_x(u32 ins, unsigned xo, bool& handled) {
    auto& g = state_.gpr;
    const unsigned rt = f_rt(ins), rb = f_rb(ins);
    handled = true;
    switch (xo) {
    case 83: // mfmsr
        if (problem_state()) return Outcome::privileged();
        g[rt] = state_.msr; return Outcome::done();
    case 146: // mtmsr
        if (problem_state()) return Outcome::privileged();
        state_.msr = g[rt]; return Outcome::done();
    case 595: // mfsr
        if (problem_state()) return Outcome::privileged();
        g[rt] = state_.sr[(ins >> 16U) & 15U]; return Outcome::done();
    case 659: // mfsrin
        if (problem_state()) return Outcome::privileged();
        g[rt] = state_.sr[g[rb] >> 28U]; return Outcome::done();
    case 210: // mtsr
        if (problem_state()) return Outcome::privileged();
        state_.sr[(ins >> 16U) & 15U] = g[rt]; return Outcome::done();
    case 242: // mtsrin
        if (problem_state()) return Outcome::privileged();
        state_.sr[g[rb] >> 28U] = g[rt]; return Outcome::done();
    case 339: { // mfspr
        const unsigned n = f_spr(ins);
        if (spr::is_privileged(n) && problem_state()) return Outcome::privileged();
        u32 v = 0;
        Outcome o = read_spr(n, v);
        if (!o.ok()) return o;
        g[rt] = v;
        return o;
    }
    case 467: { // mtspr
        const unsigned n = f_spr(ins);
        if (spr::is_privileged(n) && problem_state()) return Outcome::privileged();
        return write_spr(n, g[rt]);
    }
    case 371: { // mftb
        const unsigned n = f_spr(ins);
        if (n == spr::TBL_READ) { g[rt] = static_cast<u32>(state_.tb); return Outcome::done(); }
        if (n == spr::TBU_READ) { g[rt] = static_cast<u32>(state_.tb >> 32U); return Outcome::done(); }
        return Outcome::unsupported("mftb with unknown TBR");
    }
    case 306: case 566: // tlbie / tlbsync: no TLB is modelled (translation walks every access)
        if (problem_state()) return Outcome::privileged();
        return Outcome::done();
    case 598: case 854: // sync / eieio: single in-order interpreter, accesses already ordered
        return Outcome::done();
    case 310: case 438:
        return Outcome::unsupported("eciwx/ecowx external control (not implemented)");
    default:
        handled = false;
        return Outcome::done();
    }
}

Cpu::Outcome Cpu::op_cache_x(u32 ins, unsigned xo, bool& handled) {
    const unsigned ra = f_ra(ins), rb = f_rb(ins);
    const u32 ea = (ra == 0U ? 0U : state_.gpr[ra]) + state_.gpr[rb];
    handled = true;
    u32 lo = 0, hi = 0, split = 0; u8 wimg = 0;
    switch (xo) {
    case 278: case 246: // dcbt / dcbtst: hints, never cause exceptions
        return Outcome::done();
    case 54: case 86: { // dcbst / dcbf: no data cache modelled; memory is always up to date
        return translate_data(ea & ~31U, 32, false, lo, hi, split, wimg);
    }
    case 470: { // dcbi (supervisor; treated as a store for protection)
        if (problem_state()) return Outcome::privileged();
        // Without a data-cache model there are no modified lines to discard.
        return translate_data(ea & ~31U, 32, true, lo, hi, split, wimg);
    }
    case 982: { // icbi
        Outcome o = translate_data(ea & ~31U, 32, false, lo, hi, split, wimg);
        if (!o.ok()) return o;
        // Translated blocks are discarded on every architecturally executed
        // instruction-cache invalidate. The external observer is still notified.
        invalidate_jit_cache();
        if (icbi_handler_) icbi_handler_(lo);
        return o;
    }
    case 1014: { // dcbz
        Outcome o = translate_data(ea & ~31U, 32, true, lo, hi, split, wimg);
        if (!o.ok()) return o;
        if ((wimg & (WIMG_W | WIMG_I)) != 0U) return Outcome::alignment(ea); // 750CL: write-through / cache-inhibited
        if (const auto it = locked_cache_.find(lo); it != locked_cache_.end()) { it->second.fill(0); return o; }
        if (!bus_.is_ram(lo, 32)) return Outcome::stop(StepResult::MemoryFault, "dcbz outside RAM", lo);
        for (u32 i = 0; i < 32U; i += 8U) (void)bus_.write64(lo + i, 0);
        return o;
    }
    default:
        handled = false;
        return Outcome::done();
    }
}

} // namespace orca::ppc
