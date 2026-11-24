// Orca Core — disc image formats. See blob.hpp.
#include "orca/disc/blob.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#if defined(ORCA_HAVE_ZLIB)
#include <zlib.h>
#endif

namespace orca::disc {

namespace {
u32 le32(const u8* p) noexcept { return u32{p[0]} | u32{p[1]} << 8 | u32{p[2]} << 16 | u32{p[3]} << 24; }
u64 le64(const u8* p) noexcept { return u64{le32(p)} | u64{le32(p + 4)} << 32; }
u32 be32(const u8* p) noexcept { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
u16 be16(const u8* p) noexcept { return static_cast<u16>(p[0] << 8 | p[1]); }
bool inside(u64 offset, u64 length, u64 limit) noexcept { return offset <= limit && length <= limit - offset; }
std::string hex(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llX", static_cast<unsigned long long>(v));
    return b;
}

constexpr u64 kWiiDualLayerSize = 143432ULL * 2U * 0x8000U; // what Dolphin assumes for WBFS images

// --- CISO ---------------------------------------------------------------------
constexpr u32 kCisoHeader = 0x8000, kCisoMap = kCisoHeader - 8U;

class CisoReader final : public DiscReader {
public:
    static std::unique_ptr<CisoReader> open(std::unique_ptr<DiscReader> file, std::string& error) {
        std::vector<u8> h(kCisoHeader);
        if (!file->read(0, h)) { error = "CISO: header truncated"; return nullptr; }
        const u32 block = le32(&h[4]);
        if (block == 0U || block > (1U << 30) || (block & (block - 1U)) != 0U) { error = "CISO: block size " + hex(block) + " is not a power of two"; return nullptr; }
        std::unique_ptr<CisoReader> r(new CisoReader(std::move(file), block));
        u32 stored = 0;
        for (u32 i = 0; i < kCisoMap; ++i) {
            const u8 m = h[8 + i];
            if (m > 1U) { error = "CISO: invalid map entry " + std::to_string(m) + " for block " + std::to_string(i); return nullptr; }
            r->map_[i] = m == 1U ? static_cast<s32>(stored++) : -1;
        }
        if (stored == 0U) { error = "CISO: no blocks stored"; return nullptr; }
        const u64 need = kCisoHeader + u64{stored} * block;
        if (r->file_->size() < need) {
            error = "CISO: " + std::to_string(stored) + " stored blocks need " + hex(need) + " bytes, the file has " + hex(r->file_->size()) + " (truncated)";
            return nullptr;
        }
        return r;
    }
    u64 size() const override { return u64{kCisoMap} * block_; } // upper bound, as Dolphin
    bool read(u64 offset, std::span<u8> out) override {
        if (!inside(offset, out.size(), size())) return false;
        std::size_t done = 0;
        while (done < out.size()) {
            const u64 at = offset + done, index = at / block_, in = at % block_;
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(out.size() - done, block_ - in));
            const s32 slot = map_[index];
            if (slot < 0) std::memset(out.data() + done, 0, n);
            else if (!file_->read(kCisoHeader + u64{static_cast<u32>(slot)} * block_ + in, out.subspan(done, n))) {
                error_ = "CISO: block " + std::to_string(index) + " is missing from the file (truncated image)";
                return false;
            }
            done += n;
        }
        return true;
    }

private:
    std::string failure_reason() const override { return error_; }
    CisoReader(std::unique_ptr<DiscReader> f, u32 block) : file_(std::move(f)), block_(block), map_(kCisoMap, -1) {}
    std::unique_ptr<DiscReader> file_;
    u32 block_;
    std::vector<s32> map_;
    std::string error_;
};

// --- WBFS ---------------------------------------------------------------------
class WbfsReader final : public DiscReader {
public:
    static std::unique_ptr<WbfsReader> open(std::unique_ptr<DiscReader> file, std::string& error) {
        u8 h[12];
        if (!file->read(0, h)) { error = "WBFS: header truncated"; return nullptr; }
        const u32 hd_count = be32(&h[4]);
        const unsigned hd_shift = h[8], wbfs_shift = h[9];
        if (hd_shift < 9U || hd_shift > 16U || wbfs_shift < 15U || wbfs_shift > 30U) {
            error = "WBFS: sector shifts " + std::to_string(hd_shift) + "/" + std::to_string(wbfs_shift) + " are invalid";
            return nullptr;
        }
        const u64 hd_size = u64{1} << hd_shift, wbfs_size = u64{1} << wbfs_shift;
        if (u64{hd_count} * hd_size != file->size()) {
            error = "WBFS: the header describes " + hex(u64{hd_count} * hd_size) + " bytes but the file has " + hex(file->size()) +
                    " (split .wbf1 files or truncated images are not supported)";
            return nullptr;
        }
        u8 table0 = 0;
        if (!file->read(12, std::span<u8>(&table0, 1)) || table0 == 0U) { error = "WBFS: disc slot 0 is empty"; return nullptr; }
        const u64 blocks = (kWiiDualLayerSize + wbfs_size - 1U) / wbfs_size;
        std::vector<u8> raw(static_cast<std::size_t>(blocks) * 2U);
        if (!file->read(hd_size + 0x100U, raw)) { error = "WBFS: block table truncated"; return nullptr; }
        std::unique_ptr<WbfsReader> r(new WbfsReader(std::move(file), wbfs_size));
        r->table_.resize(static_cast<std::size_t>(blocks));
        const u64 file_blocks = r->file_->size() / wbfs_size;
        for (std::size_t i = 0; i < r->table_.size(); ++i) {
            r->table_[i] = be16(&raw[2 * i]);
            if (r->table_[i] != 0U && r->table_[i] >= file_blocks) { error = "WBFS: block " + std::to_string(i) + " points past the file"; return nullptr; }
        }
        if (r->table_[0] == 0U) { error = "WBFS: the disc's first block is not stored"; return nullptr; }
        return r;
    }
    u64 size() const override { return kWiiDualLayerSize; }
    bool read(u64 offset, std::span<u8> out) override {
        if (!inside(offset, out.size(), size())) return false;
        std::size_t done = 0;
        while (done < out.size()) {
            const u64 at = offset + done, index = at / sector_, in = at % sector_;
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(out.size() - done, sector_ - in));
            const u16 b = table_[static_cast<std::size_t>(index)];
            if (b == 0U) std::memset(out.data() + done, 0, n); // not stored (unused by the disc)
            else if (!file_->read(u64{b} * sector_ + in, out.subspan(done, n))) {
                error_ = "WBFS: block " + std::to_string(index) + " cannot be read from the file";
                return false;
            }
            done += n;
        }
        return true;
    }

private:
    std::string failure_reason() const override { return error_; }
    WbfsReader(std::unique_ptr<DiscReader> f, u64 sector) : file_(std::move(f)), sector_(sector) {}
    std::unique_ptr<DiscReader> file_;
    u64 sector_;
    std::vector<u16> table_;
    std::string error_;
};

// --- GCZ ----------------------------------------------------------------------
constexpr u32 kGczMagic = 0xB10BB10BU;
constexpr u64 kGczUncompressed = 1ULL << 63;

#if defined(ORCA_HAVE_ZLIB)
class GczReader final : public DiscReader {
public:
    static std::unique_ptr<GczReader> open(std::unique_ptr<DiscReader> file, std::string& error) {
        u8 h[32];
        if (!file->read(0, h)) { error = "GCZ: header truncated"; return nullptr; }
        std::unique_ptr<GczReader> r(new GczReader(std::move(file)));
        r->compressed_size_ = le64(&h[8]);
        r->disc_size_ = le64(&h[16]);
        r->block_size_ = le32(&h[24]);
        const u32 blocks = le32(&h[28]);
        if (blocks == 0U || r->block_size_ == 0U || r->block_size_ > (16U << 20)) { error = "GCZ: invalid block size or count"; return nullptr; }
        if (u64{blocks} * r->block_size_ < r->disc_size_ || r->disc_size_ == 0U) { error = "GCZ: the blocks do not cover the disc size"; return nullptr; }
        const u64 table = 32U + u64{blocks} * 12U;
        if (!inside(table, r->compressed_size_, r->file_->size())) {
            error = "GCZ: data (" + hex(table + r->compressed_size_) + " bytes) larger than the file (" + hex(r->file_->size()) + "): truncated";
            return nullptr;
        }
        std::vector<u8> t(static_cast<std::size_t>(u64{blocks} * 12U));
        if (!r->file_->read(32, t)) { error = "GCZ: block table truncated"; return nullptr; }
        r->pointers_.resize(blocks);
        r->hashes_.resize(blocks);
        for (u32 i = 0; i < blocks; ++i) {
            r->pointers_[i] = le64(&t[8U * i]);
            r->hashes_[i] = le32(&t[8U * blocks + 4U * i]);
        }
        r->data_offset_ = table;
        for (u32 i = 0; i < blocks; ++i) {
            const u64 start = r->pointers_[i] & ~kGczUncompressed;
            const u64 end = i + 1U < blocks ? r->pointers_[i + 1U] & ~kGczUncompressed : r->compressed_size_;
            if (end < start || end > r->compressed_size_) { error = "GCZ: block " + std::to_string(i) + " has an invalid position"; return nullptr; }
            if ((r->pointers_[i] & kGczUncompressed) != 0U && end - start != r->block_size_) { error = "GCZ: stored block " + std::to_string(i) + " has the wrong size"; return nullptr; }
            if (end - start > u64{r->block_size_} + 64U) { error = "GCZ: compressed block " + std::to_string(i) + " is larger than a block"; return nullptr; }
        }
        r->buffer_.resize(r->block_size_ + 64U);
        r->block_.resize(r->block_size_);
        return r;
    }
    u64 size() const override { return disc_size_; }
    bool read(u64 offset, std::span<u8> out) override {
        if (!inside(offset, out.size(), size())) return false;
        std::size_t done = 0;
        while (done < out.size()) {
            const u64 at = offset + done, index = at / block_size_, in = at % block_size_;
            const std::size_t n = static_cast<std::size_t>(std::min<u64>(out.size() - done, block_size_ - in));
            if (!load(index)) return false;
            std::memcpy(out.data() + done, block_.data() + in, n);
            done += n;
        }
        return true;
    }
    std::string failure_reason() const override { return error_; }

private:
    explicit GczReader(std::unique_ptr<DiscReader> f) : file_(std::move(f)) {}
    bool load(u64 index) {
        if (index == cached_) return true;
        cached_ = ~u64{0};
        const std::size_t i = static_cast<std::size_t>(index);
        const u64 start = pointers_[i] & ~kGczUncompressed;
        const u64 end = i + 1U < pointers_.size() ? pointers_[i + 1U] & ~kGczUncompressed : compressed_size_;
        const std::size_t length = static_cast<std::size_t>(end - start);
        if (!file_->read(data_offset_ + start, std::span<u8>(buffer_.data(), length))) { error_ = "GCZ: block " + std::to_string(i) + " is missing (truncated image)"; return false; }
        const uLong adler = adler32(adler32(0L, Z_NULL, 0), buffer_.data(), static_cast<uInt>(length));
        if (static_cast<u32>(adler) != hashes_[i]) { error_ = "GCZ: block " + std::to_string(i) + " is corrupt (Adler-32 mismatch)"; return false; }
        if ((pointers_[i] & kGczUncompressed) != 0U) {
            std::memcpy(block_.data(), buffer_.data(), block_size_);
        } else {
            z_stream z{};
            z.next_in = buffer_.data();
            z.avail_in = static_cast<uInt>(length);
            z.next_out = block_.data();
            z.avail_out = block_size_;
            if (inflateInit(&z) != Z_OK) { error_ = "GCZ: zlib initialisation failed"; return false; }
            const int status = inflate(&z, Z_FINISH);
            const u32 produced = block_size_ - z.avail_out;
            inflateEnd(&z);
            if (status != Z_STREAM_END || produced != block_size_) { error_ = "GCZ: block " + std::to_string(i) + " does not decompress to a whole block"; return false; }
        }
        cached_ = index;
        return true;
    }

    std::unique_ptr<DiscReader> file_;
    u64 compressed_size_{0}, disc_size_{0}, data_offset_{0}, cached_{~u64{0}};
    u32 block_size_{0};
    std::vector<u64> pointers_;
    std::vector<u32> hashes_;
    std::vector<u8> buffer_, block_;
    std::string error_;
};
#endif

bool has_disc_magic(DiscReader& d) {
    u8 h[0x20];
    if (!d.read(0, h)) return false;
    return be32(&h[0x18]) == kWiiMagic || be32(&h[0x1C]) == kGameCubeMagic;
}
} // namespace

const char* image_format_name(ImageFormat f) noexcept {
    switch (f) {
    case ImageFormat::Raw: return "ISO/GCM";
    case ImageFormat::NKit: return "NKit (raw-compatible ISO)";
    case ImageFormat::Ciso: return "CISO";
    case ImageFormat::Wbfs: return "WBFS";
    case ImageFormat::Gcz: return "GCZ";
    }
    return "?";
}

bool gcz_supported() noexcept {
#if defined(ORCA_HAVE_ZLIB)
    return true;
#else
    return false;
#endif
}

std::unique_ptr<DiscReader> open_image(std::unique_ptr<DiscReader> file, ImageFormat* format, std::string& error) {
    if (!file) { error = "no image"; return nullptr; }
    u8 m[4] = {};
    if (file->size() < 4U || !file->read(0, m)) { error = "the file is too small to be a disc image"; return nullptr; }
    const u32 le = le32(m);
    std::unique_ptr<DiscReader> disc;
    ImageFormat f = ImageFormat::Raw;
    if (std::memcmp(m, "CISO", 4) == 0) {
        f = ImageFormat::Ciso;
        disc = CisoReader::open(std::move(file), error);
    } else if (std::memcmp(m, "WBFS", 4) == 0) {
        f = ImageFormat::Wbfs;
        disc = WbfsReader::open(std::move(file), error);
    } else if (le == kGczMagic) {
        f = ImageFormat::Gcz;
#if defined(ORCA_HAVE_ZLIB)
        disc = GczReader::open(std::move(file), error);
#else
        error = "GCZ images need zlib, which this build does not have: convert the image to ISO";
        return nullptr;
#endif
    } else if (std::memcmp(m, "WIA\x01", 4) == 0 || std::memcmp(m, "RVZ\x01", 4) == 0) {
        error = std::string(std::memcmp(m, "RVZ", 3) == 0 ? "RVZ" : "WIA") +
                " images are not supported (they need bzip2/LZMA/Zstandard decompression): convert the image to ISO with Dolphin "
                "(right click > Convert File) or DolphinTool";
        return nullptr;
    } else if (le == 0xA2380FAEU) {
        error = "TGC images (GameCube demo disc files) are not supported";
        return nullptr;
    } else if (std::memcmp(m, "EGGS", 4) == 0) {
        error = "NFS images (Wii U vWii) are not supported";
        return nullptr;
    } else {
        u8 nkit[4] = {};
        if (file->size() >= 0x204U && file->read(0x200, nkit) && std::memcmp(nkit, "NKIT", 4) == 0) f = ImageFormat::NKit;
        disc = std::move(file);
    }
    if (!disc) {
        if (error.empty()) error = std::string(image_format_name(f)) + ": cannot open the image";
        return nullptr;
    }
    if (disc->size() < 0x440U || !has_disc_magic(*disc)) {
        error = std::string(image_format_name(f)) + " image: no GameCube magic at 0x1C or Wii magic at 0x18; not a GameCube or Wii disc";
        return nullptr;
    }
    if (format != nullptr) *format = f;
    return disc;
}

std::unique_ptr<DiscReader> open_image_file(const std::string& path, ImageFormat* format, std::string& error) {
    auto f = FileDisc::open(path, error);
    if (!f) return nullptr;
    auto d = open_image(std::move(f), format, error);
    if (!d) error = path + ": " + error;
    return d;
}

#if defined(__unix__) || defined(__APPLE__)
std::unique_ptr<DiscReader> open_image_fd(int fd, ImageFormat* format, std::string& error) {
    auto f = FdDisc::open(fd, error);
    if (!f) return nullptr;
    return open_image(std::move(f), format, error);
}
#endif

} // namespace orca::disc
