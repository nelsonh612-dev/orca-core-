#include "orca/disc/disc_image.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <cstring>

namespace orca::disc {
namespace {
u32 be32(const u8* p) { return (u32{p[0]} << 24U) | (u32{p[1]} << 16U) | (u32{p[2]} << 8U) | p[3]; }
} // namespace

bool MemoryDisc::read(u64 offset, std::span<u8> out) {
    if (offset > data_.size() || out.size() > data_.size() - offset) return false;
    std::memcpy(out.data(), data_.data() + offset, out.size());
    return true;
}

std::unique_ptr<FileDisc> FileDisc::open(const std::string& path, std::string& error) {
    auto d = std::unique_ptr<FileDisc>(new FileDisc());
    d->file_.open(path, std::ios::binary);
    if (!d->file_) { error = "cannot open disc image " + path; return nullptr; }
    d->file_.seekg(0, std::ios::end);
    d->size_ = static_cast<u64>(d->file_.tellg());
    return d;
}

bool FileDisc::read(u64 offset, std::span<u8> out) {
    if (offset > size_ || out.size() > size_ - offset) return false;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(file_.gcount()) == out.size();
}

#if defined(__unix__) || defined(__APPLE__)
std::unique_ptr<FdDisc> FdDisc::open(int fd, std::string& error) {
    struct stat st{};
    if (fd < 0 || ::fstat(fd, &st) != 0 || st.st_size <= 0) {
        if (fd >= 0) ::close(fd);
        error = "disc: the file descriptor is not a readable file";
        return nullptr;
    }
    return std::unique_ptr<FdDisc>(new FdDisc(fd, static_cast<u64>(st.st_size)));
}

FdDisc::~FdDisc() { ::close(fd_); }

bool FdDisc::read(u64 offset, std::span<u8> out) {
    if (offset > size_ || out.size() > size_ - offset) return false;
    std::size_t done = 0;
    while (done < out.size()) {
        const ssize_t n = ::pread(fd_, out.data() + done, out.size() - done, static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}
#endif

namespace {
std::optional<DiscHeader> read_header(DiscReader& disc, std::string& error) {
    std::array<u8, 0x440> h{};
    if (!disc.read(0, h)) { error = "disc: image smaller than the 0x440-byte header"; return std::nullopt; }
    DiscHeader d;
    d.game_id.assign(reinterpret_cast<const char*>(h.data()), 6);
    d.disc_number = h[6];
    d.version = h[7];
    d.wii = be32(&h[0x18]) == kWiiMagic;
    d.gamecube = be32(&h[0x1C]) == kGameCubeMagic;
    const char* name = reinterpret_cast<const char*>(&h[0x20]);
    d.name.assign(name, strnlen(name, 0x3E0));
    d.dol_offset = be32(&h[0x420]);
    d.fst_offset = be32(&h[0x424]);
    d.fst_size = be32(&h[0x428]);
    d.fst_max_size = be32(&h[0x42C]);
    return d;
}
} // namespace

std::optional<DiscHeader> parse_header(DiscReader& disc, std::string& error) {
    auto d = read_header(disc, error);
    if (!d) return std::nullopt;
    if (d->wii) return d; // the game's DOL and FST are inside its partition
    if (!d->gamecube) { error = "disc: neither the GameCube magic 0xC2339F3D at 0x1C nor the Wii magic 0x5D1C9EA3 at 0x18"; return std::nullopt; }
    if (d->dol_offset == 0 || d->dol_offset >= disc.size()) { error = "disc: main DOL offset outside the image"; return std::nullopt; }
    return d;
}

std::optional<DiscHeader> parse_volume_header(DiscReader& volume, bool wii, std::string& error) {
    auto d = read_header(volume, error);
    if (!d) return std::nullopt;
    if (wii) {
        if (!d->wii) { error = "Wii partition: no Wii magic at 0x18 of the decrypted data (wrong key or not a game partition)"; return std::nullopt; }
        d->offset_shift = 2;
        d->dol_offset <<= 2;
        d->fst_offset <<= 2;
        d->fst_size <<= 2;
        d->fst_max_size <<= 2;
    } else if (!d->gamecube) {
        error = "disc: GameCube magic 0xC2339F3D not found at 0x1C";
        return std::nullopt;
    }
    const u64 size = volume.size();
    if (d->dol_offset < 0x2440U || d->dol_offset >= size) { error = "disc: main DOL offset outside the volume"; return std::nullopt; }
    if (d->fst_size != 0U && (d->fst_offset > size || d->fst_size > size - d->fst_offset)) {
        error = "disc: FST outside the volume";
        return std::nullopt;
    }
    return d;
}

std::optional<ApploaderHeader> parse_apploader_header(DiscReader& disc, std::string& error) {
    std::array<u8, 0x20> h{};
    if (!disc.read(kApploaderOffset, h)) { error = "disc: no apploader header at 0x2440"; return std::nullopt; }
    ApploaderHeader a;
    const char* date = reinterpret_cast<const char*>(h.data());
    a.date.assign(date, strnlen(date, 16));
    a.entry = be32(&h[0x10]);
    a.size = be32(&h[0x14]);
    a.trailer_size = be32(&h[0x18]);
    const u64 total = u64{a.size} + a.trailer_size;
    if (a.size == 0 || total > 0x00600000U) { error = "disc: implausible apploader size"; return std::nullopt; }
    if (!(u64{kApploaderOffset} + 0x20U + total <= disc.size())) { error = "disc: apploader extends past the image"; return std::nullopt; }
    if (a.entry < kApploaderLoadAddress || a.entry >= kApploaderLoadAddress + total) {
        error = "disc: apploader entry point outside the loaded apploader"; return std::nullopt;
    }
    return a;
}

} // namespace orca::disc
