// Orca Core — architectural state of a Gekko/Broadway CPU (integer + system).
//
// Each Gekko FPR holds two slots: ps0 (the classic FPR, double format) and
// ps1 (second paired-single slot). Both are kept as raw IEEE-754 double bit
// patterns so signalling NaNs and every bit survive moves and stores.
#pragma once
#include "orca/core/bits.hpp"

#include <array>

namespace orca::ppc {

struct BatPair {
    u32 upper{0}; // BEPI | BL | Vs | Vp
    u32 lower{0}; // BRPN | WIMG | PP
    bool operator==(const BatPair&) const = default;
};

struct Fpr {
    u64 ps0{0};
    u64 ps1{0};
    bool operator==(const Fpr&) const = default;
};

struct CpuState {
    std::array<u32, 32> gpr{};
    std::array<Fpr, 32> fpr{};
    u32 fpscr{0};
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
