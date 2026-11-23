// Orca Core — routes one 4 KiB MMIO page to several devices.
// On GameCube/Wii the page at 0x0C006000 holds DI (0x000), SI (0x400),
// EXI (0x800) and AI (0xC00); the bus maps at 4 KiB granularity.
// Sub-ranges with no device report a device error (clear CPU stop).
#pragma once
#include "orca/memory/bus.hpp"

#include <vector>

namespace orca::hw {

class MmioSplitter final : public mem::MmioDevice {
public:
    // Attaches `device` at [offset, offset + size); the device sees offsets
    // relative to its own window. Overlaps are rejected.
    bool attach(u32 offset, u32 size, mem::MmioDevice& device);
    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return last_ ? last_->mmio_name() : "MmioSplitter"; }
    std::string mmio_error() const override { return last_ ? last_->mmio_error() : "no device at this offset of the page"; }

private:
    struct Slot { u32 offset; u32 size; mem::MmioDevice* device; };
    Slot* find(u32 offset, unsigned size);
    std::vector<Slot> slots_;
    mem::MmioDevice* last_{nullptr}; // device addressed by the most recent access
};

} // namespace orca::hw
