// Orca Core — physical RAM layouts of the target consoles.
//
// Only main RAM is mapped here. Hardware register windows (CP, PE, VI, PI,
// MI, DSP, DI, SI, EXI, AI, Hollywood) are attached later by their device
// modules via Bus::map_mmio(); until then accesses to them report Unmapped.
#pragma once
#include "orca/memory/bus.hpp"

namespace orca::mem {

inline constexpr u32 kMem1Base = 0x00000000U;
inline constexpr u32 kMem1Size = 24U * 1024U * 1024U; // 24 MiB 1T-SRAM (GameCube and Wii)
inline constexpr u32 kMem2Base = 0x10000000U;
inline constexpr u32 kMem2Size = 64U * 1024U * 1024U; // 64 MiB GDDR3 (Wii only)

// Maps MEM1. Returns false if the bus already has an overlapping mapping.
bool map_gamecube_ram(Bus& bus);
// Maps MEM1 and MEM2.
bool map_wii_ram(Bus& bus);

} // namespace orca::mem
