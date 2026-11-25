#include "orca/ios/net_kd_time.hpp"

#include <cstdio>

namespace orca::ios {

Result NetKdTime::open(Kernel&, const Request&, const std::string&, u32) { return Result::reply(err::kOk, kDefaultReplyTicks); }
Result NetKdTime::close(Kernel&, const Request&) { return Result::reply(err::kOk, kDefaultReplyTicks); }

Result NetKdTime::ioctl(Kernel& k, const Request&, u32 number, IoVector in, IoVector out) {
    if (out.size < 4U || (out.address & 3U) != 0U) return Result::reply(err::kInvalid, kDefaultReplyTicks);
    const auto put64 = [&](u64 v) {
        return out.size >= 12U && k.write32(out.address + 4U, static_cast<u32>(v >> 32)) &&
               k.write32(out.address + 8U, static_cast<u32>(v));
    };
    const auto misc_update = [&](u32 offset) -> bool { // true: misc.bin update requested
        u32 flag = 0;
        return k.read32(in.address + offset, flag) && flag != 0U;
    };
    s32 result = 0;
    switch (number) {
    case 0x14:
        if (!put64(universal())) return Result::reply(err::kInvalid, kDefaultReplyTicks);
        break;
    case 0x15: {
        u32 hi = 0, lo = 0;
        if (in.size < 12U || !k.read32(in.address, hi) || !k.read32(in.address + 4U, lo))
            return Result::reply(err::kInvalid, kDefaultReplyTicks);
        if (misc_update(8U))
            return Result::stop("IOS /dev/net/kd/time: SET_UNIVERSAL_TIME asks to update /shared2/nwc24/misc.bin (not emulated)");
        offset_ = static_cast<s64>((u64{hi} << 32) | lo) - static_cast<s64>(clock_());
        break;
    }
    case 0x16:
        result = -9;
        break;
    case 0x17:
        if (in.size < 8U || !k.read32(in.address, rtc_)) return Result::reply(err::kInvalid, kDefaultReplyTicks);
        if (misc_update(4U))
            return Result::stop("IOS /dev/net/kd/time: SET_RTC_COUNTER asks to update /shared2/nwc24/misc.bin (not emulated)");
        break;
    case 0x18:
        if (!put64(universal() - rtc_)) return Result::reply(err::kInvalid, kDefaultReplyTicks);
        break;
    default: {
        char b[64];
        std::snprintf(b, sizeof b, "ioctl 0x%X", number);
        return unsupported(b);
    }
    }
    if (!k.write32(out.address, 0)) return Result::stop("IOS /dev/net/kd/time: output buffer not in RAM");
    return Result::reply(result, kDefaultReplyTicks);
}

} // namespace orca::ios
