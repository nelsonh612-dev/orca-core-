#include "orca/ios/fs_device.hpp"

#include <algorithm>

namespace orca::ios {

namespace {
constexpr u64 kOverheadTicks = 2700;
constexpr u64 kClusterReadTicks = 115000;
constexpr u64 kClusterWriteTicks = 300000;
constexpr u64 kSuperblockTicks = 3170000;
constexpr u32 kParamsSize = 0x4A;
constexpr u32 kPathSize = 64;
constexpr u32 kUsableClusters = 0x7EC0 - 0x300, kReservedClusters = 0x300, kTotalInodes = 0x17FF;

u64 lookup_ticks(const std::string& path) {
    return 680U * static_cast<u64>(std::count(path.begin(), path.end(), '/'));
}
u64 clusters(u32 bytes) { return (u64{bytes} + NandFs::kClusterSize - 1U) / NandFs::kClusterSize; }
Result reply(s32 v, u64 extra = 0) { return Result::reply(v, kOverheadTicks + extra); }
Result not_ram() { return Result::stop("IOS /dev/fs: request buffer is not in RAM"); }

struct Params {
    u32 uid{0};
    u16 gid{0};
    std::string path;
    FsModes modes;
    u8 attribute{0};
};

bool read_path(Kernel& k, u32 address, std::string& out) {
    std::array<u8, kPathSize> b{};
    if (!k.read_bytes(address, b)) return false;
    const auto end = std::find(b.begin(), b.end(), u8{0});
    out.assign(b.begin(), end);
    return true;
}

bool read_params(Kernel& k, u32 address, Params& p) {
    std::array<u8, kParamsSize> b{};
    if (!k.read_bytes(address, b)) return false;
    p.uid = static_cast<u32>(b[0]) << 24 | static_cast<u32>(b[1]) << 16 | static_cast<u32>(b[2]) << 8 | b[3];
    p.gid = static_cast<u16>(b[4] << 8 | b[5]);
    const auto end = std::find(b.begin() + 6, b.begin() + 6 + kPathSize, u8{0});
    p.path.assign(b.begin() + 6, end);
    p.modes = {b[70], b[71], b[72]};
    p.attribute = b[73];
    return true;
}
} // namespace

std::optional<Result> FsDevice::credentials(Kernel& k, u32& uid, u16& gid) const {
    const auto c = k.ppc_credentials();
    if (!c) return Result::stop("IOS /dev/fs: Broadway's owner/group are not set (no title started)");
    uid = c->first;
    gid = c->second;
    return std::nullopt;
}

FsDevice::Open* FsDevice::slot(const Request& r) {
    if (r.fd >= kMaxFds || !open_[r.fd].used) return nullptr;
    return &open_[r.fd];
}

Result FsDevice::open(Kernel& k, const Request& r, const std::string& path, u32 mode) {
    u32 uid = 0;
    u16 gid = 0;
    if (auto e = credentials(k, uid, gid)) return *e;
    if (r.fd >= kMaxFds) return Result::stop("IOS /dev/fs: descriptor out of range");
    if (path == "/dev/fs") {
        open_[r.fd] = Open{true, -1, uid, gid, false};
        return reply(err::kOk);
    }
    const s32 h = nand_.open(uid, gid, path, static_cast<u8>(mode & 3U));
    if (h < 0) return reply(h, lookup_ticks(path));
    open_[r.fd] = Open{true, h, uid, gid, false};
    return reply(err::kOk, lookup_ticks(path));
}

Result FsDevice::close(Kernel&, const Request& r) {
    Open* o = slot(r);
    if (o == nullptr) return reply(fs_err::kInvalid);
    s32 res = 0;
    const bool written = o->written;
    if (o->file >= 0) res = nand_.close(o->file);
    *o = Open{};
    return reply(res, written ? kSuperblockTicks : 0U);
}

Result FsDevice::read(Kernel& k, const Request& r, u32 buffer, u32 size) {
    Open* o = slot(r);
    if (o == nullptr || o->file < 0) return reply(fs_err::kInvalid);
    u32 file_size = 0, pos = 0;
    const s32 st = nand_.status(o->file, file_size, pos);
    if (st < 0) return reply(st);
    std::vector<u8> data(pos >= file_size ? 0U : std::min(size, file_size - pos));
    const s32 n = nand_.read(o->file, data);
    if (n < 0) return reply(n);
    if (!k.write_bytes(buffer, std::span<const u8>(data.data(), static_cast<std::size_t>(n)))) return not_ram();
    return reply(n, clusters(static_cast<u32>(n)) * kClusterReadTicks);
}

Result FsDevice::write(Kernel& k, const Request& r, u32 buffer, u32 size) {
    Open* o = slot(r);
    if (o == nullptr || o->file < 0) return reply(fs_err::kInvalid);
    std::vector<u8> data(size);
    if (!k.read_bytes(buffer, data)) return not_ram();
    const s32 n = nand_.write(o->file, data);
    if (n < 0) return reply(n);
    if (n > 0) o->written = true;
    return reply(n, clusters(static_cast<u32>(n)) * kClusterWriteTicks);
}

Result FsDevice::seek(Kernel&, const Request& r, s32 offset, u32 whence) {
    Open* o = slot(r);
    if (o == nullptr || o->file < 0) return reply(fs_err::kInvalid);
    return reply(nand_.seek(o->file, offset, whence));
}

Result FsDevice::ioctl(Kernel& k, const Request& r, u32 number, IoVector in, IoVector out) {
    Open* o = slot(r);
    if (o == nullptr) return reply(fs_err::kInvalid);
    switch (number) {
    case 2: { // NAND statistics
        if (out.size < 0x1CU) return reply(fs_err::kInvalid);
        u32 used = 0, inodes = 0;
        const s32 res = nand_.usage("/", used, inodes);
        if (res < 0) return reply(res);
        used = std::min(used, kUsableClusters);
        inodes = std::min(inodes, kTotalInodes);
        const u32 words[7] = {NandFs::kClusterSize, kUsableClusters - used, used, 0, kReservedClusters,
                              kTotalInodes - inodes, inodes};
        for (u32 i = 0; i < 7; ++i)
            if (!k.write32(out.address + 4U * i, words[i])) return not_ram();
        return reply(err::kOk);
    }
    case 3:   // create directory
    case 9:   // create file
    case 5: { // set attributes
        if (in.size < kParamsSize) return reply(fs_err::kInvalid);
        Params p;
        if (!read_params(k, in.address, p)) return not_ram();
        s32 res = 0;
        if (number == 5) res = nand_.set_metadata(o->uid, p.path, p.uid, p.gid, p.attribute, p.modes);
        else res = nand_.create(o->uid, o->gid, p.path, p.attribute, p.modes, number == 9);
        return reply(res, res == 0 ? kSuperblockTicks : 0U);
    }
    case 6: { // get attributes
        if (in.size < kPathSize || out.size < kParamsSize) return reply(fs_err::kInvalid);
        std::string path;
        if (!read_path(k, in.address, path)) return not_ram();
        FsMeta m;
        const s32 res = nand_.metadata(o->uid, o->gid, path, m);
        const u64 t = 1000U + 340U * static_cast<u64>(std::count(path.begin(), path.end(), '/'));
        if (res < 0) return reply(res, t);
        std::array<u8, kParamsSize> b{};
        b[0] = static_cast<u8>(m.uid >> 24); b[1] = static_cast<u8>(m.uid >> 16);
        b[2] = static_cast<u8>(m.uid >> 8); b[3] = static_cast<u8>(m.uid);
        b[4] = static_cast<u8>(m.gid >> 8); b[5] = static_cast<u8>(m.gid);
        b[70] = m.modes.owner; b[71] = m.modes.group; b[72] = m.modes.other;
        b[73] = m.attribute;
        if (!k.write_bytes(out.address, b)) return not_ram();
        return reply(err::kOk, t);
    }
    case 7: { // delete
        if (in.size < kPathSize) return reply(fs_err::kInvalid);
        std::string path;
        if (!read_path(k, in.address, path)) return not_ram();
        return reply(nand_.remove(o->uid, o->gid, path), kSuperblockTicks);
    }
    case 8: { // rename
        if (in.size < 2U * kPathSize) return reply(fs_err::kInvalid);
        std::string from, to;
        if (!read_path(k, in.address, from) || !read_path(k, in.address + kPathSize, to)) return not_ram();
        return reply(nand_.rename(o->uid, o->gid, from, to), kSuperblockTicks);
    }
    case 11: { // file status
        if (out.size < 8U || o->file < 0) return reply(fs_err::kInvalid);
        u32 size = 0, pos = 0;
        const s32 res = nand_.status(o->file, size, pos);
        if (res < 0) return reply(res);
        if (!k.write32(out.address, size) || !k.write32(out.address + 4U, pos)) return not_ram();
        return reply(err::kOk);
    }
    case 13: // shutdown: every write already reached the host
        return reply(err::kOk);
    default:
        return Device::ioctl(k, r, number, in, out);
    }
}

Result FsDevice::ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in,
                        std::span<const IoVector> io) {
    Open* o = slot(r);
    if (o == nullptr) return reply(fs_err::kInvalid);
    if (number == 4) { // read directory
        if (in.empty() || in.size() != io.size() || in.size() > 2U || in[0].size != kPathSize) return reply(fs_err::kInvalid);
        std::string path;
        if (!read_path(k, in[0].address, path)) return not_ram();
        u32 max = 0;
        u32 names_at = 0, count_at = 0;
        if (in.size() == 2U) {
            if (in[1].size != 4U || io[1].size != 4U) return reply(fs_err::kInvalid);
            if (!k.read32(in[1].address, max)) return not_ram();
            if (io[0].size != 13U * max) return reply(fs_err::kInvalid);
            names_at = io[0].address;
            count_at = io[1].address;
        } else {
            if (io[0].size != 4U) return reply(fs_err::kInvalid);
            count_at = io[0].address;
        }
        std::vector<std::string> names;
        const s32 res = nand_.list(o->uid, o->gid, path, names);
        if (res < 0) return reply(res);
        if (names_at == 0U) {
            if (!k.write32(count_at, static_cast<u32>(names.size()))) return not_ram();
            return reply(err::kOk);
        }
        const u32 n = std::min<u32>(max, static_cast<u32>(names.size()));
        std::vector<u8> packed;
        for (u32 i = 0; i < n; ++i) {
            packed.insert(packed.end(), names[i].begin(), names[i].end());
            packed.push_back(0);
        }
        if (!k.write_bytes(names_at, packed) || !k.write32(count_at, n)) return not_ram();
        return reply(err::kOk);
    }
    if (number == 12) { // usage
        if (in.size() != 1U || io.size() != 2U || in[0].size != kPathSize || io[0].size != 4U || io[1].size != 4U)
            return reply(fs_err::kInvalid);
        std::string path;
        if (!read_path(k, in[0].address, path)) return not_ram();
        u32 used = 0, inodes = 0;
        const s32 res = nand_.usage(path, used, inodes);
        if (res < 0) return reply(res);
        if (!k.write32(io[0].address, std::min(used, kUsableClusters)) || !k.write32(io[1].address, std::min(inodes, kTotalInodes)))
            return not_ram();
        return reply(err::kOk);
    }
    return Device::ioctlv(k, r, number, in, io);
}

} // namespace orca::ios
