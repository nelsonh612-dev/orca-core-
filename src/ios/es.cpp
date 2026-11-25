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

std::optional<std::vector<u8>> Es::read_nand_file(const std::string& path) const {
    if (nand_ == nullptr) return std::nullopt;
    const s32 h = nand_->open(0, 0, path, kFsRead);
    if (h < 0) return std::nullopt;
    u32 size = 0, pos = 0;
    nand_->status(h, size, pos);
    std::vector<u8> data(size);
    const s32 n = nand_->read(h, data);
    nand_->close(h);
    if (n != static_cast<s32>(size)) return std::nullopt;
    return data;
}

namespace {
bool parse_hex32(const std::string& s, u32& v) {
    if (s.size() != 8U) return false;
    v = 0;
    for (const char c : s) {
        u32 d = 0;
        if (c >= '0' && c <= '9') d = static_cast<u32>(c - '0');
        else if (c >= 'a' && c <= 'f') d = static_cast<u32>(c - 'a' + 10);
        else return false; // IOS names title directories in lower case
        v = v << 4 | d;
    }
    return true;
}
} // namespace

std::vector<u64> Es::installed_titles() const {
    std::vector<u64> out;
    if (nand_ == nullptr) return out;
    std::vector<std::string> highs;
    if (nand_->list(0, 0, "/title", highs) < 0) return out;
    for (const auto& h : highs) {
        u32 hi = 0;
        if (!parse_hex32(h, hi)) continue;
        std::vector<std::string> lows;
        if (nand_->list(0, 0, "/title/" + h, lows) < 0) continue;
        for (const auto& l : lows) {
            u32 lo = 0;
            if (!parse_hex32(l, lo)) continue;
            FsMeta meta;
            if (nand_->metadata(0, 0, "/title/" + h + "/" + l + "/content/title.tmd", meta) < 0) continue;
            out.push_back(u64{hi} << 32 | lo);
        }
    }
    return out;
}

std::vector<u64> Es::owned_titles() const {
    std::vector<u64> out;
    if (nand_ == nullptr) return out;
    std::vector<std::string> highs;
    if (nand_->list(0, 0, "/ticket", highs) < 0) return out;
    for (const auto& h : highs) {
        u32 hi = 0;
        if (!parse_hex32(h, hi)) continue;
        std::vector<std::string> files;
        if (nand_->list(0, 0, "/ticket/" + h, files) < 0) continue;
        for (const auto& f : files) {
            u32 lo = 0;
            if (f.size() != 12U || f.substr(8) != ".tik" || !parse_hex32(f.substr(0, 8), lo)) continue;
            out.push_back(u64{hi} << 32 | lo);
        }
    }
    return out;
}

std::optional<std::vector<u8>> Es::stored_tmd(u64 id) const {
    char path[48];
    std::snprintf(path, sizeof path, "/title/%08x/%08x/content/title.tmd", static_cast<unsigned>(id >> 32), static_cast<unsigned>(id));
    auto tmd = read_nand_file(path);
    if (!tmd || !tmd_content_count(*tmd)) return std::nullopt;
    return tmd;
}

Result Es::open(Kernel&, const Request&, const std::string&, u32) { return Result::reply(err::kOk, kDefaultReplyTicks); }
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
    case 0x07: { // device ID
        if (!vectors_ok(in, io, 0, 1) || io[0].size != 4U) return inval();
        if (!console_id_)
            return Result::stop("IOS /dev/es: GetDeviceID needs the console ID, which comes from the user's keys.bin "
                                "(not imported)");
        if (!k.write32(io[0].address, *console_id_)) return ram();
        return ok();
    }
    case 0x0C: case 0x0E: { // owned / installed title count
        if (!vectors_ok(in, io, 0, 1) || io[0].size != 4U) return inval();
        if (nand_ == nullptr) return Result::stop("IOS /dev/es: title list without a NAND");
        const auto t = number == 0x0C ? owned_titles() : installed_titles();
        if (!k.write32(io[0].address, static_cast<u32>(t.size()))) return ram();
        return ok();
    }
    case 0x0D: case 0x0F: { // owned / installed titles
        if (!vectors_ok(in, io, 1, 1) || in[0].size != 4U) return inval();
        if (nand_ == nullptr) return Result::stop("IOS /dev/es: title list without a NAND");
        u32 max = 0;
        if (!k.read32(in[0].address, max)) return ram();
        const auto t = number == 0x0D ? owned_titles() : installed_titles();
        for (std::size_t i = 0; i < t.size() && i < max && (i + 1U) * 8U <= io[0].size; ++i) {
            const u32 a = io[0].address + static_cast<u32>(i) * 8U;
            if (!k.write32(a, static_cast<u32>(t[i] >> 32)) || !k.write32(a + 4U, static_cast<u32>(t[i]))) return ram();
        }
        return ok();
    }
    case 0x34: case 0x35: { // stored TMD size / stored TMD
        if (!vectors_ok(in, io, number == 0x34 ? 1U : 2U, 1) || in[0].size != 8U) return inval();
        if (nand_ == nullptr) return Result::stop("IOS /dev/es: stored TMD without a NAND");
        u64 id = 0;
        if (!read64(in[0].address, id)) return ram();
        const auto tmd = stored_tmd(id);
        if (!tmd) return Result::reply(es_err::kFsNotFound, kDefaultReplyTicks);
        if (number == 0x34) {
            if (io[0].size != 4U) return inval();
            if (!k.write32(io[0].address, static_cast<u32>(tmd->size()))) return ram();
            return ok();
        }
        if (io[0].size != tmd->size()) return inval();
        if (!k.write_bytes(io[0].address, *tmd)) return ram();
        return ok();
    }
    case 0x2E: { // boot2 version
        if (!vectors_ok(in, io, 0, 1) || io[0].size < 4U) return inval();
        if (!boot2_version_)
            return Result::stop("IOS /dev/es: GetBoot2Version needs the console's boot2 version, which comes from the "
                                "user's keys.bin (not imported)");
        if (!k.write32(io[0].address, *boot2_version_)) return ram();
        return ok();
    }
    case 0x45: // Korea check (IOS70+): -1017 without the Korean common key
        if (!vectors_ok(in, io, 0, 0)) return inval();
        return korean_key_ ? ok() : inval();
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
