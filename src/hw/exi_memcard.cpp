#include "orca/hw/exi_memcard.hpp"

#include <cstdio>

namespace orca::hw {

std::unique_ptr<MemoryCard> MemoryCard::create(core::Scheduler& scheduler, u64 cpu_hz, u32 size_mbit,
                                               std::vector<u8> image, std::string& error) {
    if (!memcard::valid_size(size_mbit)) {
        error = "memory card size must be 4, 8, 16, 32, 64 or 128 Mbit";
        return nullptr;
    }
    const std::size_t bytes = static_cast<std::size_t>(size_mbit) * memcard::kBytesPerMbit;
    if (image.empty()) image.assign(bytes, 0xFF);
    if (image.size() != bytes) {
        char buf[128];
        std::snprintf(buf, sizeof buf, "memory card image is %zu bytes; a %u Mbit card needs %zu", image.size(), size_mbit,
                      bytes);
        error = buf;
        return nullptr;
    }
    return std::unique_ptr<MemoryCard>(new MemoryCard(scheduler, cpu_hz, size_mbit, std::move(image)));
}

MemoryCard::MemoryCard(core::Scheduler& scheduler, u64 cpu_hz, u32 mbit, std::vector<u8> image)
    : scheduler_(scheduler), cpu_hz_(cpu_hz), mbit_(mbit), data_(std::move(image)) {}

void MemoryCard::command_done() {
    status_ = static_cast<u8>((status_ | memcard::STATUS_READY) & ~memcard::STATUS_BUSY);
    if (interrupt_switch_ != 0U && on_interrupt_) on_interrupt_();
}

void MemoryCard::deselect() {
    if (command_ == 0xF1 && position_ > 2U) { // sector erase
        const u32 base = (address_ & mask()) & ~(memcard::kSectorSize - 1U);
        for (u32 i = 0; i < memcard::kSectorSize; ++i) data_[base + i] = 0xFF;
        dirty_ = true;
        status_ = static_cast<u8>((status_ | memcard::STATUS_BUSY) & ~memcard::STATUS_READY);
        scheduler_.schedule_in(memcard::kCommandDoneCycles, "memcard erase", [this](u64, u64) { command_done(); });
    } else if (command_ == 0xF2 && position_ >= 5U) { // page program from the buffer
        u32 count = position_ - 5U;
        u32 i = 0;
        status_ = static_cast<u8>(status_ & ~memcard::STATUS_BUSY);
        while (count-- != 0U) {
            data_[address_ & mask()] = program_buffer_[i];
            i = (i + 1U) & 127U;
            address_ = (address_ & ~0x1FFU) | ((address_ + 1U) & 0x1FFU);
            dirty_ = true;
        }
        scheduler_.schedule_in(memcard::kCommandDoneCycles, "memcard program", [this](u64, u64) { command_done(); });
    }
    position_ = 0;
}

bool MemoryCard::transfer(u8 in, u8& out, std::string& error) {
    out = 0xFF;
    if (position_ == 0U) {
        switch (in) {
        case 0x00: case 0x52: case 0xF1: case 0xF2: case 0x83: case 0x81: break;
        case 0x89: // clear status
            status_ = static_cast<u8>((status_ & ~(memcard::STATUS_PROGRAM_ERROR | memcard::STATUS_ERASE_ERROR)) |
                                      memcard::STATUS_READY);
            break;
        default: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "command %02X is not implemented", in);
            error = buf;
            return false;
        }
        }
        command_ = in;
        ++position_;
        return true;
    }
    const u32 pos = position_;
    switch (command_) {
    case 0x00: // EXI ID
        out = pos == 1U ? static_cast<u8>(0x80U) : static_cast<u8>(mbit_ >> (24U - ((pos - 2U) & 3U) * 8U));
        break;
    case 0x52: // read array
        if (pos == 1U) address_ = static_cast<u32>(in) << 17;
        else if (pos == 2U) address_ |= static_cast<u32>(in) << 9;
        else if (pos == 3U) address_ |= static_cast<u32>(in & 3U) << 7;
        else if (pos == 4U) address_ |= in & 0x7FU;
        if (pos > 1U) {
            out = data_[address_ & mask()];
            if (pos >= 9U) address_ = (address_ & ~0x1FFU) | ((address_ + 1U) & 0x1FFU);
        }
        break;
    case 0x83: out = status_; break;
    case 0xF1:
        if (pos == 1U) address_ = static_cast<u32>(in) << 17;
        else if (pos == 2U) address_ |= static_cast<u32>(in) << 9;
        break;
    case 0x81:
        if (pos == 1U) interrupt_switch_ = in;
        break;
    case 0xF2:
        if (pos == 1U) address_ = static_cast<u32>(in) << 17;
        else if (pos == 2U) address_ |= static_cast<u32>(in) << 9;
        else if (pos == 3U) address_ |= static_cast<u32>(in & 3U) << 7;
        else if (pos == 4U) address_ |= in & 0x7FU;
        else program_buffer_[(pos - 5U) & 0x7FU] = in;
        break;
    default: break; // 0x89: further bytes return 0xFF
    }
    ++position_;
    return true;
}

bool MemoryCard::write(std::span<const u8> bytes, std::string& error) {
    u8 out = 0;
    for (u8 b : bytes)
        if (!transfer(b, out, error)) return false;
    return true;
}

bool MemoryCard::read(std::span<u8> bytes, std::string& error) {
    if (position_ == 0U) {
        error = "read before a command byte";
        return false;
    }
    for (u8& b : bytes)
        if (!transfer(0x00, b, error)) return false;
    return true;
}

bool MemoryCard::dma_read(std::span<u8> bytes, std::string& error) {
    const u32 a = address_ & mask();
    if (static_cast<u64>(a) + bytes.size() > data_.size()) {
        error = "DMA read past the end of the card";
        return false;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) bytes[i] = data_[a + i];
    return true;
}

bool MemoryCard::dma_write(std::span<const u8> bytes, std::string& error) {
    const u32 a = address_ & mask();
    if (static_cast<u64>(a) + bytes.size() > data_.size()) {
        error = "DMA write past the end of the card";
        return false;
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) data_[a + i] = bytes[i];
    dirty_ = true;
    return true;
}

u64 MemoryCard::dma_cycles(bool is_write, u32 bytes, u64) const {
    return static_cast<u64>(bytes) * (cpu_hz_ / (is_write ? 98432ULL : 524288ULL));
}

} // namespace orca::hw
