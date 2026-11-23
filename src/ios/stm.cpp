#include "orca/ios/stm.hpp"

#include <cstdio>

namespace orca::ios {

namespace {
constexpr u32 kEventHook = 0x1000;
constexpr u32 kHotReset = 0x2001;
constexpr u32 kShutdown = 0x2003;
constexpr u32 kIdle = 0x2004;
constexpr u32 kReleaseHook = 0x3002;
constexpr u32 kVideoDimming = 0x5001;
constexpr u32 kLedMode = 0x6002;
} // namespace

Result StmEventHook::open(Kernel&, const Request&, u32) {
    open_ = true;
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

Result StmEventHook::close(Kernel& k, const Request&) {
    if (hook_) return Result::stop("IOS /dev/stm/eventhook: close while the event hook is pending is not emulated");
    (void)k;
    open_ = false;
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

Result StmEventHook::ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) {
    if (number != kEventHook) return Device::ioctl(k, r, number, in, out);
    if (hook_) return Result::reply(err::kExists, kDefaultReplyTicks);
    if (out.size < 4U) return Result::stop("IOS /dev/stm/eventhook: output buffer smaller than 4 bytes");
    hook_ = Hook{r.address, out.address};
    return Result::deferred();
}

bool StmEventHook::trigger(Kernel& k, u32 event, u64 delay_ticks) {
    if (!hook_) return false;
    const Hook h = *hook_;
    hook_.reset();
    if (!k.write32(h.out, event)) {
        k.stop("IOS /dev/stm/eventhook: output buffer is not in RAM");
        return false;
    }
    k.reply(h.request, err::kOk, delay_ticks);
    return true;
}

Result StmImmediate::open(Kernel&, const Request&, u32) { return Result::reply(err::kOk, kDefaultReplyTicks); }
Result StmImmediate::close(Kernel&, const Request&) { return Result::reply(err::kOk, kDefaultReplyTicks); }

Result StmImmediate::ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) {
    switch (number) {
    case kReleaseHook:
        if (!hook_.hook_installed()) return Result::reply(err::kNotFound, kDefaultReplyTicks);
        if (!hook_.trigger(k, 0, 0)) return Result::stop("IOS /dev/stm/immediate: releasing the event hook failed");
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case kHotReset:
        return Result::stop("IOS /dev/stm/immediate: the game asked for a hot reset (system menu not emulated)");
    case kShutdown:
        return Result::stop("IOS /dev/stm/immediate: the game asked to power off");
    case kIdle:
        return Result::stop("IOS /dev/stm/immediate: the game asked to power off to standby");
    case kVideoDimming:
    case kLedMode: {
        u32 word = 0;
        if (in.size < 4U || !k.read32(in.address, word))
            return Result::stop("IOS /dev/stm/immediate: ioctl input is missing or not in RAM");
        (number == kVideoDimming ? dimming_ : led_mode_) = word;
        return Result::reply(err::kOk, kDefaultReplyTicks);
    }
    default:
        return Device::ioctl(k, r, number, in, out);
    }
}

} // namespace orca::ios
