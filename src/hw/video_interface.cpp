#include "orca/hw/video_interface.hpp"

namespace orca::hw {

VideoInterface::VideoInterface(core::Scheduler& scheduler, ProcessorInterface& pi, u64 cpu_hz)
    : scheduler_(scheduler), pi_(pi), cpu_hz_(cpu_hz) {}

VideoTiming VideoInterface::timing() const {
    VideoTiming t;
    t.half_line_pixels = reg32(vi::HTR0) & 0x1FFU;
    const u32 vtr = reg(vi::VTR);
    const u32 acv = (vtr >> 4U) & 0x3FFU, equ = vtr & 0xFU;
    const u32 vto = reg32(vi::VTO), vte = reg32(vi::VTE);
    t.odd_half_lines = 3U * equ + (vto & 0x3FFU) + 2U * acv + ((vto >> 16U) & 0x3FFU);
    t.even_half_lines = 3U * equ + (vte & 0x3FFU) + 2U * acv + ((vte >> 16U) & 0x3FFU);
    t.interlaced = (reg(vi::DCR) & vi::DCR_NIN) == 0U;
    t.pixel_hz = (reg(vi::VICLK) & 1U) != 0U ? 27000000ULL : 13500000ULL;
    t.valid = running_ && t.half_line_pixels != 0U && t.odd_half_lines != 0U &&
              (!t.interlaced || t.even_half_lines != 0U) && cpu_hz_ % t.pixel_hz == 0U;
    return t;
}

u64 VideoInterface::cycles_per_line() const {
    const VideoTiming t = timing();
    return t.valid ? cycles_for_pixels(2ULL * t.half_line_pixels) : 0U;
}

u64 VideoInterface::cycles_for_pixels(u64 pixels) const { return pixels * (cpu_hz_ / timing().pixel_hz); }

u64 VideoInterface::pixels_since_start() const {
    const VideoTiming t = timing();
    if (!t.valid) return 0;
    return (scheduler_.now() - start_cycle_) / (cpu_hz_ / t.pixel_hz);
}

namespace {
u64 frame_pixels(const VideoTiming& t) {
    return static_cast<u64>(t.interlaced ? t.odd_half_lines + t.even_half_lines : t.odd_half_lines) * t.half_line_pixels;
}
} // namespace

std::pair<u32, u32> VideoInterface::position() const {
    const VideoTiming t = timing();
    if (!t.valid) return {0, 0};
    const u64 p = pixels_since_start() % frame_pixels(t);
    const u64 hl = p / t.half_line_pixels, px = p % t.half_line_pixels;
    return {static_cast<u32>(hl / 2U + 1U), static_cast<u32>((hl % 2U) * t.half_line_pixels + px + 1U)};
}

bool VideoInterface::bottom_field() const {
    const VideoTiming t = timing();
    if (!t.valid || !t.interlaced) return false;
    const u64 hl = (pixels_since_start() % frame_pixels(t)) / t.half_line_pixels;
    return hl >= t.odd_half_lines;
}

u32 VideoInterface::xfb_address(bool bottom) const {
    const u32 fbb = reg32(bottom ? vi::BFBL : vi::TFBL) & 0x00FFFFFFU;
    // PGOFF (address in 32-byte units): the bottom field uses the top register's bit
    return (reg32(vi::TFBL) & 0x10000000U) != 0U ? (fbb << 5U) : fbb;
}

XfbField VideoInterface::xfb_field(bool bottom) const {
    const u16 pic = reg(vi::PICCONF);
    XfbField f;
    f.address = xfb_address(bottom);
    f.width = ((pic >> 8U) & 0x7FU) * 16U;
    f.stride = (pic & 0xFFU) * 32U;
    f.height = (reg(vi::VTR) >> 4U) & 0x3FFU;
    return f;
}

void VideoInterface::update_interrupt() {
    bool any = false;
    for (u32 n = 0; n < 4; ++n) {
        const u32 di = reg32(vi::DI0 + n * 4U);
        any = any || ((di & vi::DI_INT) != 0U && (di & vi::DI_ENB) != 0U);
    }
    if (any) pi_.raise(pi::CAUSE_VI); else pi_.lower(pi::CAUSE_VI);
}

void VideoInterface::restart_timing() {
    running_ = true;
    fresh_start_ = true;
    start_cycle_ = scheduler_.now();
}

void VideoInterface::reschedule() {
    for (auto& id : di_events_) { if (id != 0U) scheduler_.cancel(id); id = 0; }
    if (field_event_ != 0U) { scheduler_.cancel(field_event_); field_event_ = 0; }
    const VideoTiming t = timing();
    if (!t.valid) return;
    const u64 frame = frame_pixels(t);
    const u64 cur = pixels_since_start();
    // Only before anything has fired since the restart may an event be due at the
    // current pixel; afterwards "now" counts as already passed (no re-firing loop).
    const bool at_start = fresh_start_ && scheduler_.now() == start_cycle_;
    // Next absolute pixel index (from timing start) at which frame position `p` occurs.
    auto next_at = [&](u64 p) {
        u64 cand = cur - cur % frame + p;
        if (cand < cur || (cand == cur && !at_start)) cand += frame;
        return cand;
    };
    for (u32 n = 0; n < 4; ++n) {
        const u32 di = reg32(vi::DI0 + n * 4U);
        if ((di & vi::DI_ENB) == 0U) continue;
        const u32 vct = (di >> 16U) & 0x3FFU, hct = di & 0x3FFU;
        if (vct == 0U || hct == 0U || hct > 2U * t.half_line_pixels) continue;
        const u64 p = (static_cast<u64>(vct - 1U) * 2U + ((hct - 1U) >= t.half_line_pixels ? 1U : 0U)) * t.half_line_pixels +
                      (hct - 1U) % t.half_line_pixels;
        if (p >= frame) continue; // position never reached in this mode
        const u64 when = start_cycle_ + cycles_for_pixels(next_at(p));
        di_events_[n] = scheduler_.schedule_at(when, "VI DI", [this, n](u64, u64) {
            di_events_[n] = 0;
            fresh_start_ = false;
            const u32 off = vi::DI0 + n * 4U;
            regs_[off / 2U] = static_cast<u16>(regs_[off / 2U] | 0x8000U); // INT
            update_interrupt();
            reschedule();
        });
    }
    // Field starts: half line 0 (top/odd) and, when interlaced, half line `odd` (bottom/even).
    const u64 top = 0, bottom = static_cast<u64>(t.odd_half_lines) * t.half_line_pixels;
    u64 next = next_at(top);
    bool next_bottom = false;
    if (t.interlaced && next_at(bottom) < next) { next = next_at(bottom); next_bottom = true; }
    field_event_ = scheduler_.schedule_at(start_cycle_ + cycles_for_pixels(next), "VI field", [this, next_bottom](u64, u64) {
        field_event_ = 0;
        fresh_start_ = false;
        ++fields_;
        if (field_listener_) field_listener_(next_bottom);
        reschedule();
    });
}

u16 VideoInterface::read16(u32 off) const {
    if (off == vi::DPV) return static_cast<u16>(position().first);
    if (off == vi::DPH) return static_cast<u16>(position().second);
    return reg(off);
}

bool VideoInterface::write16(u32 off, u16 v) {
    if (off >= 0x80U) return fail("VI: register offset beyond 0x7F not implemented");
    if (off == vi::DPV || off == vi::DPH) return true; // read-only position counters: writes ignored
    u16& r = regs_[off / 2U];
    if (off >= vi::DI0 && off < vi::DI0 + 16U) {
        if ((off & 2U) == 0U) {
            const u16 int_bit = static_cast<u16>(r & v & 0x8000U); // INT can only be cleared by software
            r = static_cast<u16>((v & 0x7FFFU) | int_bit);
        } else {
            r = v;
        }
        update_interrupt();
        reschedule();
        return true;
    }
    if (off == vi::DCR) {
        const bool was = (r & vi::DCR_ENB) != 0U;
        r = v;
        if ((v & vi::DCR_RST) != 0U || (v & vi::DCR_ENB) == 0U) running_ = false;
        else if (!was || !running_) restart_timing();
        reschedule();
        return true;
    }
    r = v;
    const bool timing_reg = off == vi::VTR || off == vi::HTR0 || off == vi::HTR0 + 2U || off == vi::VTO ||
                            off == vi::VTO + 2U || off == vi::VTE || off == vi::VTE + 2U || off == vi::VICLK;
    if (timing_reg && running_) { restart_timing(); reschedule(); }
    return true;
}

bool VideoInterface::mmio_read(u32 offset, unsigned size, u64& value) {
    if (offset >= 0x80U) return fail("VI: register offset beyond 0x7F not implemented");
    if (size == 2U && (offset & 1U) == 0U) { value = read16(offset); return true; }
    if (size == 4U && (offset & 3U) == 0U) { value = (u64{read16(offset)} << 16U) | read16(offset + 2U); return true; }
    if (size == 1U) { // one byte of a 16-bit register (Dolphin's ReadToLarger<u8>)
        const u16 r = read16(offset & ~1U);
        value = (offset & 1U) != 0U ? (r & 0xFFU) : (r >> 8U);
        return true;
    }
    return fail("VI: only aligned 16/32-bit accesses and 8-bit reads are implemented");
}

bool VideoInterface::mmio_write(u32 offset, unsigned size, u64 value) {
    if (size == 2U && (offset & 1U) == 0U) return write16(offset, static_cast<u16>(value));
    if (size == 4U && (offset & 3U) == 0U)
        return write16(offset, static_cast<u16>(value >> 16U)) && write16(offset + 2U, static_cast<u16>(value));
    return fail("VI: only aligned 16/32-bit accesses are implemented");
}

} // namespace orca::hw
