// Orca Core — XF memory. See xf_memory.hpp for sources and rules.
#include "orca/gpu/xf_memory.hpp"

#include <bit>
#include <cstdio>

namespace orca::gpu {

bool XfMemory::load(u32 address, std::span<const u8> words_be, std::string& error) {
    const u32 count = static_cast<u32>(words_be.size() / 4U);
    char b[96];
    if (address >= xf::REGISTERS_END) {
        std::snprintf(b, sizeof b, "XF load at %04X is past the XF address space (ends at 1057)", address);
        error = b;
        return false;
    }
    const u32 end = address + count;
    const u32 stored_end = end > xf::REGISTERS_END ? xf::REGISTERS_END : end;
    for (u32 a = address; a < stored_end; ++a) {
        const u8* p = words_be.data() + (a - address) * 4U;
        mem_[a] = (static_cast<u32>(p[0]) << 24) | (static_cast<u32>(p[1]) << 16) | (static_cast<u32>(p[2]) << 8) | p[3];
    }
    if (end > xf::REGISTERS_END) {
        std::snprintf(b, sizeof b, "XF load at %04X of %u words runs past 1057", address, count);
        error = b;
        return false;
    }
    return true;
}

float XfMemory::as_float(u32 address) const { return std::bit_cast<float>(word(address)); }

} // namespace orca::gpu
