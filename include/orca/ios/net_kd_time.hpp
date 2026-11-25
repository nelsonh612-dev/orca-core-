// Orca Core — IOS /dev/net/kd/time, the WiiConnect24 daemon's clock (WiiBrew
// "/dev/net/kd/time"; Dolphin's NetKDTimeDevice studied as information).
// Without network the daemon never synchronises with a time server: universal
// time is the console clock (RTC counter + SRAM counter bias, as seconds since
// 1970) plus the offset software sets with SET_UNIVERSAL_TIME.
//   0x14 GET_UNIVERSAL_TIME   out: [0] result 0, [4] u64 seconds
//   0x15 SET_UNIVERSAL_TIME   in: u64 seconds, u32 "update misc.bin"
//   0x16                      answers -9 (not implemented in IOS either)
//   0x17 SET_RTC_COUNTER      in: u32 counter, u32 "update misc.bin"
//   0x18 GET_TIME_DIFF        out: [0] 0, [4] u64 universal time - counter
// Asking to update /shared2/nwc24/misc.bin stops explicitly (not emulated).
#pragma once
#include "orca/ios/kernel.hpp"

#include <functional>

namespace orca::ios {

class NetKdTime final : public Device {
public:
    // `clock`: the console's current date, seconds since 1970-01-01
    explicit NetKdTime(std::function<u64()> clock) : Device("/dev/net/kd/time"), clock_(std::move(clock)) {}
    Result open(Kernel& k, const Request& r, const std::string& path, u32 mode) override;
    Result close(Kernel& k, const Request& r) override;
    Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) override;

private:
    std::function<u64()> clock_;
    s64 offset_{0};
    u32 rtc_{0};
    u64 universal() const { return static_cast<u64>(static_cast<s64>(clock_()) + offset_); }
};

} // namespace orca::ios
