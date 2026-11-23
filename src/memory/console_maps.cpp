#include "orca/memory/console_maps.hpp"

namespace orca::mem {

bool map_gamecube_ram(Bus& bus) { return bus.map_ram("MEM1", kMem1Base, kMem1Size); }

bool map_wii_ram(Bus& bus) {
    return bus.map_ram("MEM1", kMem1Base, kMem1Size) && bus.map_ram("MEM2", kMem2Base, kMem2Size);
}

} // namespace orca::mem
