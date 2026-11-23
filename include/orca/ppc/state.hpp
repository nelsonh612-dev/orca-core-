// Orca Core — architectural state of a Gekko/Broadway CPU (integer + system).
//
// Floating-point registers, FPSCR and paired-single state are not part of
// this milestone; they will be added to this struct by the FPU milestone.
#pragma once
#include "orca/core/bits.hpp"

#include <array>

namespace orca::ppc {

struct BatPair {
    u32 upper{0}; // BEPI | BL | Vs | Vp
    u32 lower{0}; // BRPN | WIMG | PP
    bool operator==(const BatPair&) const = default;
};

struct CpuState {
    std::array<u32, 32> gpr{};
    u32 pc{0};
    u32 lr{0};
    u32 ctr{0};
    u32 cr{0};
    u32 xer{0};
    u32 msr{0};

    // Exception handling
    u32 srr0{0}, srr1{0};
    u32 dar{0}, dsisr{0};
    std::array<u32, 4> sprg{};

    // Memory management
    u32 sdr1{0};
    std::array<u32, 16> sr{};
    std::array<BatPair, 4> ibat{};
    std::array<BatPair, 4> dbat{};

    // Time base and decrementer
    u64 tb{0};
    u32 dec{0};
    bool dec_pending{false};

    // Reservation for lwarx/stwcx. (physical address of the 32-byte granule)
    bool reserve{false};
    u32 reserve_address{0};

    // Implementation registers. Their *storage* is architectural; behaviour
    // that depends on specific bits (caches, locked cache, write-gather pipe,
    // performance monitor) is not emulated yet — see README "Known issues".
    u32 hid0{0}, hid1{0}, hid2{0}, hid4{0};
    u32 ear{0}, iabr{0}, dabr{0}, l2cr{0}, ictc{0};
    std::array<u32, 3> thrm{};
    std::array<u32, 8> gqr{};
    u32 wpar{0}, dmau{0}, dmal{0};
    u32 mmcr0{0}, mmcr1{0}, sia{0};
    std::array<u32, 4> pmc{};

    bool halted{false};

    bool operator==(const CpuState&) const = default;
};

} // namespace orca::ppc
