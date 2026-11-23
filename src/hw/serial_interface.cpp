#include "orca/hw/serial_interface.hpp"

#include "orca/hw/video_interface.hpp"

#include <cstdio>

namespace orca::hw {

std::vector<u8> StandardController::status_bytes() const {
    const auto b0 = static_cast<u8>(((state.buttons >> 8U) & 0x1FU) | (need_origin ? 0x20U : 0U));
    const auto b1 = static_cast<u8>(0x80U | (state.buttons & 0x7FU));
    return {b0, b1, state.stick_x, state.stick_y, state.cstick_x, state.cstick_y, state.trigger_l, state.trigger_r};
}

std::optional<std::vector<u8>> StandardController::transfer(const std::vector<u8>& out, std::string& error) {
    if (out.empty()) { error = "SI pad: empty command"; return std::nullopt; }
    char buf[96];
    switch (out[0]) {
    case 0x00: case 0xFF:
        return std::vector<u8>{0x09, 0x00, static_cast<u8>((need_origin ? 0x20U : 0U) | ((motor & 3U) << 3U) | (analog_mode & 7U))};
    case 0x40: {
        if (out.size() < 3) { error = "SI pad: poll command shorter than 3 bytes"; return std::nullopt; }
        const u8 mode = out[1] & 7U;
        if (mode != 3U) {
            std::snprintf(buf, sizeof buf, "SI pad: analog mode %u not implemented (only mode 3)", mode);
            error = buf;
            return std::nullopt;
        }
        analog_mode = mode;
        motor = out[2] & 3U;
        return status_bytes();
    }
    case 0x41: case 0x42: {
        if (out[0] == 0x42) { origin = state; }
        need_origin = false;
        const PadState saved = state;
        state = origin;
        std::vector<u8> r = status_bytes();
        state = saved;
        r.push_back(0); r.push_back(0); // analog A/B (absent on retail controllers)
        return r;
    }
    default:
        std::snprintf(buf, sizeof buf, "SI pad: command 0x%02X not implemented", out[0]);
        error = buf;
        return std::nullopt;
    }
}

SerialInterface::SerialInterface(core::Scheduler& scheduler, ProcessorInterface& pi, const VideoInterface* vi)
    : scheduler_(scheduler), pi_(pi), vi_(vi) {}

void SerialInterface::update_interrupt() {
    const bool tc = (comcsr_ & si::TCINT) != 0U && (comcsr_ & si::TCINTMSK) != 0U;
    const bool rd = (comcsr_ & si::RDSTINT) != 0U && (comcsr_ & si::RDSTINTMSK) != 0U;
    if (tc || rd) pi_.raise(pi::CAUSE_SI); else pi_.lower(pi::CAUSE_SI);
}

bool SerialInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (size != 4U || (offset & 3U) != 0U) return fail("SI: only aligned 32-bit accesses are implemented");
    if (offset < 0x30U) {
        const unsigned c = offset / 0xCU, r = offset % 0xCU;
        if (r == 0U) { value = outbuf_[c]; return true; }
        value = r == 4U ? inbuf_h_[c] : inbuf_l_[c];
        sisr_ &= ~(si::SR_RDST << si::sisr_shift(c)); // data consumed
        bool any = false;
        for (unsigned k = 0; k < 4; ++k) any = any || (sisr_ & (si::SR_RDST << si::sisr_shift(k))) != 0U;
        if (!any) comcsr_ &= ~si::RDSTINT;
        update_interrupt();
        return true;
    }
    switch (offset) {
    case 0x30: value = sipoll_; return true;
    case 0x34: value = comcsr_; return true;
    case 0x38: value = sisr_; return true;
    case 0x3C: value = exilk_; return true;
    default: break;
    }
    if (offset >= 0x80U && offset < 0x100U) {
        value = (u64{io_[offset - 0x80U]} << 24U) | (u64{io_[offset - 0x7FU]} << 16U) | (u64{io_[offset - 0x7EU]} << 8U) | io_[offset - 0x7DU];
        return true;
    }
    return fail("SI: register not implemented");
}

bool SerialInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size != 4U || (offset & 3U) != 0U) return fail("SI: only aligned 32-bit accesses are implemented");
    const auto v = static_cast<u32>(value);
    if (offset < 0x30U) {
        const unsigned c = offset / 0xCU;
        if (offset % 0xCU != 0U) return fail("SI: SICnINBUF registers are read-only");
        outbuf_[c] = v;
        return true;
    }
    if (offset >= 0x80U && offset < 0x100U) {
        for (unsigned i = 0; i < 4; ++i) io_[offset - 0x80U + i] = static_cast<u8>(v >> (24U - i * 8U));
        return true;
    }
    switch (offset) {
    case 0x30: sipoll_ = v; schedule_poll(); return true;
    case 0x34: {
        if ((comcsr_ & si::TSTART) != 0U && (v & si::TSTART) != 0U) return fail("SI: transfer started while one is in progress");
        const u32 keep = comcsr_ & (si::TCINT | si::COMERR | si::RDSTINT);
        comcsr_ = (v & ~(si::TCINT | si::COMERR | si::RDSTINT)) | keep;
        if ((v & si::TCINT) != 0U) comcsr_ &= ~si::TCINT; // write 1 to clear
        if ((v & si::TSTART) != 0U && !start_transfer()) { comcsr_ &= ~si::TSTART; return false; }
        update_interrupt();
        return true;
    }
    case 0x38: {
        u32 w1c = 0;
        for (unsigned c = 0; c < 4; ++c) w1c |= (si::SR_NOREP | si::SR_COLL | si::SR_OVRUN | si::SR_UNRUN) << si::sisr_shift(c);
        sisr_ &= ~(v & w1c);
        if ((v & si::SISR_WR) != 0U) for (unsigned c = 0; c < 4; ++c) sisr_ &= ~(si::SR_WRST << si::sisr_shift(c));
        return true;
    }
    case 0x3C: exilk_ = v & 0x80000000U; return true;
    default: return fail("SI: register not implemented");
    }
}

bool SerialInterface::start_transfer() {
    const unsigned channel = (comcsr_ >> 1U) & 3U;
    u32 out_len = (comcsr_ >> 16U) & 0x7FU, in_len = (comcsr_ >> 8U) & 0x7FU;
    if (out_len == 0U) out_len = 128U;
    if (in_len == 0U) in_len = 128U;
    const std::vector<u8> out(io_.begin(), io_.begin() + out_len);
    std::optional<std::vector<u8>> response;
    if (devices_[channel]) {
        std::string error;
        response = devices_[channel]->transfer(out, error);
        if (!response) return fail(error);
    }
    scheduler_.schedule_in(0, "SI transfer", [this, channel, in_len, response](u64, u64) {
        const unsigned sh = si::sisr_shift(channel);
        if (!response) {
            sisr_ |= si::SR_NOREP << sh;
            comcsr_ |= si::COMERR;
        } else {
            const auto n = static_cast<u32>(std::min<std::size_t>(response->size(), in_len));
            for (u32 i = 0; i < n; ++i) io_[i] = (*response)[i];
            if (response->size() < in_len) { sisr_ |= si::SR_UNRUN << sh; comcsr_ |= si::COMERR; }
            if (response->size() > in_len) { sisr_ |= si::SR_OVRUN << sh; comcsr_ |= si::COMERR; }
        }
        comcsr_ &= ~si::TSTART;
        comcsr_ |= si::TCINT;
        update_interrupt();
    });
    return true;
}

void SerialInterface::poll() {
    for (unsigned c = 0; c < 4; ++c) {
        if ((sipoll_ & (0x80U >> c)) == 0U) continue; // EN bit for channel c
        const unsigned sh = si::sisr_shift(c);
        std::string error;
        std::optional<std::vector<u8>> r;
        if (devices_[c]) {
            const std::vector<u8> cmd{static_cast<u8>(outbuf_[c] >> 16U), static_cast<u8>(outbuf_[c] >> 8U), static_cast<u8>(outbuf_[c])};
            r = devices_[c]->transfer(cmd, error);
        }
        if (!r || r->size() < 8U) {
            inbuf_h_[c] = 0x80000000U; // ERRSTAT
            sisr_ |= si::SR_NOREP << sh;
        } else {
            const auto& b = *r;
            inbuf_h_[c] = (u32{b[0] & 0x3FU} << 24U) | (u32{b[1]} << 16U) | (u32{b[2]} << 8U) | b[3];
            inbuf_l_[c] = (u32{b[4]} << 24U) | (u32{b[5]} << 16U) | (u32{b[6]} << 8U) | b[7];
        }
        sisr_ |= si::SR_RDST << sh;
        comcsr_ |= si::RDSTINT;
    }
    update_interrupt();
}

void SerialInterface::schedule_poll() {
    if (poll_event_ != 0U) { scheduler_.cancel(poll_event_); poll_event_ = 0; }
    const u32 x_lines = (sipoll_ >> 16U) & 0x3FFU;
    if (x_lines == 0U || (sipoll_ & 0xF0U) == 0U || vi_ == nullptr) return;
    const u64 line = vi_->cycles_per_line();
    if (line == 0U) return; // video timing not running: no automatic polling
    poll_event_ = scheduler_.schedule_in(x_lines * line, "SI poll", [this](u64, u64) {
        poll_event_ = 0;
        poll();
        schedule_poll();
    });
}

} // namespace orca::hw
