// Orca Core — IOS state manager devices (public Wii documentation, e.g.
// WiiBrew "/dev/stm"):
//   /dev/stm/eventhook  ioctl 0x1000: answered when the console's reset or
//                       power button is pressed (event word written to the
//                       output buffer: 0x20000 reset, 0x800 power), or with
//                       event 0 when /dev/stm/immediate releases the hook.
//   /dev/stm/immediate  0x2001 hot reset, 0x2003 shutdown, 0x2004 idle (power
//                       off with standby), 0x3002 release the event hook,
//                       0x5001 video dimming, 0x6002 slot LED mode.
// Reset, shutdown and idle end emulation with an explicit stop that names the
// request (they leave the game; a return to the system menu is not
// emulated). Dimming and the LED mode are recorded for the frontend. Every
// other ioctl stops explicitly.
#pragma once
#include "orca/ios/kernel.hpp"

#include <optional>

namespace orca::ios {

inline constexpr u32 kStmEventReset = 0x00020000U;
inline constexpr u32 kStmEventPower = 0x00000800U;

class StmEventHook final : public Device {
public:
    StmEventHook() : Device("/dev/stm/eventhook") {}
    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;

    bool hook_installed() const noexcept { return hook_.has_value(); }
    // Answers the pending hook with `event`; false if no hook is installed.
    bool trigger(Kernel& k, u32 event, u64 delay_ticks);

private:
    struct Hook { u32 request; u32 out; };
    std::optional<Hook> hook_;
    bool open_{false};
};

class StmImmediate final : public Device {
public:
    explicit StmImmediate(StmEventHook& hook) : Device("/dev/stm/immediate"), hook_(hook) {}
    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;

    // first input word of the last request (meaning not interpreted here)
    std::optional<u32> dimming_request() const noexcept { return dimming_; }
    std::optional<u32> led_mode_request() const noexcept { return led_mode_; }

private:
    StmEventHook& hook_;
    std::optional<u32> dimming_, led_mode_;
};

} // namespace orca::ios
