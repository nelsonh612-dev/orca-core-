#include "orca/ios/es.hpp"

#include <cstdio>

namespace orca::ios {

namespace {
constexpr u32 kTmdHeaderSize = 0x1E4;
constexpr u32 kTmdContentSize = 0x24;
constexpr u32 kTmdContentView = 0x10;
constexpr u32 kTicketSize = 0x2A4;
constexpr u32 kTicketViewSize = 0xD8;
constexpr u32 kMaxDiTmd = 4U * 1024U * 1024U;

u16 be16(const u8* p) { return static_cast<u16>(p[0] << 8 | p[1]); }
u32 be32(const u8* p) { return static_cast<u32>(p[0]) << 24 | static_cast<u32>(p[1]) << 16 | static_cast<u32>(p[2]) << 8 | p[3]; }
u64 be64(const u8* p) { return u64{be32(p)} << 32 | be32(p + 4); }

std::string hex64(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llX", static_cast<unsigned long long>(v));
    return b;
}

std::vector<u8> ticket_view_of(std::span<const u8> ticket) {
    std::vector<u8> v(4, 0);
    v[0] = ticket[0x1BC];
    v.insert(v.end(), ticket.begin() + 0x1D0, ticket.begin() + 0x1D0 + (kTicketViewSize - 4));
    return v;
}

// Every vector count matches and each vector with a length has an address.
bool vectors_ok(std::span<const IoVector> in, std::span<const IoVector> io, std::size_t n_in, std::size_t n_io) {
    if (in.size() != n_in || io.size() != n_io) return false;
    for (const auto& v : in) if (v.size != 0U && v.address == 0U) return false;
    for (const auto& v : io) if (v.size != 0U && v.address == 0U) return false;
    return true;
}
} // namespace

std::optional<u32> tmd_content_count(std::span<const u8> tmd) {
    if (tmd.size() < kTmdHeaderSize) return std::nullopt;
    const u32 n = be16(&tmd[0x1DE]);
    if (tmd.size() < kTmdHeaderSize + u64{n} * kTmdContentSize) return std::nullopt;
    return n;
}

std::vector<u8> tmd_view(std::span<const u8> tmd) {
    const auto n = tmd_content_count(tmd);
    if (!n) return {};
    std::vector<u8> v(tmd.begin() + 0x180, tmd.begin() + 0x1D8);
    v.insert(v.end(), tmd.begin() + 0x1DC, tmd.begin() + 0x1E0); // title version, content count
    for (u32 i = 0; i < *n; ++i) {
        const auto c = tmd.begin() + kTmdHeaderSize + i * kTmdContentSize;
        v.insert(v.end(), c, c + kTmdContentView);
    }
    return v;
}

u64 TitleContext::title_id() const { return be64(&tmd[0x18C]); }
std::vector<u8> TitleContext::ticket_view() const { return ticket_view_of(ticket); }

bool Es::set_active_title(std::vector<u8> ticket, std::vector<u8> tmd, std::string& error) {
    if (ticket.size() != kTicketSize) { error = "ES: a ticket is 0x2A4 bytes"; return false; }
    if (!tmd_content_count(tmd)) { error = "ES: the TMD is truncated"; return false; }
    if (be64(&ticket[0x1DC]) != be64(&tmd[0x18C])) {
        error = "ES: ticket title " + hex64(be64(&ticket[0x1DC])) + " does not match TMD title " + hex64(be64(&tmd[0x18C]));
        return false;
    }
    title_ = TitleContext{std::move(ticket), std::move(tmd)};
    return true;
}

Result Es::open(Kernel&, const Request&, u32) { return Result::reply(err::kOk, kDefaultReplyTicks); }
Result Es::close(Kernel&, const Request&) { return Result::reply(err::kOk, kDefaultReplyTicks); }

Result Es::ioctlv(Kernel& k, const Request& r, u32 number, std::span<const IoVector> in, std::span<const IoVector> io) {
    const auto ok = [] { return Result::reply(err::kOk, kDefaultReplyTicks); };
    const auto inval = [] { return Result::reply(es_err::kInvalid, kDefaultReplyTicks); };
    const auto ram = [] { return Result::stop("IOS /dev/es: request buffer is not in RAM"); };
    const auto read64 = [&k](u32 a, u64& v) {
        u32 hi = 0, lo = 0;
        if (!k.read32(a, hi) || !k.read32(a + 4U, lo)) return false;
        v = u64{hi} << 32 | lo;
        return true;
    };
    // the title a request names must be the active one (no NAND title store)
    const auto named_title = [&](u64& id) -> std::optional<Result> {
        if (in.empty() || in[0].size != 8U) return inval();
        if (!read64(in[0].address, id)) return ram();
        if (!title_ || title_->title_id() != id)
            return Result::stop("IOS /dev/es: ioctlv 0x" + hex64(number).substr(14) + " for title " + hex64(id) +
                                ", which is not the active title (the NAND title store is not emulated)");
        return std::nullopt;
    };

    switch (number) {
    case 0x20: { // title ID
        if (!vectors_ok(in, io, 0, 1) || io[0].size != 8U) return inval();
        if (!title_) return inval();
        const u64 id = title_->title_id();
        if (!k.write32(io[0].address, static_cast<u32>(id >> 32)) || !k.write32(io[0].address + 4U, static_cast<u32>(id)))
            return ram();
        return ok();
    }
    case 0x1D: { // title data directory
        if (!vectors_ok(in, io, 1, 1) || in[0].size != 8U) return inval();
        u64 id = 0;
        if (!read64(in[0].address, id)) return ram();
        char path[32];
        std::snprintf(path, sizeof path, "/title/%08x/%08x/data", static_cast<unsigned>(id >> 32), static_cast<unsigned>(id));
        const std::string s(path);
        if (io[0].size < s.size() + 1U)
            return Result::stop("IOS /dev/es: title directory buffer of " + std::to_string(io[0].size) + " bytes");
        std::vector<u8> b(s.begin(), s.end());
        b.push_back(0);
        if (!k.write_bytes(io[0].address, b)) return ram();
        return ok();
    }
    case 0x12: { // ticket view count
        if (!vectors_ok(in, io, 1, 1) || io[0].size != 4U) return inval();
        u64 id = 0;
        if (auto e = named_title(id)) return *e;
        if (!k.write32(io[0].address, 1U)) return ram();
        return ok();
    }
    case 0x13: { // ticket views
        if (!vectors_ok(in, io, 2, 1) || in[1].size != 4U) return inval();
        u64 id = 0;
        if (auto e = named_title(id)) return *e;
        u32 max = 0;
        if (!k.read32(in[1].address, max)) return ram();
        if (max == 0U) return ok();
        if (io[0].size < kTicketViewSize) return inval();
        if (!k.write_bytes(io[0].address, title_->ticket_view())) return ram();
        return ok();
    }
    case 0x14: { // TMD view size
        if (!vectors_ok(in, io, 1, 1) || io[0].size != 4U) return inval();
        u64 id = 0;
        if (auto e = named_title(id)) return *e;
        if (!k.write32(io[0].address, static_cast<u32>(tmd_view(title_->tmd).size()))) return ram();
        return ok();
    }
    case 0x15: { // TMD view
        if (!vectors_ok(in, io, 2, 1) || in[1].size != 4U) return inval();
        u32 size = 0;
        if (!k.read32(in[1].address, size)) return ram();
        if (size != io[0].size) return inval();
        u64 id = 0;
        if (auto e = named_title(id)) return *e;
        const auto view = tmd_view(title_->tmd);
        if (io[0].size < view.size()) return inval();
        if (!k.write_bytes(io[0].address, view)) return ram();
        return ok();
    }
    case 0x16: { // consumption: no limit entries
        if (!vectors_ok(in, io, 1, 2) || io[1].size != 4U) return inval();
        if (!k.write32(io[1].address, 0U)) return ram();
        return ok();
    }
    case 0x19:   // DI TMD view size
    case 0x1A: { // DI TMD view
        if (number == 0x19 ? !vectors_ok(in, io, 1, 1) : !vectors_ok(in, io, 2, 1)) return inval();
        if (in[0].size >= kMaxDiTmd) return inval();
        if (number == 0x19 && io[0].size != 4U) return inval();
        if (number == 0x1A) {
            u32 size = 0;
            if (in[1].size != 4U) return inval();
            if (!k.read32(in[1].address, size)) return ram();
            if (size != io[0].size) return inval();
        }
        std::vector<u8> view;
        if (in[0].size != 0U) {
            std::vector<u8> tmd(in[0].size);
            if (!k.read_bytes(in[0].address, tmd)) return ram();
            view = tmd_view(tmd);
            if (view.empty()) return inval();
        } else {
            if (!title_) return inval();
            view = tmd_view(title_->tmd);
        }
        if (number == 0x19) {
            if (!k.write32(io[0].address, static_cast<u32>(view.size()))) return ram();
            return ok();
        }
        if (io[0].size < view.size()) return inval();
        if (!k.write_bytes(io[0].address, view)) return ram();
        return ok();
    }
    case 0x1B: { // DI ticket view
        if (!vectors_ok(in, io, 1, 1) || io[0].size != kTicketViewSize) return inval();
        std::vector<u8> view;
        if (in[0].size == kTicketSize) {
            std::vector<u8> t(kTicketSize);
            if (!k.read_bytes(in[0].address, t)) return ram();
            view = ticket_view_of(t);
        } else if (in[0].size == 0U) {
            if (!title_) return inval();
            view = title_->ticket_view();
        } else {
            return inval();
        }
        if (!k.write_bytes(io[0].address, view)) return ram();
        return ok();
    }
    case 0x39: { // DI TMD size
        if (!vectors_ok(in, io, 0, 1) || io[0].size != 4U || !title_) return inval();
        if (!k.write32(io[0].address, static_cast<u32>(title_->tmd.size()))) return ram();
        return ok();
    }
    case 0x3A: { // DI TMD
        if (!vectors_ok(in, io, 1, 1) || in[0].size != 4U) return inval();
        u32 size = 0;
        if (!k.read32(in[0].address, size)) return ram();
        if (size != io[0].size || !title_ || title_->tmd.size() > size) return inval();
        if (!k.write_bytes(io[0].address, title_->tmd)) return ram();
        return ok();
    }
    default:
        return Device::ioctlv(k, r, number, in, io);
    }
}

} // namespace orca::ios
