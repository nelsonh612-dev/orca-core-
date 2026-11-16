// Orca Core — PowerPC 32-bit OEA address translation (BAT + hashed page table).
//
// Implements, per the PEM chapter 7:
//   * real addressing mode (MSR[IR]/MSR[DR] = 0): EA = PA;
//   * block address translation with IBAT0-3 / DBAT0-3 (BAT has priority);
//   * segmented page translation through the 16 segment registers and the
//     hashed page table addressed by SDR1, including primary/secondary hash,
//     key/PP protection, N (no-execute) segments and R/C bit updates.
//
// There is no TLB model: every access walks BATs and, if needed, the page
// table. This is slow but architecturally exact; tlbie/tlbsync therefore
// have nothing to discard. Direct-store segments (SR[T] = 1) are reported as
// Unsupported. Broadway's four extra BAT pairs (HID4[SBE]) are not modelled.
#pragma once
#include "orca/memory/bus.hpp"
#include "orca/ppc/state.hpp"

namespace orca::ppc {

enum class AccessType : u8 { Read, Write, Fetch };

enum class TranslateStatus : u8 {
    Ok,
    PageFault,        // no BAT and no matching PTE -> DSI/ISI "not found"
    Protection,       // BAT/PTE protection violation
    NoExecute,        // instruction fetch from an N=1 segment
    DirectStore,      // SR[T]=1, not supported
    BusError,         // page table lives in unmapped physical memory
};

struct Translation {
    TranslateStatus status{TranslateStatus::Ok};
    u32 physical{0};
    u8 wimg{0}; // W I M G (bit 3 = W ... bit 0 = G)
};

// Real-mode WIMG for data accesses (PEM 7.2: 0b0011, coherent + guarded)
// and instruction fetches (0b0001).
inline constexpr u8 kRealModeDataWimg = 0x3U;
inline constexpr u8 kRealModeFetchWimg = 0x1U;
inline constexpr u8 WIMG_W = 0x8U, WIMG_I = 0x4U, WIMG_M = 0x2U, WIMG_G = 0x1U;

// Translates an effective address. May write the page table (R/C bits)
// through `bus` when a PTE is used.
Translation translate(CpuState& state, mem::Bus& bus, u32 ea, AccessType type);

} // namespace orca::ppc
