#include "orca/ios/sdio.hpp"

#include <cstdio>
#include <vector>

namespace orca::ios {

namespace {
constexpr u32 kWriteHcr = 0x01, kReadHcr = 0x02, kResetCard = 0x04, kSetClk = 0x06, kSendCmd = 0x07,
              kGetStatus = 0x0B, kGetOcr = 0x0C;
constexpr u32 kHcrClockControl = 0x2C, kHcrSoftwareReset = 0x2F;
constexpr u32 kCmdEventRegister = 0x40, kCmdEventUnregister = 0x41;
constexpr u32 kStatusNoCard = 0;
constexpr u32 kOcrNoCard = 0x00FF8000U;

std::string hex(u32 v) {
    char b[12];
    std::snprintf(b, sizeof b, "%X", v);
    return b;
}
} // namespace

Result SdioSlot0::open(Kernel&, const Request&, const std::string&, u32) {
    hcr_.fill(0);
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

Result SdioSlot0::close(Kernel&, const Request&) {
    if (event_) return Result::stop("IOS /dev/sdio/slot0: close while an SD event is pending is not emulated");
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

Result SdioSlot0::ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) {
    // IOS clears the output buffer before answering
    if (out.size != 0U) {
        const std::vector<u8> zero(out.size, 0);
        if (!k.write_bytes(out.address, zero)) return Result::stop("IOS /dev/sdio/slot0: output buffer not in RAM");
    }
    const auto word_out = [&](u32 v) { return out.size >= 4U && k.write32(out.address, v); };
    switch (number) {
    case kGetStatus:
        if (!word_out(kStatusNoCard)) return Result::stop("IOS /dev/sdio/slot0: GETSTATUS output buffer too small");
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case kGetOcr:
        if (!word_out(kOcrNoCard)) return Result::stop("IOS /dev/sdio/slot0: GETOCR output buffer too small");
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case kResetCard:
        if (!word_out(kStatusNoCard)) return Result::stop("IOS /dev/sdio/slot0: RESETCARD output buffer too small");
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case kSetClk:
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case kWriteHcr: case kReadHcr: {
        u32 reg = 0, val = 0;
        if (in.size < 4U || !k.read32(in.address, reg)) return Result::stop("IOS /dev/sdio/slot0: HCR request without a register");
        if (reg >= hcr_.size() * 4U) return Result::stop("IOS /dev/sdio/slot0: host-controller register 0x" + hex(reg) + " out of range");
        u32& slot = hcr_[reg / 4U];
        if (number == kReadHcr) {
            if (!word_out(slot)) return Result::stop("IOS /dev/sdio/slot0: READHCR output buffer too small");
            return Result::reply(err::kOk, kDefaultReplyTicks);
        }
        if (in.size < 20U || !k.read32(in.address + 16U, val)) return Result::stop("IOS /dev/sdio/slot0: WRITEHCR without a value");
        if (reg == kHcrClockControl && (val & 1U) != 0U) slot = val | 2U; // internal clock reported stable
        else if (reg == kHcrSoftwareReset && val != 0U) slot = 0;
        else slot = val;
        return Result::reply(err::kOk, kDefaultReplyTicks);
    }
    case kSendCmd: {
        u32 command = 0, arg = 0;
        if (in.size < 16U || !k.read32(in.address, command) || !k.read32(in.address + 12U, arg))
            return Result::stop("IOS /dev/sdio/slot0: SENDCMD without a command block");
        if (command == kCmdEventRegister) {
            if (event_) return Result::stop("IOS /dev/sdio/slot0: a second SD event registered while one is pending");
            if (arg == kEventRemove) return Result::reply(static_cast<s32>(kEventRemove), kDefaultReplyTicks); // already empty
            if (arg != kEventInsert) return Result::stop("IOS /dev/sdio/slot0: unknown SD event type " + hex(arg));
            event_ = Event{r.address, arg};
            return Result::deferred(); // no card will be inserted
        }
        if (command == kCmdEventUnregister) {
            if (!event_) return Result::reply(err::kInvalid, kDefaultReplyTicks);
            k.reply(event_->request, static_cast<s32>(kEventInvalid), kDefaultReplyTicks);
            event_.reset();
            return Result::reply(err::kOk, kDefaultReplyTicks);
        }
        return Result::stop("IOS /dev/sdio/slot0: SD command 0x" + hex(command) + " with no SD card inserted (Orca has no SD card)");
    }
    default:
        return unsupported("ioctl 0x" + hex(number));
    }
}

} // namespace orca::ios
