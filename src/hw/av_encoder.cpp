#include "orca/hw/av_encoder.hpp"

namespace orca::hw {

void AvEncoder::update(bool scl, bool sda) {
    const bool old_scl = scl_, old_sda = sda_;
    scl_ = scl;
    sda_ = sda;
    if (old_scl && scl) {
        if (old_sda && !sda) { // start (or repeated start)
            phase_ = Phase::Address;
            bit_ = 0;
            byte_ = 0;
            drive_low_ = false;
            clocked_ = false; // the master pulls SCL low after a start: not a clock
        } else if (!old_sda && sda) { // stop
            phase_ = Phase::Idle;
            drive_low_ = false;
        }
        return;
    }
    if (phase_ == Phase::Idle || phase_ == Phase::Ignore) return;
    if (!old_scl && scl) { // rising edge: the receiver samples SDA
        clocked_ = true;
        const bool line = sda_line(sda);
        if (bit_ < 8U) {
            if (phase_ != Phase::Read) byte_ = static_cast<u8>((u32{byte_} << 1) | (line ? 1U : 0U));
        } else if (phase_ == Phase::Read) {
            nacked_ = line; // the master acknowledges a byte it read by pulling SDA low
        }
        return;
    }
    if (!(old_scl && !scl) || !clocked_) return; // falling edge: the transmitter sets up the next bit
    clocked_ = false;
    if (bit_ < 8U) {
        ++bit_;
        if (phase_ == Phase::Read)
            drive_low_ = bit_ < 8U && ((u32{byte_} >> (7U - bit_)) & 1U) == 0U; // released for the master's ack
        else if (bit_ == 8U)
            byte_received();
        return;
    }
    // end of the acknowledge clock
    bit_ = 0;
    drive_low_ = false;
    if (phase_ == Phase::Address) phase_ = read_next_ ? Phase::Read : Phase::Write;
    else if (phase_ == Phase::Read && nacked_) { phase_ = Phase::Ignore; return; }
    if (phase_ == Phase::Read) {
        load_read_byte();
        drive_low_ = (byte_ & 0x80U) == 0U;
    } else {
        byte_ = 0;
    }
}

void AvEncoder::byte_received() {
    if (phase_ == Phase::Address) {
        if ((u32{byte_} >> 1) != kAddress) { // another device's address: no acknowledge
            phase_ = Phase::Ignore;
            return;
        }
        read_next_ = (byte_ & 1U) != 0U;
        index_next_ = true;
        drive_low_ = true;
        return;
    }
    if (index_next_) {
        index_ = byte_;
        index_next_ = false;
    } else {
        regs_[index_] = byte_;
        index_ = static_cast<u8>(index_ + 1U);
        ++bytes_written_;
    }
    drive_low_ = true;
}

void AvEncoder::load_read_byte() {
    byte_ = regs_[index_];
    index_ = static_cast<u8>(index_ + 1U);
    ++bytes_read_;
}

} // namespace orca::hw
