// Orca Core — high-level emulation of IOS, the Wii's Starlet operating system,
// as Broadway sees it: requests through the Hollywood IPC mailbox.
//
// Request block (0x40 bytes, physical address in PPCMSG; public Wii
// documentation, e.g. WiiBrew "IOS"):
//   +0x00 command  1 open, 2 close, 3 read, 4 write, 5 seek, 6 ioctl, 7 ioctlv
//   +0x04 result   (written by IOS)
//   +0x08 fd
//   +0x0C..+0x1C arguments:
//     open    path pointer, mode
//     read    buffer, length            write  buffer, length
//     seek    offset, whence
//     ioctl   number, in buffer, in length, out buffer, out length
//     ioctlv  number, input count, in/out count, vector table
//             (each vector: address, length)
// IOS answers by writing the result at +0x04, the original command at +0x08
// and 8 (reply) at +0x00, then hands the block address back in ARMMSG.
// Every address is physical.
//
// Sequence (the order and delays come from hardware measurements reported in
// Dolphin's notes; they are information, not code): Broadway sets X1; about
// 500 timebase ticks later IOS takes the request (clears X1, sets Y2 =
// acknowledge) and executes it; the reply (Y1) follows after the device's
// delay. Replies leave in request order. IOS sends one acknowledge/reply at a
// time: the next waits until Broadway has cleared Y1/Y2 and the IPC flag.
//
// Devices are registered by path. Paths outside /dev/ are files of the NAND
// and go to the device registered as /dev/fs. Opening a path no device claims, a command a
// device does not implement, or anything a device cannot do stops emulation
// with an explicit reason (stopped(), stop_reason()); nothing answers with an
// invented result. A device may also answer later (Result::deferred()) and
// call Kernel::reply() when the work finishes.
#pragma once
#include "orca/core/scheduler.hpp"
#include "orca/hw/hollywood_ipc.hpp"
#include "orca/memory/bus.hpp"

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace orca::ios {

enum class Command : u32 { Open = 1, Close = 2, Read = 3, Write = 4, Seek = 5, Ioctl = 6, Ioctlv = 7, Reply = 8 };

// IOS result codes
namespace err {
inline constexpr s32 kOk = 0;
inline constexpr s32 kAccess = -1;
inline constexpr s32 kExists = -2;
inline constexpr s32 kInvalid = -4;
inline constexpr s32 kMaxFds = -5;
inline constexpr s32 kNotFound = -6;
inline constexpr s32 kQueueFull = -8;
inline constexpr s32 kIo = -12;
inline constexpr s32 kNoMemory = -22;
} // namespace err

inline constexpr u32 kMaxFds = 0x18;  // descriptors available to Broadway
inline constexpr u32 kMaxPath = 64;   // including the terminating zero
// Reply delay used where no measurement is known (Dolphin's generic value).
inline constexpr u64 kDefaultReplyTicks = 4000;

struct IoVector {
    u32 address;
    u32 size;
};

struct Request {
    u32 address{0};
    u32 command{0};
    u32 fd{0};
    std::array<u32, 5> args{}; // +0x0C .. +0x1C
};

// Delays are in timebase ticks.
struct Result {
    enum class Kind : u8 { Reply, Deferred, Stop };
    Kind kind{Kind::Reply};
    s32 value{0};
    u64 delay_ticks{0};
    std::string reason;
    static Result reply(s32 v, u64 delay_ticks) { return {Kind::Reply, v, delay_ticks, {}}; }
    static Result deferred() { return {Kind::Deferred, 0, 0, {}}; }
    static Result stop(std::string why) { return {Kind::Stop, 0, 0, std::move(why)}; }
};

class Kernel;

class Device {
public:
    explicit Device(std::string path) : path_(std::move(path)) {}
    virtual ~Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    const std::string& path() const noexcept { return path_; }

    // Every command stops by default: a device implements what it supports.
    // `r.fd` is the descriptor the request will get if the open succeeds.
    virtual Result open(Kernel& k, const Request& r, const std::string& path, u32 mode);
    virtual Result close(Kernel& k, const Request& r);
    virtual Result read(Kernel& k, const Request& r, u32 buffer, u32 size);
    virtual Result write(Kernel& k, const Request& r, u32 buffer, u32 size);
    virtual Result seek(Kernel& k, const Request& r, s32 offset, u32 whence);
    virtual Result ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out);
    virtual Result ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                          std::span<const IoVector> io);

protected:
    Result unsupported(std::string_view what) const;

private:
    std::string path_;
};

class Kernel {
public:
    Kernel(mem::Bus& bus, core::Scheduler& scheduler, hw::HollywoodIpc& ipc, u32 cpu_cycles_per_timebase_tick);

    // Registers a device under its path; false if the path is taken.
    bool add_device(std::unique_ptr<Device> device);
    Device* device(std::string_view path) const;

    // Completes a deferred request `delay_ticks` from now.
    void reply(u32 request_address, s32 value, u64 delay_ticks);

    // Stops emulation with an explicit reason (first reason wins).
    void stop(std::string why);
    bool stopped() const noexcept { return stopped_; }
    const std::string& stop_reason() const noexcept { return stop_reason_; }

    // Physical memory helpers for devices (DMA-type accesses). The string
    // reader fails if no terminating zero is found within `max` bytes.
    bool read_bytes(u32 address, std::span<u8> out);
    bool write_bytes(u32 address, std::span<const u8> in);
    bool read32(u32 address, u32& v);
    bool write32(u32 address, u32 v);
    bool read_string(u32 address, u32 max, std::string& out);

    mem::Bus& bus() noexcept { return bus_; }
    core::Scheduler& scheduler() noexcept { return scheduler_; }
    u64 ticks_to_cycles(u64 ticks) const noexcept { return ticks * cycles_per_tick_; }

    Device* fd_device(u32 fd) const noexcept { return fd < kMaxFds ? fds_[fd] : nullptr; }

    // Owner and group of Broadway's requests (for the file system), set when a
    // title starts.
    void set_ppc_credentials(u32 uid, u16 gid) { ppc_uid_ = uid; ppc_gid_ = gid; }
    std::optional<std::pair<u32, u16>> ppc_credentials() const {
        if (!ppc_uid_) return std::nullopt;
        return std::pair<u32, u16>{*ppc_uid_, ppc_gid_};
    }
    u64 requests_executed() const noexcept { return executed_; }

private:
    void on_execute(u32 address);
    void update();
    void execute(u32 address);
    Result dispatch(const Request& r);
    void finish(const Request& r, s32 value, u64 delay_ticks);

    mem::Bus& bus_;
    core::Scheduler& scheduler_;
    hw::HollywoodIpc& ipc_;
    u64 cycles_per_tick_;
    std::map<std::string, std::unique_ptr<Device>, std::less<>> devices_;
    std::array<Device*, kMaxFds> fds_{};
    std::map<u32, u32> pending_; // deferred request address -> command
    std::deque<u32> requests_, replies_;
    u64 last_reply_time_{0};
    u64 executed_{0};
    std::optional<u32> ppc_uid_;
    u16 ppc_gid_{0};
    bool stopped_{false};
    std::string stop_reason_;
};

} // namespace orca::ios
