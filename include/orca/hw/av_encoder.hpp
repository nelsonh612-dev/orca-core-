#pragma once
// The Wii's A/V encoder as seen from its I2C bus. Broadway bit-bangs the bus on
// two Hollywood GPIO pins (SCL bit 14, SDA bit 15); the encoder answers at the
// 7-bit address 0x70 (0xE0 to write, 0xE1 to read). A write sends a register
// index and then data bytes for consecutive registers; a read returns registers
// from the current index on. (WiiBrew: Hardware/AV Encoder; I2C bus protocol:
// start/stop are SDA edges while SCL is high, data is sampled on the SCL rising
// edge and changed while SCL is low, the receiver pulls SDA low on the ninth
// clock to acknowledge.)
//
// What the encoder does with its registers (analog video and audio output
// settings) has no effect on the emulated picture: the register file is kept so
// that the software reads back what it wrote.
#include "orca/core/bits.hpp"

#include <array>

namespace orca::hw {

class AvEncoder {
public:
    static constexpr u8 kAddress = 0x70U;

    // The two pins as the master drives them: `scl`/`sda` are the master's line
    // levels (true = released or driven high). Call after every pin change.
    void update(bool scl, bool sda);
    // Level of the data line as an input sees it: high unless someone pulls it low.
    bool sda_line(bool master_sda) const noexcept { return master_sda && !drive_low_; }

    const std::array<u8, 256>& registers() const noexcept { return regs_; }
    u64 bytes_written() const noexcept { return bytes_written_; }
    u64 bytes_read() const noexcept { return bytes_read_; }

private:
    enum class Phase { Idle, Address, Write, Read, Ignore };
    void byte_received();
    void load_read_byte();

    bool scl_{true}, sda_{true};
    Phase phase_{Phase::Idle};
    unsigned bit_{0};    // bits of the current byte already clocked (0-8; 8 = ack clock)
    u8 byte_{0};
    bool drive_low_{false};
    bool index_next_{false}; // the next written byte is the register index
    bool nacked_{false};
    bool read_next_{false};
    bool clocked_{false};      // SCL rose since the start or the last falling edge    // the address byte asked for a read
    u8 index_{0};
    std::array<u8, 256> regs_{};
    u64 bytes_written_{0}, bytes_read_{0};
};

} // namespace orca::hw
