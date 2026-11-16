#include "orca/hw/processor_interface.hpp"

#include <cstring>

namespace orca::hw {

ProcessorInterface::ProcessorInterface(mem::Bus& bus, std::function<void(bool)> line)
    : bus_(bus), line_callback_(std::move(line)) {}

void ProcessorInterface::update_line() {
    const bool level = (causes_ & mask_) != 0U;
    if (level != line_level_) {
        line_level_ = level;
        if (line_callback_) line_callback_(level);
    }
}

void ProcessorInterface::raise(u32 cause) { causes_ |= cause; update_line(); }
void ProcessorInterface::lower(u32 cause) { causes_ &= ~cause; update_line(); }

bool ProcessorInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U) return false;
    switch (offset) {
    case 0x00: value = causes_; return true;
    case 0x04: value = mask_; return true;
    case 0x0C: value = fifo_base_; return true;
    case 0x10: value = fifo_end_; return true;
    case 0x14: value = fifo_wp_; return true;
    default: return false;
    }
}

bool ProcessorInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U) return false;
    const auto v = static_cast<u32>(value);
    switch (offset) {
    case 0x00: // only the reset-switch cause is acknowledged by software
        if ((v & pi::CAUSE_RSW) != 0U) lower(pi::CAUSE_RSW);
        return true;
    case 0x04: mask_ = v; update_line(); return true;
    case 0x0C: fifo_base_ = v & pi::FIFO_ADDR_MASK; return true;
    case 0x10: fifo_end_ = v & pi::FIFO_ADDR_MASK; return true;
    case 0x14: fifo_wp_ = v & (pi::FIFO_ADDR_MASK | pi::FIFO_WRAP); return true;
    default: return false;
    }
}

bool ProcessorInterface::write_fifo_burst(const std::array<u8, 32>& burst) {
    const u32 addr = fifo_wp_ & pi::FIFO_ADDR_MASK;
    if (bus_.dma_write(addr, burst) != mem::BusStatus::Ok) return false;
    u32 next = addr + 32U;
    u32 wrap = fifo_wp_ & pi::FIFO_WRAP;
    if (next >= fifo_end_) { next = fifo_base_; wrap = pi::FIFO_WRAP; }
    fifo_wp_ = next | wrap;
    if (burst_listener_) burst_listener_(addr);
    return true;
}

bool GatherPipe::mmio_write(u32, unsigned size, u64 value) {
    if (size != 1U && size != 2U && size != 4U && size != 8U) return false;
    // Validate before touching the buffer so a failing store has no side effects.
    if (fill_ + size >= 32U && !pi_.fifo_target_ok()) return false;
    for (unsigned i = 0; i < size; ++i) buffer_[fill_++] = static_cast<u8>(value >> ((size - 1U - i) * 8U));
    while (fill_ >= 32U) {
        std::array<u8, 32> burst{};
        std::memcpy(burst.data(), buffer_.data(), 32);
        if (!pi_.write_fifo_burst(burst)) return false;
        std::memmove(buffer_.data(), buffer_.data() + 32, fill_ - 32U);
        fill_ -= 32U;
    }
    return true;
}

} // namespace orca::hw
