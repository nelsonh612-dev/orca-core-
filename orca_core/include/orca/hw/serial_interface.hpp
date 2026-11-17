// Orca Core — Serial Interface (SI, physical 0x0C006400) and the standard
// GameCube controller (Joy-Bus device).
//
// Registers (YAGCD 5.8): per channel n (stride 0xC) SICnOUTBUF (+0),
// SICnINBUFH (+4), SICnINBUFL (+8); SIPOLL 0x30; SICOMCSR 0x34; SISR 0x38;
// SIEXILK 0x3C; I/O buffer 0x80-0xFF (128 bytes).
//   SICOMCSR: TCINT(31, w1c) TCINTMSK(30) COMERR(29) RDSTINT(28) RDSTINTMSK(27)
//             OUTLNGTH(22:16, 0 = 128) INLNGTH(14:8, 0 = 128) CHANNEL(2:1) TSTART(0)
//   SISR per channel c (channel 0 in bits 29:24, 1 in 21:16, 2 in 13:8, 3 in 5:0):
//             RDST, WRST, NOREP(w1c), COLL(w1c), OVRUN(w1c), UNRUN(w1c); WR(31)
//   SICnINBUFH: ERRSTAT(31) ERRLATCH(30) input byte0[5:0] byte1 byte2 byte3
//   SICnINBUFL: input bytes 4-7
//
// Standard controller protocol (YAGCD chapter 9, libjoybus):
//   0x00/0xFF identify: 1 -> 3 bytes: 0x09 0x00 status (bit5 need-origin,
//             bits4:3 motor, bits2:0 analog mode)
//   0x40 poll: 3 -> 8 bytes (byte1 = analog mode, byte2 = motor); analog mode 3:
//             [0 0 NEEDORIGIN START Y X B A] [1 L R Z UP DOWN RIGHT LEFT]
//             stickX stickY cstickX cstickY triggerL triggerR
//   0x41 origin: 1 -> 10 bytes; 0x42 calibrate: 3 -> 10 bytes
// Other analog modes and commands are reported as errors (clear CPU stop).
//
// Simplifications (documented in docs/ESTADO.md): transfers complete at the
// next scheduler step; automatic polling (SIPOLL) runs every X video lines
// while the VI is running (the Y count per frame is not modelled); the EN bit
// for channel c is bit (7 - c) and VBCPY is not modelled.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace orca::hw {

class VideoInterface;

class SiDevice {
public:
    virtual ~SiDevice() = default;
    // Returns the response bytes, or nullopt with `error` set if the command is
    // not supported (the triggering MMIO write then fails with that reason).
    virtual std::optional<std::vector<u8>> transfer(const std::vector<u8>& out, std::string& error) = 0;
};

struct PadButtons {
    static constexpr u16 A = 0x0100, B = 0x0200, X = 0x0400, Y = 0x0800, START = 0x1000;
    static constexpr u16 LEFT = 0x0001, RIGHT = 0x0002, DOWN = 0x0004, UP = 0x0008, Z = 0x0010, R = 0x0020, L = 0x0040;
};

struct PadState {
    u16 buttons{0};     // PadButtons bits (same positions as the first two status bytes)
    u8 stick_x{0x80}, stick_y{0x80};
    u8 cstick_x{0x80}, cstick_y{0x80};
    u8 trigger_l{0}, trigger_r{0};
};

class StandardController final : public SiDevice {
public:
    PadState state;                    // set by the frontend / tests
    PadState origin;
    bool need_origin{true};
    u8 motor{0};
    u8 analog_mode{3};
    // Raw two-bit motor command from PAD_CMD_POLL. The frontend maps this to
    // its haptic device without coupling the emulated SI device to Android.
    void set_rumble_sink(std::function<void(u8)> sink) { rumble_sink_ = std::move(sink); }
    std::optional<std::vector<u8>> transfer(const std::vector<u8>& out, std::string& error) override;
    std::vector<u8> status_bytes() const;
private:
    std::function<void(u8)> rumble_sink_;
};

namespace si {
inline constexpr u32 kOffsetInPage = 0x400U;
inline constexpr u32 TCINT = 0x80000000U, TCINTMSK = 0x40000000U, COMERR = 0x20000000U, RDSTINT = 0x10000000U,
                     RDSTINTMSK = 0x08000000U, TSTART = 0x00000001U;
inline constexpr u32 SISR_WR = 0x80000000U;
inline constexpr u32 SR_RDST = 0x20U, SR_WRST = 0x10U, SR_NOREP = 0x08U, SR_COLL = 0x04U, SR_OVRUN = 0x02U, SR_UNRUN = 0x01U;
constexpr unsigned sisr_shift(unsigned channel) noexcept { return 24U - channel * 8U; }
} // namespace si

class SerialInterface final : public mem::MmioDevice {
public:
    SerialInterface(core::Scheduler& scheduler, ProcessorInterface& pi, const VideoInterface* vi);

    void connect(unsigned channel, std::shared_ptr<SiDevice> device) { devices_[channel & 3U] = std::move(device); }
    // Called at every VI field start: starts automatic polling once video runs.
    void on_video_field() { if (poll_event_ == 0U) schedule_poll(); }

    bool mmio_read(u32 offset, unsigned size, u64& value) override;
    bool mmio_write(u32 offset, unsigned size, u64 value) override;
    const char* mmio_name() const noexcept override { return "SI"; }
    std::string mmio_error() const override { return last_error_; }

private:
    bool start_transfer();
    void poll();
    void schedule_poll();
    void update_interrupt();
    bool fail(std::string why) { last_error_ = std::move(why); return false; }

    core::Scheduler& scheduler_;
    ProcessorInterface& pi_;
    const VideoInterface* vi_;
    std::array<std::shared_ptr<SiDevice>, 4> devices_{};
    std::array<u32, 4> outbuf_{}, inbuf_h_{}, inbuf_l_{};
    u32 sipoll_{0}, comcsr_{0}, sisr_{0}, exilk_{0};
    std::array<u8, 128> io_{};
    core::Scheduler::EventId poll_event_{0};
    std::string last_error_;
};

} // namespace orca::hw
