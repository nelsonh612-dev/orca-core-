// Orca Core — GameCube memory card (EXI channel 0 or 1, CS0).
//
// Sources: gc-memcard-adapter (J. Chamberlain, MIT; captures of a real card
// bus) for command codes and the 4-byte address packing; Dolphin's
// EXI_DeviceMemoryCard.cpp (read as information only, no code copied) for the
// command semantics, status bits, EXI ID and timing. Implemented commands:
//   0x00 EXI ID      : byte 1 = 0x80, then the size in Mbit as a big-endian u32
//   0x52 read array  : AD1 AD2 AD3 BA; address = AD1<<17 | AD2<<9 | (AD3&3)<<7 | (BA&0x7F);
//                      bytes from position 2 return card data; from position 9 the
//                      address advances inside its 512-byte sector
//   0xF1 sector erase: AD1 AD2; on CS release the 8 KiB block is set to 0xFF, BUSY
//   0xF2 page program: AD1 AD2 AD3 BA + data (128-byte buffer); written on CS release
//   0x83 read status : status byte
//   0x89 clear status: clears the erase/program error bits, sets READY
//   0x81 set interrupt: next byte enables (non-zero) the card's interrupt line
// Status: BUSY 0x80, UNLOCKED 0x40, SLEEP 0x20, ERASE ERROR 0x10, PROGRAM ERROR
// 0x08, READY 0x01. The card starts BUSY | UNLOCKED | READY like Dolphin's,
// i.e. the official unlock handshake is not emulated. Erase/program finish
// 5000 CPU cycles after CS release (READY, not BUSY, interrupt if enabled).
// The interrupt is modelled as an edge: each completed command sets EXIINT
// once (Dolphin polls a level that 0x89 clears; not modelled).
// DMA reads/writes act on the address set up by the command, as one block;
// TCINT comes after size * (CPU Hz / 524288) cycles for reads and
// size * (CPU Hz / 98432) for writes (Dolphin's transfer rates).
// Any other command, a read before a command byte or an access outside the card
// is a device error. A blank card is all 0xFF (games offer to format it).
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/external_interface.hpp"

#include <array>
#include <functional>
#include <memory>
#include <vector>

namespace orca::hw {

namespace memcard {
inline constexpr u8 STATUS_BUSY = 0x80, STATUS_UNLOCKED = 0x40, STATUS_SLEEP = 0x20, STATUS_ERASE_ERROR = 0x10,
                    STATUS_PROGRAM_ERROR = 0x08, STATUS_READY = 0x01;
inline constexpr u32 kBytesPerMbit = 128U * 1024U;
inline constexpr u32 kSectorSize = 0x2000U;
inline constexpr u64 kCommandDoneCycles = 5000;
constexpr bool valid_size(u32 mbit) noexcept {
    return mbit == 4 || mbit == 8 || mbit == 16 || mbit == 32 || mbit == 64 || mbit == 128;
}
} // namespace memcard

class MemoryCard final : public ExiDevice {
public:
    // `image` empty = blank card; otherwise it must be mbit * 128 KiB long.
    static std::unique_ptr<MemoryCard> create(core::Scheduler& scheduler, u64 cpu_hz, u32 size_mbit,
                                              std::vector<u8> image, std::string& error);

    void set_interrupt_callback(std::function<void()> cb) { on_interrupt_ = std::move(cb); }
    const std::vector<u8>& image() const noexcept { return data_; }
    bool dirty() const noexcept { return dirty_; }
    u8 status() const noexcept { return status_; }
    u32 size_mbit() const noexcept { return mbit_; }

    const char* exi_name() const noexcept override { return "memory card"; }
    bool external() const noexcept override { return true; }
    void select() override { position_ = 0; }
    void deselect() override;
    bool write(std::span<const u8> bytes, std::string& error) override;
    bool read(std::span<u8> bytes, std::string& error) override;
    bool dma_read(std::span<u8> bytes, std::string& error) override;
    bool dma_write(std::span<const u8> bytes, std::string& error) override;
    u64 dma_cycles(bool is_write, u32 bytes, u64 clock_cycles) const override;

private:
    MemoryCard(core::Scheduler& scheduler, u64 cpu_hz, u32 mbit, std::vector<u8> image);
    bool transfer(u8 in, u8& out, std::string& error);
    void command_done();
    u32 mask() const noexcept { return static_cast<u32>(data_.size()) - 1U; }

    core::Scheduler& scheduler_;
    u64 cpu_hz_;
    u32 mbit_;
    std::vector<u8> data_;
    bool dirty_{false};
    std::function<void()> on_interrupt_;
    u8 command_{0};
    u8 status_{memcard::STATUS_BUSY | memcard::STATUS_UNLOCKED | memcard::STATUS_READY};
    u32 position_{0};
    u32 address_{0};
    u8 interrupt_switch_{0};
    std::array<u8, 128> program_buffer_{};
};

} // namespace orca::hw
