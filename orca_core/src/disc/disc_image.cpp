#include "orca/disc/disc_image.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace orca::disc {
namespace {
u32 be32(const u8* p) { return (u32{p[0]} << 24U) | (u32{p[1]} << 16U) | (u32{p[2]} << 8U) | p[3]; }
} // namespace

bool MemoryDisc::read(u64 offset, std::span<u8> out) {
    if (offset > data_.size() || out.size() > data_.size() - offset) return false;
    // A zero-length read is valid at any position up to EOF. Avoid pointer
    // arithmetic on data() when the backing vector is empty and avoid passing
    // a potentially null span::data() to memcpy, even with a zero byte count.
    if (out.empty()) return true;
    std::memcpy(out.data(), data_.data() + static_cast<std::size_t>(offset), out.size());
    return true;
}

std::unique_ptr<FileDisc> FileDisc::open(const std::string& path, std::string& error) {
    auto d = std::unique_ptr<FileDisc>(new FileDisc());
    d->file_.open(path, std::ios::binary);
    if (!d->file_) { error = "cannot open disc image " + path; return nullptr; }
    d->file_.seekg(0, std::ios::end);
    const std::streampos end = d->file_.tellg();
    if (end < std::streampos{0}) {
        error = "cannot determine disc image size " + path;
        return nullptr;
    }
    d->size_ = static_cast<u64>(end);
    d->file_.seekg(0, std::ios::beg);
    if (!d->file_) {
        error = "cannot seek to start of disc image " + path;
        return nullptr;
    }
    return d;
}

bool FileDisc::read(u64 offset, std::span<u8> out) {
    if (offset > size_ || out.size() > size_ - offset) return false;
    if (out.empty()) return true;
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    return static_cast<std::size_t>(file_.gcount()) == out.size();
}

std::optional<std::vector<WiiPartition>> parse_wii_partition_table(DiscReader& disc, std::string& error) {
    // The four group descriptors occupy 0x20 bytes at 0x40000. Each
    // descriptor contains a partition count and a byte offset divided by 4.
    constexpr u64 kGroupTableBytes = kWiiPartitionGroupCount * 8U;
    if (disc.size() < kWiiPartitionInfoOffset ||
        kGroupTableBytes > disc.size() - kWiiPartitionInfoOffset) {
        error = "Wii disc: partition information table is truncated";
        return std::nullopt;
    }
    std::array<u8, kGroupTableBytes> groups{};
    if (!disc.read(kWiiPartitionInfoOffset, groups)) {
        error = "Wii disc: cannot read partition group table";
        return std::nullopt;
    }

    constexpr u32 kMaxPartitions = 4096U;
    std::vector<WiiPartition> partitions;
    for (u32 group = 0; group < kWiiPartitionGroupCount; ++group) {
        const u32 count = be32(&groups[group * 8U]);
        const u32 offset_words = be32(&groups[group * 8U + 4U]);
        if (count == 0U) continue;
        if (count > kMaxPartitions || partitions.size() > kMaxPartitions - count) {
            error = "Wii disc: unreasonable partition count";
            return std::nullopt;
        }
        const u64 table_offset = u64{offset_words} * 4U;
        const u64 table_bytes = u64{count} * 8U;
        if (table_offset > disc.size() || table_bytes > disc.size() - table_offset) {
            error = "Wii disc: partition entry table extends past the image";
            return std::nullopt;
        }

        std::vector<u8> entries(static_cast<std::size_t>(table_bytes));
        if (!disc.read(table_offset, entries)) {
            error = "Wii disc: cannot read partition entries";
            return std::nullopt;
        }
        for (u32 i = 0; i < count; ++i) {
            const u64 partition_offset = u64{be32(&entries[i * 8U])} * 4U;
            const u32 type = be32(&entries[i * 8U + 4U]);
            if (partition_offset >= disc.size()) {
                error = "Wii disc: partition offset outside the image";
                return std::nullopt;
            }
            partitions.push_back({partition_offset, type});
        }
    }
    if (partitions.empty()) {
        error = "Wii disc: partition table contains no partitions";
        return std::nullopt;
    }
    return partitions;
}


std::optional<WiiPartitionHeader> parse_wii_partition_header(
    DiscReader& disc, const WiiPartition& partition, std::string& error) {
    constexpr u64 kTicketSize = 0x2A4U;
    constexpr u64 kMetadataSize = 0x1CU;
    constexpr u64 kH3Size = 0x18000U;
    if (partition.offset > disc.size() ||
        kTicketSize + kMetadataSize > disc.size() - partition.offset) {
        error = "Wii partition: ticket or metadata block is truncated";
        return std::nullopt;
    }

    std::array<u8, kMetadataSize> raw{};
    if (!disc.read(partition.offset + kTicketSize, raw)) {
        error = "Wii partition: cannot read metadata block";
        return std::nullopt;
    }

    WiiPartitionHeader result;
    result.tmd_size = be32(&raw[0x00]);
    result.tmd_offset = u64{be32(&raw[0x04])} * 4U;
    result.cert_size = be32(&raw[0x08]);
    result.cert_offset = u64{be32(&raw[0x0C])} * 4U;
    result.h3_offset = u64{be32(&raw[0x10])} * 4U;
    result.data_offset = u64{be32(&raw[0x14])} * 4U;
    result.data_size = u64{be32(&raw[0x18])} * 4U;

    const u64 partition_size = disc.size() - partition.offset;
    const auto range_in_partition = [partition_size](u64 offset, u64 length) {
        return offset <= partition_size && length <= partition_size - offset;
    };
    if (result.tmd_size == 0U || !range_in_partition(result.tmd_offset, result.tmd_size)) {
        error = "Wii partition: TMD range is empty or outside the image";
        return std::nullopt;
    }
    if (result.cert_size != 0U && !range_in_partition(result.cert_offset, result.cert_size)) {
        error = "Wii partition: certificate chain extends outside the image";
        return std::nullopt;
    }
    if (!range_in_partition(result.h3_offset, kH3Size)) {
        error = "Wii partition: H3 table extends outside the image";
        return std::nullopt;
    }
    if (result.data_size == 0U || !range_in_partition(result.data_offset, result.data_size)) {
        error = "Wii partition: encrypted data range is empty or outside the image";
        return std::nullopt;
    }
    if (result.data_offset < kTicketSize + kMetadataSize ||
        result.tmd_offset < kTicketSize + kMetadataSize ||
        (result.cert_size != 0U && result.cert_offset < kTicketSize + kMetadataSize)) {
        error = "Wii partition: metadata overlaps the ticket/header";
        return std::nullopt;
    }
    return result;
}

std::optional<DiscHeader> parse_header(DiscReader& disc, std::string& error) {
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
    if (d.wii) { error = "disc: Wii discs (encrypted partitions) are not supported yet"; return std::nullopt; }
    if (!d.gamecube) { error = "disc: GameCube magic 0xC2339F3D not found at 0x1C"; return std::nullopt; }
    if (d.dol_offset == 0 || d.dol_offset >= disc.size()) { error = "disc: main DOL offset outside the image"; return std::nullopt; }
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
