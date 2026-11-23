#include "orca/wiimote/wiimote.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <cstdio>

namespace orca::wiimote {

namespace {
constexpr u8 kSpeakerAddr = 0x51, kExtensionAddr = 0x52, kCameraAddr = 0x58, kEepromI2cAddr = 0x50;
constexpr u8 kErrBusy = 4, kErrSpace = 6, kErrNack = 7, kErrAddress = 8;

// Calibration blocks end with checksum bytes: 0x55 + the sum of the data, and
// each further checksum byte adds 0x55 to the previous one.
template <std::size_t N>
void checksum(std::array<u8, N>& b, std::size_t bytes) {
    u8 sum = 0x55;
    for (std::size_t i = 0; i < N - bytes; ++i) sum = static_cast<u8>(sum + b[i]);
    for (std::size_t i = N - bytes; i < N; ++i) {
        b[i] = sum;
        sum = static_cast<u8>(sum + 0x55);
    }
}

std::string hex2(u32 v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02X", v);
    return b;
}

// IR object coordinates: 10 bits each
void put_extended(u8* o, const CameraPoint& p) {
    o[0] = static_cast<u8>(p.x);
    o[1] = static_cast<u8>(p.y);
    o[2] = static_cast<u8>((p.y >> 8 & 3U) << 6 | (p.x >> 8 & 3U) << 4 | (p.size & 0xFU));
}
} // namespace

std::array<u8, 0x100> nunchuk_registers() {
    std::array<u8, 0x100> r{};
    std::array<u8, 16> cal{0x80, 0x80, 0x80, 0x00, 0xB3, 0xB3, 0xB3, 0x00, 0xE0, 0x20, 0x80, 0xE0, 0x20, 0x80, 0, 0};
    checksum(cal, 2);
    std::copy(cal.begin(), cal.end(), r.begin() + 0x20);
    const u8 id[6] = {0x00, 0x00, 0xA4, 0x20, 0x00, 0x00};
    std::copy(id, id + 6, r.begin() + 0xFA);
    return r;
}

Wiimote::Wiimote() {
    // Calibration (representative factory values; a real remote has its own):
    // IR corners and accelerometer 0 g / 1 g, each block written twice.
    constexpr u16 lx = 0x7F, ly = 0x5D, hx = 0x380, hy = 0x2A2;
    std::array<u8, 11> ir{static_cast<u8>(lx), static_cast<u8>(ly),
                          static_cast<u8>((ly & 0x300) >> 2 | (lx & 0x300) >> 4 | (ly & 0x300) >> 6 | (hx & 0x300) >> 8),
                          static_cast<u8>(hx & 0xFF), static_cast<u8>(ly), static_cast<u8>(hx & 0xFF), static_cast<u8>(hy & 0xFF),
                          static_cast<u8>((hy & 0x300) >> 2 | (hx & 0x300) >> 4 | (hy & 0x300) >> 6 | (lx & 0x300) >> 8),
                          static_cast<u8>(lx), static_cast<u8>(hy & 0xFF), 0};
    checksum(ir, 1);
    std::array<u8, 10> acc{0x80, 0x80, 0x80, 0, 0x9A, 0x9A, 0x9A, 0, 0, 0};
    checksum(acc, 1);
    std::copy(ir.begin(), ir.end(), eeprom_.begin());
    std::copy(ir.begin(), ir.end(), eeprom_.begin() + 11);
    std::copy(acc.begin(), acc.end(), eeprom_.begin() + 22);
    std::copy(acc.begin(), acc.end(), eeprom_.begin() + 32);
    reset();
}

void Wiimote::reset() {
    speaker_ = {};
    speaker_[7] = 0x01; // decoder ready for data
    camera_ = {};
    out_.clear();
    leds_ = 0;
    rumble_ = false;
    mode_ = 0x30;
    continuous_ = false;
    camera_enabled_ = speaker_enabled_ = speaker_muted_ = false;
    extension_reported_ = false; // a connected extension is announced on the first update
    extension_present_ = false;
    extension_ = {};
    read_ = {};
    last_data_.clear();
    predictor_ = 0;
    step_ = 127;
    speaker_out_.clear();
}

std::optional<std::vector<u8>> Wiimote::pop_report() {
    if (out_.empty()) return std::nullopt;
    auto r = std::move(out_.front());
    out_.pop_front();
    return r;
}

void Wiimote::ack(u8 report, u8 err) {
    out_.push_back({0x22, static_cast<u8>(buttons_ >> 8), static_cast<u8>(buttons_), report, err});
}

void Wiimote::status_report() {
    const u8 flags = static_cast<u8>((battery_ < 0x20 ? 1U : 0U) | (extension_present_ ? 2U : 0U) |
                                     (speaker_enabled_ ? 4U : 0U) | (camera_enabled_ ? 8U : 0U) | static_cast<unsigned>(leds_) << 4U);
    extension_reported_ = extension_present_;
    out_.push_back({0x20, static_cast<u8>(buttons_ >> 8), static_cast<u8>(buttons_), flags, 0, 0, battery_});
}

bool Wiimote::output_report(std::span<const u8> r) {
    if (r.size() < 2U) return fail("Wii Remote: output report " + (r.empty() ? std::string("(empty)") : hex2(r[0])) + " without payload");
    const u8 id = r[0];
    const u8 b0 = r[1];
    const auto need = [&](std::size_t n) { return r.size() >= n + 1U ? true : fail("Wii Remote: output report " + hex2(id) + " too short"); };
    rumble_ = (b0 & 1U) != 0U;
    const bool want_ack = (b0 & 2U) != 0U;
    switch (id) {
    case 0x10: return true;
    case 0x11:
        leds_ = static_cast<u8>(b0 >> 4);
        if (want_ack) ack(id, 0);
        return true;
    case 0x12: {
        if (!need(2)) return false;
        const u8 m = r[2];
        if (m == 0x3E || m == 0x3F) return fail("Wii Remote: interleaved reporting modes 0x3E/0x3F are not emulated");
        if ((m < 0x30 || m > 0x37) && m != 0x3D) return fail("Wii Remote: reporting mode " + hex2(m) + " does not exist");
        mode_ = m;
        continuous_ = (b0 & 4U) != 0U;
        last_data_.clear();
        if (want_ack) ack(id, 0);
        return true;
    }
    case 0x13:
    case 0x1A:
        if (id == 0x13) camera_enabled_ = (b0 & 4U) != 0U;
        if (want_ack) ack(id, 0);
        return true;
    case 0x14:
    case 0x19:
        (id == 0x14 ? speaker_enabled_ : speaker_muted_) = (b0 & 4U) != 0U;
        if (want_ack) ack(id, 0);
        return true;
    case 0x15:
        status_report();
        return true;
    case 0x16: {
        if (!need(21)) return false;
        const u8 space = static_cast<u8>(b0 >> 2 & 3U);
        const u8 device = static_cast<u8>(r[2] >> 1);
        const u16 address = static_cast<u16>(r[3] << 8 | r[4]);
        const u8 size = r[5];
        if (size == 0U || size > 16U) return fail("Wii Remote: write of " + std::to_string(size) + " bytes");
        u8 err = 0;
        if (space == 0U) {
            if (address + size > kEepromSize) err = kErrAddress;
            else std::copy_n(&r[6], size, eeprom_.begin() + address);
        } else if (space == 1U || space == 2U) {
            if (device == kEepromI2cAddr) err = kErrAddress;
            else if (bus_write(device, static_cast<u8>(address), size, &r[6]) != size) err = kErrNack;
            if (failed()) return false;
        } else {
            err = kErrSpace;
        }
        ack(id, err);
        return true;
    }
    case 0x17: {
        if (!need(6)) return false;
        if (read_.size != 0U) { ack(id, kErrBusy); return true; }
        read_.space = static_cast<u8>(b0 >> 2 & 3U);
        read_.device = static_cast<u8>(r[2] >> 1);
        read_.address = static_cast<u16>(r[3] << 8 | r[4]);
        read_.size = static_cast<u16>(r[5] << 8 | r[6]);
        return true;
    }
    case 0x18: {
        if (!need(21)) return false;
        const u8 len = static_cast<u8>(b0 >> 3);
        if (len > 20U) return fail("Wii Remote: speaker data of " + std::to_string(len) + " bytes");
        bus_write(kSpeakerAddr, 0, len, &r[2]);
        return !failed();
    }
    default:
        return fail("Wii Remote: output report " + hex2(id) + " is not emulated");
    }
}

int Wiimote::bus_read(u8 device, u8 address, int count, u8* out) {
    const int n = std::min(count, 0x100 - address);
    switch (device) {
    case kSpeakerAddr: std::copy_n(speaker_.begin() + address, n, out); return n;
    case kCameraAddr:
        if (!camera_enabled_) return 0;
        std::copy_n(camera_.begin() + address, n, out);
        return n;
    case kExtensionAddr:
        if (!extension_present_) return 0;
        if (extension_[0xF0] == 0xAA) { fail("Wii Remote: encrypted extension reads (0xF0 = 0xAA) are not emulated"); return 0; }
        std::copy_n(extension_.begin() + address, n, out);
        return n;
    default: return 0;
    }
}

int Wiimote::bus_write(u8 device, u8 address, int count, const u8* in) {
    const int n = std::min(count, 0x100 - address);
    switch (device) {
    case kSpeakerAddr: {
        if (!speaker_enabled_) return count; // accepted and ignored
        if (address == 0U) { speaker_data(std::span<const u8>(in, static_cast<std::size_t>(count))); return count; }
        // registers: 1 flags, 2 format, 3-4 rate divisor (little-endian), 5 volume,
        // 7 decoder flags (bits 2/3 write 1 to clear, bit 1 resets the decoder),
        // 8 playback enable, 9 input enable
        const u8 before = speaker_[7];
        std::copy_n(in, n, speaker_.begin() + address);
        const u8 written = (address <= 7 && address + n > 7) ? speaker_[7] : 0;
        speaker_[7] = static_cast<u8>(before & ~(written & 0x0CU));
        if ((written & 0x02U) != 0U) { predictor_ = 0; step_ = 127; }
        if ((speaker_[1] & 0x80U) != 0U) speaker_[8] &= 0xFEU; // playback disabled
        return count;
    }
    case kCameraAddr:
        if (!camera_enabled_) return 0;
        std::copy_n(in, n, camera_.begin() + address);
        return n;
    case kExtensionAddr:
        if (!extension_present_) return 0;
        std::copy_n(in, n, extension_.begin() + address);
        return n;
    default: return 0;
    }
}

void Wiimote::speaker_data(std::span<const u8> data) {
    if ((speaker_[1] & 0x01U) != 0U || (speaker_[9] & 0x01U) == 0U) return; // stopped / input off
    static constexpr s32 diff[16] = {1, 3, 5, 7, 9, 11, 13, 15, -1, -3, -5, -7, -9, -11, -13, -15};
    static constexpr s32 scale[16] = {230, 230, 230, 230, 307, 409, 512, 614, 230, 230, 230, 230, 307, 409, 512, 614};
    std::vector<s16> samples;
    const u8 format = speaker_[2] & 0xE0U;
    const auto nibble = [&](u8 v) {
        predictor_ = std::clamp(predictor_ + step_ * diff[v] / 8, -32768, 32767);
        step_ = std::clamp((step_ * scale[v]) >> 8, 127, 24576);
        samples.push_back(static_cast<s16>(predictor_));
    };
    switch (format) {
    case 0x00: for (u8 v : data) { nibble(v >> 4); nibble(v & 0xFU); } break;
    case 0x40: for (u8 v : data) samples.push_back(static_cast<s16>(static_cast<s8>(v) * 0x100)); break;
    default:
        fail("Wii Remote: speaker format " + hex2(speaker_[2]) + " is not emulated");
        return;
    }
    speaker_[7] |= 0x08U; // processed data
    if ((speaker_[8] & 0x01U) == 0U || speaker_muted_ || (speaker_[2] & 0x01U) != 0U) return;
    const s32 volume = speaker_[5];
    for (s16 s : samples) speaker_out_.push_back(static_cast<s16>(std::clamp(s * volume / 0x7F, -32768, 32767)));
}

std::vector<s16> Wiimote::take_speaker_samples() { return std::exchange(speaker_out_, {}); }

u32 Wiimote::speaker_sample_rate() const noexcept {
    u32 div = static_cast<u32>(speaker_[3] | speaker_[4] << 8);
    if (div == 0U) div = 0xFFFF;
    return 12000000U / div;
}

bool Wiimote::read_step() {
    if (read_.size == 0U) return false;
    const u16 n = std::min<u16>(16, read_.size);
    std::array<u8, 16> data{};
    u8 err = 0;
    if (read_.space == 0U) {
        if (read_.address + read_.size > kEepromSize) err = kErrAddress;
        else std::copy_n(eeprom_.begin() + read_.address, n, data.begin());
    } else if (read_.space == 1U || read_.space == 2U) {
        if (read_.device == kEepromI2cAddr) err = kErrAddress;
        else if (bus_read(read_.device, static_cast<u8>(read_.address), n, data.data()) != n) err = kErrNack;
    } else {
        err = kErrSpace;
    }
    std::vector<u8> rep{0x21, static_cast<u8>(buttons_ >> 8), static_cast<u8>(buttons_),
                        static_cast<u8>((err != 0U ? 0xFU : n - 1U) << 4 | err),
                        static_cast<u8>(read_.address >> 8), static_cast<u8>(read_.address)};
    rep.insert(rep.end(), data.begin(), data.end());
    out_.push_back(std::move(rep));
    if (err != 0U) read_.size = 0;
    else { read_.address = static_cast<u16>(read_.address + n); read_.size = static_cast<u16>(read_.size - n); }
    return true;
}

void Wiimote::camera_data(const InputState& in, u8* out, int count) const {
    std::fill_n(out, count, u8{0xFF});
    if (!camera_enabled_ || camera_[0x30] != 0x08U) return;
    std::array<u8, 36> d;
    d.fill(0xFF);
    switch (camera_[0x33]) {
    case 1: // basic: two objects in 5 bytes
        for (int i = 0; i < 2; ++i) {
            const CameraPoint& a = in.points[static_cast<std::size_t>(2 * i)];
            const CameraPoint& b = in.points[static_cast<std::size_t>(2 * i + 1)];
            const u16 ax = a.visible() ? a.x : 0x3FF, ay = a.visible() ? a.y : 0x3FF;
            const u16 bx = b.visible() ? b.x : 0x3FF, by = b.visible() ? b.y : 0x3FF;
            u8* o = &d[static_cast<std::size_t>(5 * i)];
            o[0] = static_cast<u8>(ax);
            o[1] = static_cast<u8>(ay);
            o[2] = static_cast<u8>((ay >> 8 & 3U) << 6 | (ax >> 8 & 3U) << 4 | (by >> 8 & 3U) << 2 | (bx >> 8 & 3U));
            o[3] = static_cast<u8>(bx);
            o[4] = static_cast<u8>(by);
        }
        break;
    case 3: // extended: 3 bytes per object
        for (std::size_t i = 0; i < 4; ++i)
            if (in.points[i].visible()) put_extended(&d[3 * i], in.points[i]);
        break;
    case 5: // full: 9 bytes per object (extended + bounding box + intensity)
        for (std::size_t i = 0; i < 4; ++i) {
            const CameraPoint& p = in.points[i];
            if (!p.visible()) continue;
            u8* o = &d[9 * i];
            put_extended(o, p);
            const int xmin = std::max(p.x - p.size, 0), ymin = std::max(p.y - p.size, 0);
            const int xmax = std::min(p.x + p.size, int{kCameraWidth}), ymax = std::min(p.y + p.size, int{kCameraHeight});
            o[3] = static_cast<u8>(xmin >> 3 & 0x7F);
            o[4] = static_cast<u8>(ymin >> 3 & 0x7F);
            o[5] = static_cast<u8>(xmax >> 3 & 0x7F);
            o[6] = static_cast<u8>(ymax >> 3 & 0x7F);
            o[7] = 0;
            // intensity grows with the spot's area (Dolphin's model: area / 64 * pi / 4)
            const long intensity = std::lround((xmax - xmin) * (ymax - ymin) / 8 / 8 * 0.7853981633974483);
            o[8] = static_cast<u8>(std::min(255L, intensity));
        }
        break;
    default:
        return; // no data in an unknown mode: 0xFF as when disabled
    }
    std::copy_n(d.begin(), std::min(count, 36), out);
}

void Wiimote::data_report(const InputState& in) {
    if (mode_ == 0U) return; // disabled after an extension change
    struct Layout { bool core; bool accel; int ir; int ext; };
    Layout l{};
    switch (mode_) {
    case 0x30: l = {true, false, 0, 0}; break;
    case 0x31: l = {true, true, 0, 0}; break;
    case 0x32: l = {true, false, 0, 8}; break;
    case 0x33: l = {true, true, 12, 0}; break;
    case 0x34: l = {true, false, 0, 19}; break;
    case 0x35: l = {true, true, 0, 16}; break;
    case 0x36: l = {true, false, 10, 9}; break;
    case 0x37: l = {true, true, 10, 6}; break;
    default: l = {false, false, 0, 21}; break; // 0x3D
    }
    std::vector<u8> rep{mode_};
    if (l.core) {
        u16 b = buttons_;
        if (l.accel) b = static_cast<u16>(b | (in.accel_x & 3U) << 13 | (in.accel_y >> 1 & 1U) << 5 | (in.accel_z >> 1 & 1U) << 6);
        rep.push_back(static_cast<u8>(b >> 8));
        rep.push_back(static_cast<u8>(b));
    }
    if (l.accel) {
        rep.push_back(static_cast<u8>(in.accel_x >> 2));
        rep.push_back(static_cast<u8>(in.accel_y >> 2));
        rep.push_back(static_cast<u8>(in.accel_z >> 2));
    }
    if (l.ir != 0) {
        const std::size_t at = rep.size();
        rep.resize(at + static_cast<std::size_t>(l.ir));
        camera_data(in, &rep[at], l.ir);
    }
    if (l.ext != 0) {
        const std::size_t at = rep.size();
        rep.resize(at + static_cast<std::size_t>(l.ext), 0xFF);
        if (bus_read(kExtensionAddr, 0, l.ext, &rep[at]) != l.ext) std::fill(rep.begin() + static_cast<std::ptrdiff_t>(at), rep.end(), u8{0xFF});
    }
    if (!continuous_ && rep == last_data_) return; // only changes are reported
    last_data_ = rep;
    out_.push_back(std::move(rep));
}

void Wiimote::update(const InputState& in) {
    if (failed()) return;
    buttons_ = in.buttons & button::kMask;
    battery_ = in.battery;
    const bool present = in.nunchuk.has_value();
    if (present != extension_present_) {
        extension_present_ = present;
        extension_ = present ? nunchuk_registers() : std::array<u8, 0x100>{};
    }
    if (present) {
        const NunchukState& n = *in.nunchuk;
        extension_[0] = n.stick_x;
        extension_[1] = n.stick_y;
        extension_[2] = static_cast<u8>(n.accel_x >> 2);
        extension_[3] = static_cast<u8>(n.accel_y >> 2);
        extension_[4] = static_cast<u8>(n.accel_z >> 2);
        extension_[5] = static_cast<u8>((n.accel_z & 3U) << 6 | (n.accel_y & 3U) << 4 | (n.accel_x & 3U) << 2 |
                                        (n.c ? 0U : 2U) | (n.z ? 0U : 1U));
    }
    if (extension_present_ != extension_reported_) {
        mode_ = 0; // reporting stops until the console sets a mode again
        status_report();
        return;
    }
    if (read_step()) return;
    data_report(in);
}

} // namespace orca::wiimote
