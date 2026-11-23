// Orca Core — PowerPC 750CL (Gekko/Broadway) register numbers and bit masks.
//
// Sources: PowerPC Microprocessor Family: The Programming Environments for
// 32-Bit Microprocessors (PEM) and the IBM PowerPC 750CL RISC Microprocessor
// User's Manual. Masks are written for host (LSB = bit 0) numbering; the
// comment gives the IBM bit number used in the manuals.
#pragma once
#include "orca/core/bits.hpp"

namespace orca::ppc {

// --- XER -----------------------------------------------------------------
inline constexpr u32 XER_SO = 0x80000000U; // IBM bit 0
inline constexpr u32 XER_OV = 0x40000000U; // IBM bit 1
inline constexpr u32 XER_CA = 0x20000000U; // IBM bit 2
inline constexpr u32 XER_BYTE_COUNT = 0x0000007FU; // IBM bits 25-31 (lswx/stswx)

// --- MSR -----------------------------------------------------------------
inline constexpr u32 MSR_POW = 0x00040000U; // 13 power management enable
inline constexpr u32 MSR_ILE = 0x00010000U; // 15 exception little-endian mode
inline constexpr u32 MSR_EE  = 0x00008000U; // 16 external interrupt enable
inline constexpr u32 MSR_PR  = 0x00004000U; // 17 problem (user) state
inline constexpr u32 MSR_FP  = 0x00002000U; // 18 floating-point available
inline constexpr u32 MSR_ME  = 0x00001000U; // 19 machine check enable
inline constexpr u32 MSR_FE0 = 0x00000800U; // 20
inline constexpr u32 MSR_SE  = 0x00000400U; // 21 single-step trace
inline constexpr u32 MSR_BE  = 0x00000200U; // 22 branch trace
inline constexpr u32 MSR_FE1 = 0x00000100U; // 23
inline constexpr u32 MSR_IP  = 0x00000040U; // 25 exception prefix (0xFFF00000)
inline constexpr u32 MSR_IR  = 0x00000020U; // 26 instruction address translation
inline constexpr u32 MSR_DR  = 0x00000010U; // 27 data address translation
inline constexpr u32 MSR_PM  = 0x00000004U; // 29 performance monitor mark (750)
inline constexpr u32 MSR_RI  = 0x00000002U; // 30 recoverable exception
inline constexpr u32 MSR_LE  = 0x00000001U; // 31 little-endian mode

// MSR bits saved into SRR1 on exception and restored by rfi
// (IBM bits 16-23, 25-27, 30-31).
inline constexpr u32 MSR_SAVE_MASK = 0x0000FF73U;

// --- HID2 (Gekko/Broadway) ----------------------------------------------
inline constexpr u32 HID2_LSQE = 0x80000000U; // 0 paired-single quantized load/store enable
inline constexpr u32 HID2_WPE  = 0x40000000U; // 1 write-gather pipe enable
inline constexpr u32 HID2_PSE  = 0x20000000U; // 2 paired-single enable
inline constexpr u32 HID2_LCE  = 0x10000000U; // 3 locked cache enable

// --- SRR1 exception-specific bits ---------------------------------------
inline constexpr u32 SRR1_PROGRAM_FP         = 0x00100000U; // 11
inline constexpr u32 SRR1_PROGRAM_ILLEGAL    = 0x00080000U; // 12
inline constexpr u32 SRR1_PROGRAM_PRIVILEGED = 0x00040000U; // 13
inline constexpr u32 SRR1_PROGRAM_TRAP       = 0x00020000U; // 14
inline constexpr u32 SRR1_ISI_NOT_FOUND      = 0x40000000U; // 1
inline constexpr u32 SRR1_ISI_NO_EXECUTE     = 0x10000000U; // 3 (N bit / direct-store)
inline constexpr u32 SRR1_ISI_PROTECTION     = 0x08000000U; // 4

// --- DSISR bits for DSI ---------------------------------------------------
inline constexpr u32 DSISR_NOT_FOUND    = 0x40000000U; // 1
inline constexpr u32 DSISR_PROTECTION   = 0x08000000U; // 4
inline constexpr u32 DSISR_STORE        = 0x02000000U; // 6
inline constexpr u32 DSISR_EAR_DISABLED = 0x00100000U; // 11: eciwx/ecowx with EAR[E] = 0

// --- SPR numbers (as used by mfspr/mtspr, after field swap) ---------------
namespace spr {
inline constexpr unsigned XER = 1, LR = 8, CTR = 9;
inline constexpr unsigned DSISR = 18, DAR = 19, DEC = 22, SDR1 = 25, SRR0 = 26, SRR1 = 27;
inline constexpr unsigned TBL_READ = 268, TBU_READ = 269; // mftb only
inline constexpr unsigned SPRG0 = 272, SPRG1 = 273, SPRG2 = 274, SPRG3 = 275;
inline constexpr unsigned EAR = 282, TBL_WRITE = 284, TBU_WRITE = 285, PVR = 287;
inline constexpr unsigned IBAT0U = 528; // IBAT0U..IBAT3L = 528..535
inline constexpr unsigned DBAT0U = 536; // DBAT0U..DBAT3L = 536..543
inline constexpr unsigned IBAT4U = 560; // Broadway: IBAT4U..IBAT7L = 560..567
inline constexpr unsigned DBAT4U = 568; // Broadway: DBAT4U..DBAT7L = 568..575
inline constexpr unsigned GQR0 = 912;   // GQR0..GQR7 = 912..919 (Gekko paired-single quantisation)
inline constexpr unsigned HID2 = 920, WPAR = 921, DMAU = 922, DMAL = 923;
inline constexpr unsigned UMMCR0 = 936, UPMC1 = 937, UPMC2 = 938, USIA = 939;
inline constexpr unsigned UMMCR1 = 940, UPMC3 = 941, UPMC4 = 942;
inline constexpr unsigned MMCR0 = 952, PMC1 = 953, PMC2 = 954, SIA = 955;
inline constexpr unsigned MMCR1 = 956, PMC3 = 957, PMC4 = 958;
inline constexpr unsigned HID0 = 1008, HID1 = 1009, IABR = 1010, HID4 = 1011, DABR = 1013;
inline constexpr unsigned L2CR = 1017, ICTC = 1019, THRM1 = 1020, THRM2 = 1021, THRM3 = 1022;

// SPRs whose number has bit 0x10 set are supervisor-only (PEM 8.x, mtspr/mfspr).
constexpr bool is_privileged(unsigned n) noexcept { return (n & 0x10U) != 0U; }
} // namespace spr

// --- Exception vectors (offsets from 0x00000000 or 0xFFF00000) -----------
enum class Exception : u32 {
    None = 0,
    SystemReset = 0x100,
    MachineCheck = 0x200,
    DSI = 0x300,
    ISI = 0x400,
    External = 0x500,
    Alignment = 0x600,
    Program = 0x700,
    FloatingPointUnavailable = 0x800,
    Decrementer = 0x900,
    SystemCall = 0xC00,
    Trace = 0xD00,
    PerformanceMonitor = 0xF00,
};

} // namespace orca::ppc
