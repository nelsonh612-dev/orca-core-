#include "orca/hw/mmio_splitter.hpp"

namespace orca::hw {

bool MmioSplitter::attach(u32 offset, u32 size, mem::MmioDevice& device) {
    if (size == 0 || offset + size > 0x1000U || offset + size < offset) return false;
    for (const Slot& s : slots_)
        if (offset < s.offset + s.size && s.offset < offset + size) return false;
    slots_.push_back({offset, size, &device});
    return true;
}

MmioSplitter::Slot* MmioSplitter::find(u32 offset, unsigned size) {
    for (Slot& s : slots_)
        if (offset >= s.offset && offset + size <= s.offset + s.size) return &s;
    return nullptr;
}

bool MmioSplitter::mmio_read(u32 offset, unsigned size, u64& value) {
    Slot* s = find(offset, size);
    last_ = s != nullptr ? s->device : nullptr;
    return s != nullptr && s->device->mmio_read(offset - s->offset, size, value);
}

bool MmioSplitter::mmio_write(u32 offset, unsigned size, u64 value) {
    Slot* s = find(offset, size);
    last_ = s != nullptr ? s->device : nullptr;
    return s != nullptr && s->device->mmio_write(offset - s->offset, size, value);
}

} // namespace orca::hw
