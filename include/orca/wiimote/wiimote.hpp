// Orca Core — the Wii Remote as a HID device: output reports from the console,
// input reports back, EEPROM, the I2C bus with the speaker (0x51), the IR
// camera (0x58) and the extension port (0x52: Nunchuk).
//
// Public documentation used: WiiBrew "Wiimote" (reports, memory and
// registers, IR camera, speaker, extension controllers, Nunchuk); Dolphin's
// WiimoteEmu as information for the behaviour verified on hardware (read
// replies one per report period, status report and reporting disabled after
// an extension change, speaker register bits, default calibration values).
//
// Reports are given without the Bluetooth HID header: report ID, then payload.
//   output 0x10 rumble, 0x11 LEDs, 0x12 reporting mode, 0x13/0x1A IR camera
//   enable, 0x14 speaker enable, 0x15 status request, 0x16 write memory or
//   registers, 0x17 read memory or registers, 0x18 speaker data, 0x19 speaker
//   mute. Bit 0 of the first payload byte is rumble in all of them; bit 1
//   asks for an acknowledge (0x22) in 0x11-0x14, 0x19, 0x1A; 0x16 always
//   answers with one.
//   input 0x20 status, 0x21 read reply, 0x22 acknowledge, data reports
//   0x30-0x37 and 0x3D.
// Memory: EEPROM 0x0000-0x16FF (space 0); I2C registers (space 1 or 2) by
// device address. Errors: 4 busy, 6 invalid space, 7 no answer from the
// device, 8 invalid address.
//
// Not emulated, each reported by error(): the interleaved modes 0x3E/0x3F,
// encrypted extension reads (register 0xF0 = 0xAA), the Wii MotionPlus
// (address 0x53 does not answer, as without one), unknown output reports.
#pragma once
#include "orca/core/bits.hpp"

#include <array>
#include <deque>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace orca::wiimote {

// Core buttons as they appear in reports (big-endian 16-bit word)
namespace button {
inline constexpr u16 kLeft = 0x0100, kRight = 0x0200, kDown = 0x0400, kUp = 0x0800, kPlus = 0x1000;
inline constexpr u16 kTwo = 0x0001, kOne = 0x0002, kB = 0x0004, kA = 0x0008, kMinus = 0x0010, kHome = 0x0080;
inline constexpr u16 kMask = 0x1F9F;
} // namespace button

inline constexpr u16 kCameraWidth = 1024, kCameraHeight = 768;
inline constexpr u32 kEepromSize = 0x1700;

struct CameraPoint {
    u16 x{0xFFFF}, y{0xFFFF}; // outside 1024x768: not seen
    u8 size{0};               // 0-15
    bool visible() const noexcept { return x < kCameraWidth && y < kCameraHeight; }
};

struct NunchukState {
    u8 stick_x{0x80}, stick_y{0x80};
    u16 accel_x{0x200}, accel_y{0x200}, accel_z{0x2CC}; // 10-bit
    bool c{false}, z{false};
};

struct InputState {
    u16 buttons{0};                                      // button:: bits
    u16 accel_x{0x200}, accel_y{0x200}, accel_z{0x268}; // 10-bit (1 g on Z at rest)
    std::array<CameraPoint, 4> points{};
    std::optional<NunchukState> nunchuk;
    u8 battery{0xC0}; // status report battery level (below 0x20: "low")
};

class Wiimote {
public:
    Wiimote();
    // State after connecting: core-buttons reporting, not continuous, all
    // devices reset. The EEPROM is kept.
    void reset();

    std::array<u8, kEepromSize>& eeprom() noexcept { return eeprom_; }

    // One report from the console. False on a report that cannot be emulated (error()).
    bool output_report(std::span<const u8> report);
    // One report period: sends a status report after an extension change, else
    // the next part of a pending read, else the data report of the current
    // mode (not continuous: only when the data changed).
    void update(const InputState& in);
    // Next input report (report ID first), if any.
    std::optional<std::vector<u8>> pop_report();

    bool rumble() const noexcept { return rumble_; }
    u8 leds() const noexcept { return leds_; }
    u8 reporting_mode() const noexcept { return mode_; }
    bool ir_enabled() const noexcept { return camera_enabled_; }

    // Decoded speaker audio since the last call and its sample rate (12 MHz / divisor).
    std::vector<s16> take_speaker_samples();
    u32 speaker_sample_rate() const noexcept;

    bool failed() const noexcept { return !error_.empty(); }
    const std::string& error() const noexcept { return error_; }

private:
    void ack(u8 report, u8 error);
    void status_report();
    bool read_step();
    void data_report(const InputState& in);
    // I2C: number of bytes handled (0 = no answer)
    int bus_read(u8 device, u8 address, int count, u8* out);
    int bus_write(u8 device, u8 address, int count, const u8* in);
    void speaker_data(std::span<const u8> data);
    void camera_data(const InputState& in, u8* out, int count) const;
    bool fail(std::string why) { if (error_.empty()) error_ = std::move(why); return false; }

    std::array<u8, kEepromSize> eeprom_{};
    std::array<u8, 0x100> speaker_{}, camera_{}, extension_{};
    std::deque<std::vector<u8>> out_;
    u16 buttons_{0};
    u8 battery_{0xC0};
    u8 leds_{0};
    bool rumble_{false};
    u8 mode_{0x30};
    bool continuous_{false};
    bool camera_enabled_{false}, speaker_enabled_{false}, speaker_muted_{false};
    bool extension_reported_{false}; // extension bit of the last status report
    bool extension_present_{false};
    struct Read { u8 space{0}, device{0}; u16 address{0}, size{0}; } read_{};
    std::vector<u8> last_data_;
    s32 predictor_{0}, step_{127};
    std::vector<s16> speaker_out_;
    std::string error_;
};

// Nunchuk register block (identifier, calibration) as after power-on.
std::array<u8, 0x100> nunchuk_registers();

} // namespace orca::wiimote
