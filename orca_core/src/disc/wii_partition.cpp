#include "orca/disc/wii_partition.hpp"

#include <array>
#include <cstring>
#include <limits>

namespace orca::disc {
namespace {
u32 be32(const u8* p) {
    return (u32{p[0]} << 24U) | (u32{p[1]} << 16U) | (u32{p[2]} << 8U) | u32{p[3]};
}
bool scaled_word_offset(u32 words, u64 base, u64 limit, u64& result) {
    const u64 relative = u64{words} * 4ULL;
    if (relative > limit || base > std::numeric_limits<u64>::max() - relative) return false;
    result = base + relative;
    return result <= limit;
}
} // namespace

std::optional<WiiPartitionTable> parse_wii_partition_table(DiscReader& disc, std::string& error) {
    std::array<u8, 0x20> disc_header{};
    if (!disc.read(0, disc_header)) {
        error = "Wii disc: image is smaller than the disc header";
        return std::nullopt;
    }
    if (be32(&disc_header[0x18]) != kWiiMagic) {
        error = "Wii disc: Wii magic 0x5D1C9EA3 not found";
        return std::nullopt;
    }

    std::array<u8, kWiiPartitionGroups * 8U> groups{};
    if (!disc.read(kWiiPartitionTableOffset, groups)) {
        error = "Wii disc: partition group table is missing or truncated";
        return std::nullopt;
    }

    WiiPartitionTable table;
    for (u32 group = 0; group < kWiiPartitionGroups; ++group) {
        const std::size_t descriptor = static_cast<std::size_t>(group) * 8U;
        const u32 count = be32(&groups[descriptor]);
        const u32 table_words = be32(&groups[descriptor + 4U]);
        if (count == 0U) continue;
        if (count > 4096U) {
            error = "Wii disc: unreasonable partition count in group " + std::to_string(group);
            return std::nullopt;
        }
        const u64 entries_offset = u64{table_words} * 4ULL;
        const u64 entries_bytes = u64{count} * kWiiPartitionEntrySize;
        if (entries_offset > disc.size() || entries_bytes > disc.size() - entries_offset) {
            error = "Wii disc: partition entry table extends past the image";
            return std::nullopt;
        }
        std::vector<u8> entries(static_cast<std::size_t>(entries_bytes));
        if (!disc.read(entries_offset, entries)) {
            error = "Wii disc: failed to read partition entries";
            return std::nullopt;
        }
        for (u32 index = 0; index < count; ++index) {
            const std::size_t entry = static_cast<std::size_t>(index) * kWiiPartitionEntrySize;
            const u64 partition_offset = u64{be32(&entries[entry])} * 4ULL;
            const u32 type = be32(&entries[entry + 4U]);
            if (partition_offset > disc.size() || kWiiPartitionHeaderSize > disc.size() - partition_offset) {
                error = "Wii disc: partition header extends past the image";
                return std::nullopt;
            }
            std::array<u8, kWiiPartitionHeaderSize> metadata{};
            if (!disc.read(partition_offset, metadata)) {
                error = "Wii disc: failed to read partition metadata";
                return std::nullopt;
            }

            WiiPartitionInfo info;
            info.offset = partition_offset;
            info.type = type;
            const u32 tmd_size = be32(&metadata[0x2A4]);
            const u32 tmd_words = be32(&metadata[0x2A8]);
            const u32 cert_size = be32(&metadata[0x2AC]);
            const u32 cert_words = be32(&metadata[0x2B0]);
            const u32 h3_words = be32(&metadata[0x2B4]);
            const u32 data_words = be32(&metadata[0x2B8]);
            const u32 data_size_words = be32(&metadata[0x2BC]);
            info.tmd_size = tmd_size;
            info.cert_size = cert_size;
            if (!scaled_word_offset(tmd_words, partition_offset, disc.size(), info.tmd_offset) ||
                !scaled_word_offset(cert_words, partition_offset, disc.size(), info.cert_offset) ||
                !scaled_word_offset(h3_words, partition_offset, disc.size(), info.h3_offset) ||
                !scaled_word_offset(data_words, partition_offset, disc.size(), info.data_offset)) {
                error = "Wii disc: partition metadata contains an out-of-range offset";
                return std::nullopt;
            }
            info.data_size = u64{data_size_words} * 4ULL;
            if (info.tmd_size > disc.size() - info.tmd_offset ||
                info.cert_size > disc.size() - info.cert_offset ||
                info.data_size > disc.size() - info.data_offset) {
                error = "Wii disc: TMD, certificate, or data range extends past the image";
                return std::nullopt;
            }
            // Ticket fields are at fixed offsets within the 0x2A4-byte ticket.
            for (std::size_t b = 0; b < info.title_id.size(); ++b)
                info.title_id[b] = metadata[0x1DCU + b];
            info.common_key_index = metadata[0x1F1U];
            table.partitions.push_back(info);
        }
    }
    if (table.partitions.empty()) {
        error = "Wii disc: no partitions found in the partition group table";
        return std::nullopt;
    }
    return table;
}


std::optional<DiscHeader> parse_wii_partition_header(DiscReader& partition, std::string& error) {
    std::array<u8,0x440> h{};
    if(!partition.read(0,h)) {
        error="Wii partition: decrypted header is truncated";
        return std::nullopt;
    }
    if(be32(&h[0x18])!=kWiiMagic) {
        error="Wii partition: decrypted header does not contain Wii magic";
        return std::nullopt;
    }
    DiscHeader d;
    d.game_id.assign(reinterpret_cast<const char*>(h.data()),6);
    d.disc_number=h[6];
    d.version=h[7];
    d.wii=true;
    const char* name=reinterpret_cast<const char*>(&h[0x20]);
    d.name.assign(name,strnlen(name,0x3E0));
    const u32 dol_words=be32(&h[0x420]);
    const u32 fst_words=be32(&h[0x424]);
    if(dol_words>0x3FFFFFFFU || fst_words>0x3FFFFFFFU) {
        error="Wii partition: DOL/FST word offset overflows a 32-bit byte offset";
        return std::nullopt;
    }
    d.dol_offset=dol_words*4U;
    d.fst_offset=fst_words*4U;
    d.fst_size=be32(&h[0x428]);
    d.fst_max_size=be32(&h[0x42C]);
    if(d.dol_offset==0U || d.dol_offset>=partition.size()) {
        error="Wii partition: main DOL offset is outside decrypted partition";
        return std::nullopt;
    }
    if(d.fst_size!=0U && (d.fst_offset>partition.size() || d.fst_size>partition.size()-d.fst_offset)) {
        error="Wii partition: FST range is outside decrypted partition";
        return std::nullopt;
    }
    return d;
}

} // namespace orca::disc
