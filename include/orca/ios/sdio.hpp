// Orca Core — IOS /dev/sdio/slot0, the front SD card slot, with no card inserted
// (public Wii documentation, e.g. WiiBrew "/dev/sdio"; Dolphin's SDIOSlot0 studied
// as information). Orca has no SD card image, so the slot is empty, as on a
// console with nothing in the slot:
//   ioctl 0x0B GETSTATUS  -> 0 (no card)
//   ioctl 0x01/0x02       write/read a host-controller register (stored; the
//                         clock-control register reports "stable" once enabled,
//                         a software-reset write clears its register)
//   ioctl 0x04 RESETCARD  -> status word (0: no card)
//   ioctl 0x06 SETCLK     accepted
//   ioctl 0x0C GETOCR     -> 0x00FF8000 (voltage window; card not initialised)
//   ioctl 0x07 SENDCMD    command 0x40 (register an event): an insert event stays
//                         pending (no card arrives); a remove event is answered at
//                         once (the slot is empty). 0x41 cancels the pending
//                         event. Any other card command stops explicitly: there
//                         is no card to answer it.
// ioctlv requests (data transfers) stop explicitly.
#pragma once
#include "orca/ios/kernel.hpp"

#include <array>
#include <optional>

namespace orca::ios {

class SdioSlot0 final : public Device {
public:
    SdioSlot0() : Device("/dev/sdio/slot0") {}
    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;

    static constexpr u32 kEventInsert = 1, kEventRemove = 2, kEventInvalid = 3;
    bool event_pending() const noexcept { return event_.has_value(); }

private:
    struct Event { u32 request; u32 type; };
    std::array<u32, 0x200 / 4> hcr_{};
    std::optional<Event> event_;
};

} // namespace orca::ios
