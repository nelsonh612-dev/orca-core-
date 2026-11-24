#include "orca/ios/di_device.hpp"

#include <array>
#include <cstdio>

namespace orca::ios {

namespace {
constexpr u64 kOverheadTicks = 2700;
constexpr s32 kTimedOut = 0x10;

std::string hex(u32 v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}

u32 be32(const u8* p) { return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3]; }

// DI register offsets
constexpr u32 DISR = 0x00, DICVR = 0x04, DICMD0 = 0x08, DICMD1 = 0x0C, DICMD2 = 0x10, DIMAR = 0x14, DILENGTH = 0x18,
              DICR = 0x1C, DIIMMBUF = 0x20;
} // namespace

std::optional<u32> uid_for_title(NandFs& nand, u64 title, std::string& error) {
    const std::string path = "/sys/uid.sys";
    FsMeta m;
    if (nand.metadata(0, 0, "/sys", m) == fs_err::kNotFound && nand.create(0, 0, "/sys", 0, {3, 3, 0}, false) != 0) {
        error = "uid.sys: cannot create /sys on the NAND";
        return std::nullopt;
    }
    if (nand.metadata(0, 0, path, m) == fs_err::kNotFound && nand.create(0, 0, path, 0, {3, 3, 0}, true) != 0) {
        error = "uid.sys: cannot create " + path;
        return std::nullopt;
    }
    const s32 h = nand.open(0, 0, path, 3);
    if (h < 0) { error = "uid.sys: cannot open (" + std::to_string(h) + ")"; return std::nullopt; }
    u32 size = 0, pos = 0;
    nand.status(h, size, pos);
    std::vector<u8> data(size);
    if (nand.read(h, data) != static_cast<s32>(size) || size % 12U != 0U) {
        nand.close(h);
        error = "uid.sys: unreadable or not a list of 12-byte entries";
        return std::nullopt;
    }
    u32 next = 0x1000;
    for (u32 off = 0; off < size; off += 12U) {
        const u64 t = u64{be32(&data[off])} << 32 | be32(&data[off + 4U]);
        const u32 uid = be32(&data[off + 8U]);
        if (t == title) { nand.close(h); return uid; }
        next = std::max(next, uid + 1U);
    }
    const std::array<u8, 12> entry{static_cast<u8>(title >> 56), static_cast<u8>(title >> 48), static_cast<u8>(title >> 40),
                                   static_cast<u8>(title >> 32), static_cast<u8>(title >> 24), static_cast<u8>(title >> 16),
                                   static_cast<u8>(title >> 8),  static_cast<u8>(title),       static_cast<u8>(next >> 24),
                                   static_cast<u8>(next >> 16),  static_cast<u8>(next >> 8),   static_cast<u8>(next)};
    const bool ok = nand.write(h, entry) == 12;
    nand.close(h);
    if (!ok) { error = "uid.sys: write failed"; return std::nullopt; }
    return next;
}

DiDevice::DiDevice(hw::DiscInterface& drive, Es& es, NandFs* nand, disc::WiiKeys keys)
    : Device("/dev/di"), drive_(drive), es_(es), nand_(nand), keys_(std::move(keys)) {
    drive_.set_completion_listener([this](bool ok) {
        if (kernel_ != nullptr) drive_done(*kernel_, ok);
    });
}

Result DiDevice::open(Kernel& k, const Request&, const std::string&, u32) {
    kernel_ = &k;
    return Result::reply(err::kOk, kDefaultReplyTicks);
}
Result DiDevice::close(Kernel&, const Request&) { return Result::reply(err::kOk, kDefaultReplyTicks); }

bool DiDevice::program(Kernel& k, u32 cmd0, u32 cmd1, u32 cmd2, u32 dma_address, u32 dma_length, bool dma) {
    const bool ok = drive_.mmio_write(DICMD0, 4, cmd0) && drive_.mmio_write(DICMD1, 4, cmd1) &&
                    drive_.mmio_write(DICMD2, 4, cmd2) && drive_.mmio_write(DIMAR, 4, dma_address) &&
                    drive_.mmio_write(DILENGTH, 4, dma_length) &&
                    drive_.mmio_write(DICR, 4, dma ? (hw::di::DICR_TSTART | hw::di::DICR_DMA) : hw::di::DICR_TSTART);
    if (!ok) k.stop("IOS /dev/di: " + drive_.last_error());
    return ok;
}

bool DiDevice::open_partition(Kernel& k, u64 offset, std::string& error) {
    if (drive_.disc() == nullptr) { error = "no disc"; return false; }
    auto p = disc::WiiPartition::open(*drive_.disc(), offset, keys_, error);
    if (!p) return false;
    if (!es_.set_active_title(p->ticket_bytes(), p->tmd(), error)) return false;
    const auto& tmd = p->tmd();
    const u64 title = es_.active_title()->title_id();
    const u16 gid = static_cast<u16>(tmd[0x198] << 8 | tmd[0x199]);
    if (nand_ == nullptr) { error = "the NAND is needed to give the title an owner ID (uid.sys)"; return false; }
    const auto uid = uid_for_title(*nand_, title, error);
    if (!uid) return false;
    // the title's directories, as ES makes them for a title it runs: the data
    // directory belongs to the title
    {
        char hi[16], lo[16];
        std::snprintf(hi, sizeof hi, "%08x", static_cast<unsigned>(title >> 32));
        std::snprintf(lo, sizeof lo, "%08x", static_cast<unsigned>(title));
        const std::string base = std::string("/title/") + hi;
        const std::string dirs[] = {"/title", base, base + "/" + lo, base + "/" + lo + "/data"};
        for (std::size_t i = 0; i < 4; ++i) {
            FsMeta m;
            if (nand_->metadata(0, 0, dirs[i], m) == 0) continue;
            const bool data = i == 3;
            if (nand_->create(0, 0, dirs[i], 0, data ? FsModes{3, 3, 0} : FsModes{3, 3, 1}, false) != 0 ||
                (data && nand_->set_metadata(0, dirs[i], *uid, gid, 0, {3, 3, 0}) != 0)) {
                error = "cannot create " + dirs[i] + " on the NAND";
                return false;
            }
        }
    }
    k.set_ppc_credentials(*uid, gid);
    partition_ = std::move(p);
    return true;
}

Result DiDevice::ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) {
    kernel_ = &k;
    if (in.size != 0x20U) return Result::reply(di_res::kSecurity, kOverheadTicks);
    std::array<u8, 0x20> b{};
    if (!k.read_bytes(in.address, b)) return Result::stop("IOS /dev/di: input buffer is not in RAM");
    Pending p{r.address, number, out.address, out.size, {}};
    for (u32 i = 0; i < 7; ++i) p.args[i] = be32(&b[4U + 4U * i]);
    if (running_ || !queue_.empty()) {
        queue_.push_back(p);
        return Result::deferred();
    }
    if (auto res = start(k, p)) return *res;
    return Result::deferred();
}

std::optional<Result> DiDevice::start(Kernel& k, const Pending& p) {
    const auto done = [](s32 v) { return std::optional<Result>(Result::reply(v, kOverheadTicks)); };
    const auto write_out = [&](u32 v) -> std::optional<Result> {
        if (p.out_size < 4U) return done(di_res::kSecurity);
        if (!k.write32(p.out, v)) return Result::stop("IOS /dev/di: output buffer is not in RAM");
        return done(di_res::kSuccess);
    };
    const auto reg = [&](u32 off) {
        u64 v = 0;
        (void)drive_.mmio_read(off, 4, v);
        return static_cast<u32>(v);
    };
    const auto via_drive = [&](u32 c0, u32 c1, u32 c2, u32 dma_length, bool dma) -> std::optional<Result> {
        if (dma) {
            if (p.out_size < dma_length) return done(kTimedOut);
            if ((dma_length & 0x1FU) != 0U || (p.out & 0x1FU) != 0U) return done(di_res::kBadArgument);
            last_length_ = dma_length;
        }
        running_ = p;
        if (!program(k, c0, c1, c2, dma ? p.out : 0U, dma ? dma_length : 0U, dma)) {
            running_.reset();
            return Result::stop(k.stop_reason());
        }
        return std::nullopt;
    };
    const u8 sub7 = static_cast<u8>(p.args[0] & 0xFFU); // byte 7 of the input
    const u8 sub11 = static_cast<u8>(p.args[1] & 0xFFU); // byte 11
    switch (p.command) {
    case 0x12: return via_drive(0x12000000U, 0, 0, 0x20, true);
    case 0x70: return via_drive(0xA8000040U, 0, 0x20, 0x20, true);
    case 0x71: { // partition read
        const u32 length = p.args[0], position = p.args[1];
        if (!partition_) return done(di_res::kSecurity);
        if (p.out_size < length) return done(di_res::kSecurity);
        last_length_ = position; // IOS keeps the offset here (its own bug)
        if (!drive_.begin_host_read()) return done(di_res::kDriveError);
        const u64 at = u64{position} << 2;
        if (at > partition_->size() || length > partition_->size() - at) return done(di_res::kDriveError); // the drive's "block out of bounds"
        std::vector<u8> data(length);
        if (!partition_->read(at, data)) // inside the partition, but corrupt or missing in the image
            return Result::stop("IOS /dev/di: partition read at 0x" + hex(position) + " failed: " + partition_->last_error());
        if (!k.write_bytes(p.out, data)) return Result::stop("IOS /dev/di: read destination is not in RAM");
        return done(di_res::kSuccess);
    }
    case 0x7A: return write_out(reg(DICVR));
    case 0x83: return write_out(last_length_);
    case 0x84: return write_out(reg(DIIMMBUF));
    case 0x85: case 0x86: case 0x89: {
        const u32 cvr = reg(DICVR);
        u32 v = cvr & hw::di::DICVR_CVRINTMASK;
        if (p.command == 0x85) v = 0;
        if (p.command == 0x89) v = hw::di::DICVR_CVRINTMASK;
        if (p.command == 0x86) v |= hw::di::DICVR_CVRINT; // write 1 to clear
        if (!drive_.mmio_write(DICVR, 4, v)) return Result::stop("IOS /dev/di: " + drive_.last_error());
        return done(di_res::kSuccess);
    }
    case 0x87: return done(di_res::kSuccess);
    case 0x88: return write_out(drive_.disc() != nullptr ? 2U : 1U);
    case 0x8A:
        drive_.reset_drive(p.args[0] != 0U);
        return done(di_res::kSuccess);
    case 0x8B: return done(di_res::kSecurity); // only as ioctlv
    case 0x8C:
        partition_.reset();
        return done(di_res::kSuccess);
    case 0x8D: { // unencrypted read
        const u32 length = p.args[0], position = p.args[1];
        const u64 end = u64{position} + (length >> 2);
        struct Range { u32 start, end; };
        constexpr Range ranges[] = {{0, 0x14000}, {0x460A0000, 0x460A0008}, {0x7ED40000, 0x7ED40008}};
        for (const auto& rg : ranges)
            if (rg.start <= position && position <= rg.end && end <= rg.end) return via_drive(0xA8000000U, position, length, length, true);
        return done(di_res::kSecurity);
    }
    case 0x95: return write_out(reg(DISR));
    case 0x96: return write_out(reg(DICR));
    case 0xAB: return via_drive(0xAB000000U, p.args[0], 0, 0, false);
    case 0xE0: return via_drive(0xE0000000U, 0, 0, 0, false);
    case 0xE1: return via_drive(0xE1000000U | (u32{sub7} & 3U) << 16, p.args[2], p.args[1], 0, false);
    case 0xE2: return via_drive(0xE2000000U | (u32{sub7} & 3U) << 16, 0, 0, 0, false);
    case 0xE3: return via_drive(0xE3000000U | (u32{sub7} & 1U) << 17 | (u32{sub11} & 1U) << 20, 0, 0, 0, false);
    case 0xE4: return via_drive(0xE4000000U | (u32{sub7} & 1U) << 16 | (u32{sub11} & 0xFU), 0, 0, 0, false);
    default:
        return Result::stop("IOS /dev/di: request 0x" + hex(p.command).substr(6) + " is not emulated");
    }
}

void DiDevice::drive_done(Kernel& k, bool ok) {
    if (!running_) return; // a command Orca's own code started (none on Wii)
    const Pending p = *running_;
    running_.reset();
    // which results copy the immediate buffer (as IOS does): request error, stop motor, buffer config
    const bool copy_imm = p.command == 0xE0U || p.command == 0xE3U || p.command == 0xE4U;
    if (copy_imm && p.out_size >= 4U) {
        u64 imm = 0;
        (void)drive_.mmio_read(DIIMMBUF, 4, imm);
        if (!k.write32(p.out, static_cast<u32>(imm))) { k.stop("IOS /dev/di: output buffer is not in RAM"); return; }
    }
    (void)drive_.mmio_write(DISR, 4, hw::di::DISR_TCINT | hw::di::DISR_DEINT); // acknowledge
    k.reply(p.request, ok ? di_res::kSuccess : di_res::kDriveError, 0);
    next(k);
}

void DiDevice::next(Kernel& k) {
    while (!running_ && !queue_.empty() && !k.stopped()) {
        const Pending p = queue_.front();
        queue_.pop_front();
        auto res = start(k, p);
        if (!res) return; // waiting for the drive
        if (res->kind == Result::Kind::Stop) { k.stop(res->reason); return; }
        k.reply(p.request, res->value, res->delay_ticks);
    }
}

Result DiDevice::ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in, std::span<const IoVector> io) {
    kernel_ = &k;
    if (number != 0x8B) return Device::ioctlv(k, r, number, in, io);
    if (in.size() != 3U || io.size() != 2U || in[0].size < 8U) return Result::reply(di_res::kBadArgument, kDefaultReplyTicks);
    if (in[1].size != 0U || in[2].size != 0U)
        return Result::stop("IOS /dev/di: opening a partition with a ticket or certificates from memory is not emulated");
    u32 offset = 0;
    if (!k.read32(in[0].address + 4U, offset)) return Result::stop("IOS /dev/di: input buffer is not in RAM");
    std::string error;
    if (!open_partition(k, u64{offset} << 2, error)) return Result::stop("IOS /dev/di: open partition: " + error);
    const auto& tmd = partition_->tmd();
    if (io[0].size < tmd.size()) return Result::stop("IOS /dev/di: TMD buffer too small");
    if (!k.write_bytes(io[0].address, tmd) || !k.write32(io[1].address, 0U))
        return Result::stop("IOS /dev/di: output buffer is not in RAM");
    return Result::reply(di_res::kSuccess, kDefaultReplyTicks);
}

} // namespace orca::ios
