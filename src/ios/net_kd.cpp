#include "orca/ios/net_kd.hpp"

#include <cstdio>
#include <vector>

namespace orca::ios {

namespace {
std::string name_of(u32 n) {
    switch (n) {
    case 0x04: return "KD_GET_TIME_TRIGGERS";
    case 0x05: return "SET_SCHEDULE_SPAN";
    case 0x06: return "STARTUP_SOCKET";
    case 0x07: return "CLEANUP_SOCKET";
    case 0x08: return "LOCK_SOCKET";
    case 0x09: return "UNLOCK_SOCKET";
    case 0x0A: return "CHECK_MAIL_NOW";
    case 0x0B: return "SEND_MAIL_NOW";
    case 0x0C: return "RECEIVE_MAIL_NOW";
    case 0x0D: return "SAVE_MAIL_NOW";
    case 0x0E: return "DOWNLOAD_NOW_EX";
    case 0x0F: return "REQUEST_GENERATED_USER_ID";
    case 0x10: return "REQUEST_REGISTER_USER_ID";
    case 0x1F: return "SET_FILTER_MODE";
    case 0x20: return "SET_DEBUG_MODE";
    case 0x21: return "KD_SET_NEXT_WAKEUP";
    case 0x22: return "SET_SCRIPT_MODE";
    default: {
        char b[16];
        std::snprintf(b, sizeof b, "0x%X", n);
        return b;
    }
    }
}
} // namespace

Result NetKdRequest::open(Kernel&, const Request&, const std::string&, u32) {
    return Result::reply(err::kOk, kDefaultReplyTicks);
}

Result NetKdRequest::close(Kernel&, const Request&) { return Result::reply(err::kOk, kDefaultReplyTicks); }

Result NetKdRequest::ioctl(Kernel& k, const Request&, u32 number, IoVector, IoVector out) {
    const auto status_word = [&](u32 v) -> bool {
        if (out.size < 4U || (out.address & 3U) != 0U) return false;
        return k.write32(out.address, v);
    };
    switch (number) {
    case 0x01: // SUSPEND_SCHEDULER
        if (!status_word(0)) return Result::reply(err::kInvalid, kDefaultReplyTicks);
        ++suspended_;
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case 0x02: // EXEC_TRY_SUSPEND_SCHEDULER
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case 0x03: // EXEC_RESUME_SCHEDULER
        if (suspended_ == 0)
            return Result::stop("IOS /dev/net/kd/request: RESUME_SCHEDULER without a matching suspend (not emulated)");
        if (!status_word(0)) return Result::reply(err::kInvalid, kDefaultReplyTicks);
        --suspended_;
        return Result::reply(err::kOk, kDefaultReplyTicks);
    case 0x1E: { // GET_SCHEDULER_STAT
        if (out.address == 0U || (out.address & 3U) != 0U || out.size < 16U) return Result::reply(err::kInvalid, kDefaultReplyTicks);
        const std::vector<u8> zero(out.size < 256U ? out.size : 256U, 0);
        if (!k.write_bytes(out.address, zero)) return Result::stop("IOS /dev/net/kd/request: output buffer not in RAM");
        return Result::reply(err::kOk, kDefaultReplyTicks);
    }
    case 0x28: // REQUEST_SHUTDOWN
        return Result::reply(err::kOk, kDefaultReplyTicks);
    default:
        return Result::stop("IOS /dev/net/kd/request: " + name_of(number) +
                            " needs the network or WiiConnect24 setup, which Orca does not emulate");
    }
}

} // namespace orca::ios
