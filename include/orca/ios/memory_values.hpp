// Orca Core — the low-memory values an IOS kernel writes for Broadway when it
// starts (Wii), per IOS version.
//
// Layout (public Wii documentation, e.g. WiiBrew "Memory map"):
//   0x3100 MEM1 size        0x3104 MEM1 simulated size   0x3108 MEM1 end
//   0x310C MEM1 arena start 0x3110 MEM1 arena end        0x3114 0xDEADBEEF
//   0x3118 MEM2 size        0x311C MEM2 simulated size   0x3120 MEM2 end
//   0x3124 MEM2 arena start 0x3128 MEM2 arena end        0x312C 0xDEADBEEF
//   0x3130 IPC buffer start 0x3134 IPC buffer end        0x3138 Hollywood revision
//   0x313C 0xDEADBEEF       0x3140 IOS version (number << 16 | revision)
//   0x3144 IOS build date   0x3148/0x314C IOS reserved range
//   0x3150/0x3154 0xDEADBEEF 0x3158 RAM vendor   0x315C boot flags 0xDE 0xAD
//   0x315E 0xBEEF           0x3160 system menu sync
//   0x28 / 0x30 / 0x34 / 0xF0: the legacy (GameCube) memory size, arena and
//   simulated size.
// The values per IOS are those observed on consoles running the last
// revision of each IOS, as recorded in Dolphin's IOS version table (used
// here as information: a table of measured values). MEM2 ends where IOS's
// own area begins; the IPC buffer is the 128 KiB just below it.
#pragma once
#include "orca/memory/bus.hpp"

#include <optional>

namespace orca::ios {

struct IosMemoryValues {
    u32 ios;          // IOS number
    u32 version;      // number << 16 | revision
    u32 date;         // build date
    u32 mem2_end;     // physical + 0x80000000 (cached address)
    u32 reserved_begin, reserved_end;
    u32 ram_vendor;
    u32 sysmenu_sync;
};

std::optional<IosMemoryValues> ios_memory_values(u32 ios);

// Clears 0x0000-0x3FFF and writes the values above for `ios`.
bool write_ios_memory_values(mem::Bus& bus, const IosMemoryValues& v);

} // namespace orca::ios
