#include "orca/ios/kernel.hpp"

#include <cstdio>

namespace orca::ios {

namespace {
constexpr u64 kAckDelayTicks = 500;       // X1 -> IOS takes the request
constexpr u64 kInvalidCommandTicks = 978; // unknown command number
constexpr u64 kInvalidFdTicks = 550;      // descriptor not open
constexpr u64 kNoFreeFdTicks = 5000;      // all descriptors in use
constexpr u32 kMaxVectors = 32;           // sanity bound for ioctlv vector counts

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

const char* command_name(u32 c) {
    switch (c) {
    case 1: return "open";
    case 2: return "close";
    case 3: return "read";
    case 4: return "write";
    case 5: return "seek";
    case 6: return "ioctl";
    case 7: return "ioctlv";
    default: return "?";
    }
}
} // namespace

// --- Device defaults: every command stops -----------------------------------

Result Device::unsupported(std::string_view what) const {
    return Result::stop("IOS " + path_ + ": " + std::string(what) + " is not emulated");
}
Result Device::open(Kernel&, const Request&, u32) { return unsupported("open"); }
Result Device::close(Kernel&, const Request&) { return unsupported("close"); }
Result Device::read(Kernel&, const Request&, u32, u32) { return unsupported("read"); }
Result Device::write(Kernel&, const Request&, u32, u32) { return unsupported("write"); }
Result Device::seek(Kernel&, const Request&, s32, u32) { return unsupported("seek"); }
Result Device::ioctl(Kernel&, const Request&, u32 number, IoVector, IoVector) {
    return unsupported("ioctl 0x" + hex(number));
}
Result Device::ioctlv(Kernel&, const Request&, u32 number, std::span<const IoVector>, std::span<const IoVector>) {
    return unsupported("ioctlv 0x" + hex(number));
}

// --- Kernel -----------------------------------------------------------------

Kernel::Kernel(mem::Bus& bus, core::Scheduler& scheduler, hw::HollywoodIpc& ipc, u32 cpu_cycles_per_timebase_tick)
    : bus_(bus), scheduler_(scheduler), ipc_(ipc), cycles_per_tick_(cpu_cycles_per_timebase_tick) {
    ipc_.set_execute_listener([this](u32 address) { on_execute(address); });
    ipc_.set_ready_listener([this] { update(); });
}

bool Kernel::add_device(std::unique_ptr<Device> device) {
    if (!device) return false;
    const std::string path = device->path();
    return devices_.emplace(path, std::move(device)).second;
}

Device* Kernel::device(std::string_view path) const {
    const auto it = devices_.find(path);
    return it == devices_.end() ? nullptr : it->second.get();
}

void Kernel::stop(std::string why) {
    if (stopped_) return;
    stopped_ = true;
    stop_reason_ = std::move(why);
}

bool Kernel::read_bytes(u32 address, std::span<u8> out) {
    return out.empty() || bus_.dma_read(address, out) == mem::BusStatus::Ok;
}
bool Kernel::write_bytes(u32 address, std::span<const u8> in) {
    return in.empty() || bus_.dma_write(address, in) == mem::BusStatus::Ok;
}
bool Kernel::read32(u32 address, u32& v) {
    std::array<u8, 4> b{};
    if (!read_bytes(address, b)) return false;
    v = static_cast<u32>(b[0]) << 24 | static_cast<u32>(b[1]) << 16 | static_cast<u32>(b[2]) << 8 | b[3];
    return true;
}
bool Kernel::write32(u32 address, u32 v) {
    const std::array<u8, 4> b{static_cast<u8>(v >> 24), static_cast<u8>(v >> 16), static_cast<u8>(v >> 8),
                              static_cast<u8>(v)};
    return write_bytes(address, b);
}
bool Kernel::read_string(u32 address, u32 max, std::string& out) {
    out.clear();
    for (u32 i = 0; i < max; ++i) {
        std::array<u8, 1> c{};
        if (!read_bytes(address + i, c)) return false;
        if (c[0] == 0U) return true;
        out.push_back(static_cast<char>(c[0]));
    }
    return false;
}

void Kernel::on_execute(u32 address) {
    scheduler_.schedule_in(ticks_to_cycles(kAckDelayTicks), "IOS request", [this, address](u64, u64) {
        requests_.push_back(address);
        update();
    });
}

void Kernel::update() {
    if (stopped_ || !ipc_.ready()) return;
    if (!requests_.empty()) {
        const u32 address = requests_.front();
        requests_.pop_front();
        ipc_.clear_execute();
        ipc_.acknowledge();
        execute(address);
        return;
    }
    if (!replies_.empty()) {
        ipc_.reply(replies_.front());
        replies_.pop_front();
    }
}

void Kernel::execute(u32 address) {
    Request r;
    r.address = address;
    u32 words[8];
    for (u32 i = 0; i < 8; ++i) {
        if (!read32(address + 4U * i, words[i])) {
            stop("IOS: request block at 0x" + hex(address) + " is not in RAM");
            return;
        }
    }
    r.command = words[0];
    r.fd = words[2];
    for (u32 i = 0; i < 5; ++i) r.args[i] = words[3 + i];
    ++executed_;
    Result res = dispatch(r);
    switch (res.kind) {
    case Result::Kind::Reply: finish(r, res.value, res.delay_ticks); break;
    case Result::Kind::Deferred: pending_[address] = r.command; break;
    case Result::Kind::Stop: stop(std::move(res.reason)); break;
    }
}

Result Kernel::dispatch(const Request& r) {
    if (r.command < 1U || r.command > 7U) return Result::reply(err::kInvalid, kInvalidCommandTicks);
    const auto cmd = static_cast<Command>(r.command);
    if (cmd == Command::Open) {
        std::string path;
        if (!read_string(r.args[0], kMaxPath, path))
            return Result::stop("IOS open: path at 0x" + hex(r.args[0]) + " is not a string of at most 63 bytes in RAM");
        Device* d = device(path);
        if (d == nullptr) return Result::stop("IOS open of \"" + path + "\": no emulated device or file system path");
        u32 fd = 0;
        while (fd < kMaxFds && fds_[fd] != nullptr) ++fd;
        if (fd == kMaxFds) return Result::reply(err::kMaxFds, kNoFreeFdTicks);
        Result res = d->open(*this, r, r.args[1]);
        if (res.kind == Result::Kind::Deferred)
            return Result::stop("IOS " + path + ": deferred open is not supported by the kernel");
        if (res.kind == Result::Kind::Reply && res.value >= 0) {
            fds_[fd] = d;
            res.value = static_cast<s32>(fd);
        }
        return res;
    }
    Device* d = fd_device(r.fd);
    if (d == nullptr) return Result::reply(err::kInvalid, kInvalidFdTicks);
    switch (cmd) {
    case Command::Close: {
        Result res = d->close(*this, r);
        if (res.kind != Result::Kind::Stop) fds_[r.fd] = nullptr;
        return res;
    }
    case Command::Read: return d->read(*this, r, r.args[0], r.args[1]);
    case Command::Write: return d->write(*this, r, r.args[0], r.args[1]);
    case Command::Seek: return d->seek(*this, r, static_cast<s32>(r.args[0]), r.args[1]);
    case Command::Ioctl: return d->ioctl(*this, r, r.args[0], {r.args[1], r.args[2]}, {r.args[3], r.args[4]});
    case Command::Ioctlv: {
        const u32 n_in = r.args[1], n_io = r.args[2];
        if (n_in > kMaxVectors || n_io > kMaxVectors)
            return Result::stop("IOS " + d->path() + ": ioctlv 0x" + hex(r.args[0]) + " with " + std::to_string(n_in) +
                                "+" + std::to_string(n_io) + " vectors");
        std::vector<IoVector> v(n_in + n_io);
        for (u32 i = 0; i < n_in + n_io; ++i) {
            if (!read32(r.args[3] + 8U * i, v[i].address) || !read32(r.args[3] + 8U * i + 4U, v[i].size))
                return Result::stop("IOS " + d->path() + ": ioctlv vector table at 0x" + hex(r.args[3]) + " is not in RAM");
        }
        return d->ioctlv(*this, r, r.args[0], std::span<const IoVector>(v.data(), n_in),
                         std::span<const IoVector>(v.data() + n_in, n_io));
    }
    default: break;
    }
    return Result::stop(std::string("IOS: unexpected command ") + command_name(r.command));
}

void Kernel::finish(const Request& r, s32 value, u64 delay_ticks) {
    if (!write32(r.address + 4U, static_cast<u32>(value)) || !write32(r.address + 8U, r.command) ||
        !write32(r.address, static_cast<u32>(Command::Reply))) {
        stop("IOS: request block at 0x" + hex(r.address) + " is not in RAM");
        return;
    }
    u64 due = scheduler_.now() + ticks_to_cycles(delay_ticks);
    if (due < last_reply_time_) due = last_reply_time_; // replies leave in order
    last_reply_time_ = due;
    const u32 address = r.address;
    scheduler_.schedule_at(due, "IOS reply", [this, address](u64, u64) {
        replies_.push_back(address);
        update();
    });
}

void Kernel::reply(u32 request_address, s32 value, u64 delay_ticks) {
    const auto it = pending_.find(request_address);
    if (it == pending_.end()) {
        stop("IOS: reply to 0x" + hex(request_address) + ", which is not a pending request");
        return;
    }
    Request r;
    r.address = request_address;
    r.command = it->second;
    pending_.erase(it);
    finish(r, value, delay_ticks);
}

} // namespace orca::ios
