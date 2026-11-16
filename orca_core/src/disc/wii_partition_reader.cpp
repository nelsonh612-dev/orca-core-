#include "orca/disc/wii_partition_reader.hpp"
#include "orca/crypto/sha1.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <vector>

namespace orca::disc {
namespace {
constexpr u64 kWiiH3TableSize=0x18000ULL;
constexpr std::size_t kH0Size=0x26CU;
constexpr std::size_t kH1Offset=0x280U;
constexpr std::size_t kH1Size=0xA0U;
constexpr std::size_t kH2Offset=0x340U;
constexpr std::size_t kH2Size=0xA0U;
}
WiiPartitionReader::WiiPartitionReader(DiscReader& raw,const WiiPartitionInfo& p,
                                       const crypto::Aes128Key& key)
    : raw_(raw),data_offset_(p.data_offset),h3_offset_(p.h3_offset),title_key_(key) {
    if(p.data_size==0 || p.data_size%kWiiClusterRawSize!=0) {
        error_="Wii partition: encrypted data size is zero or not a whole 0x8000-byte cluster";
        return;
    }
    cluster_count_=p.data_size/kWiiClusterRawSize;
    if(cluster_count_>std::numeric_limits<u64>::max()/kWiiClusterDataSize) {
        error_="Wii partition: logical data size overflows";
        return;
    }
    logical_size_=cluster_count_*kWiiClusterDataSize;
    if(data_offset_>raw_.size() || p.data_size>raw_.size()-data_offset_) {
        error_="Wii partition: encrypted data range exceeds the raw disc";
        logical_size_=0;
        return;
    }
    if(h3_offset_<=p.offset || h3_offset_>raw_.size() ||
       kWiiH3TableSize>raw_.size()-h3_offset_) {
        error_="Wii partition: H3 hash table is missing or outside the raw disc";
        logical_size_=0;
        return;
    }
    valid_=true;
}
WiiPartitionReader::~WiiPartitionReader() {
    volatile u8* p=title_key_.data();
    for(std::size_t i=0;i<title_key_.size();++i) p[i]=0;
}
bool WiiPartitionReader::read_hash_header(u64 index,std::array<u8,kWiiClusterHashSize>& out) {
    if(index>=cluster_count_) return false;
    std::array<u8,kWiiClusterRawSize> raw{};
    const u64 offset=data_offset_+index*kWiiClusterRawSize;
    if(!raw_.read(offset,raw)) {
        error_="Wii partition: failed to read encrypted hash header for cluster "+std::to_string(index);
        return false;
    }
    std::copy_n(raw.begin(),out.size(),out.begin());
    crypto::Aes128Iv iv{};
    if(!crypto::aes128_cbc_decrypt(title_key_,iv,out)) {
        error_="Wii partition: failed to decrypt hash header";
        return false;
    }
    return true;
}
bool WiiPartitionReader::verify_hash_group(u64 index) {
    const u64 group=index/64U;
    if(verified_group_==group) return true;
    const u64 first=group*64U;
    const u64 end=std::min(first+64U,cluster_count_);
    std::array<std::array<u8,kWiiClusterHashSize>,64> headers{};
    for(u64 i=first;i<end;++i) {
        if(!read_hash_header(i,headers[static_cast<std::size_t>(i-first)])) return false;
    }
    const auto& reference=headers[0];
    // H1 authenticates each cluster's H0 table within its group of eight.
    for(u64 subgroup=0;subgroup<8U;++subgroup) {
        const u64 sub_first=first+subgroup*8U;
        if(sub_first>=end) break;
        const auto& h1=headers[static_cast<std::size_t>(sub_first-first)];
        for(u64 i=sub_first;i<std::min(sub_first+8U,end);++i) {
            const auto& current=headers[static_cast<std::size_t>(i-first)];
            if(!std::equal(h1.begin()+static_cast<std::ptrdiff_t>(kH1Offset),
                           h1.begin()+static_cast<std::ptrdiff_t>(kH1Offset+kH1Size),
                           current.begin()+static_cast<std::ptrdiff_t>(kH1Offset))) {
                error_="Wii partition: inconsistent H1 tables inside subgroup "+std::to_string(subgroup);
                return false;
            }
            const auto digest=crypto::sha1(std::span<const u8>(current.data(),kH0Size));
            const std::size_t slot=static_cast<std::size_t>(i-sub_first)*20U;
            if(!std::equal(digest.begin(),digest.end(),
                           h1.begin()+static_cast<std::ptrdiff_t>(kH1Offset+slot))) {
                error_="Wii partition: H1 SHA-1 mismatch in cluster "+std::to_string(i);
                return false;
            }
        }
    }
    // Every cluster in a 64-cluster group carries the same H2 table.
    for(u64 i=first;i<end;++i) {
        const auto& current=headers[static_cast<std::size_t>(i-first)];
        if(!std::equal(reference.begin()+static_cast<std::ptrdiff_t>(kH2Offset),
                       reference.begin()+static_cast<std::ptrdiff_t>(kH2Offset+kH2Size),
                       current.begin()+static_cast<std::ptrdiff_t>(kH2Offset))) {
            error_="Wii partition: inconsistent H2 tables inside 64-cluster group "+std::to_string(group);
            return false;
        }
    }
    // H2 authenticates each subgroup's H1 table.
    for(u64 subgroup=0;subgroup<8U;++subgroup) {
        const u64 sub_first=first+subgroup*8U;
        if(sub_first>=end) break;
        const auto& h1=headers[static_cast<std::size_t>(sub_first-first)];
        const auto digest=crypto::sha1(std::span<const u8>(
            h1.data()+kH1Offset,kH1Size));
        const std::size_t slot=static_cast<std::size_t>(subgroup)*20U;
        if(!std::equal(digest.begin(),digest.end(),
                       reference.begin()+static_cast<std::ptrdiff_t>(kH2Offset+slot))) {
            error_="Wii partition: H2 SHA-1 mismatch in subgroup "+std::to_string(subgroup);
            return false;
        }
    }
    // H3 authenticates the H2 table for this 64-cluster group.
    std::array<u8,20> stored_h3{};
    const u64 h3_entry=h3_offset_+group*20U;
    if(group*20U+stored_h3.size()>kWiiH3TableSize ||
       !raw_.read(h3_entry,stored_h3)) {
        error_="Wii partition: H3 entry is outside the H3 table";
        return false;
    }
    const auto h3_digest=crypto::sha1(std::span<const u8>(
        reference.data()+kH2Offset,kH2Size));
    if(h3_digest!=stored_h3) {
        error_="Wii partition: H3 SHA-1 mismatch in 64-cluster group "+std::to_string(group);
        return false;
    }
    verified_group_=group;
    return true;
}
bool WiiPartitionReader::read_cluster(u64 index,std::array<u8,kWiiClusterDataSize>& out) {
    if(!valid_ || index>=cluster_count_) return false;
    if(!verify_hash_group(index)) return false;
    std::array<u8,kWiiClusterRawSize> raw{};
    const u64 offset=data_offset_+index*kWiiClusterRawSize;
    if(!raw_.read(offset,raw)) { error_="Wii partition: failed to read encrypted cluster"; return false; }

    // The user-data IV is in the still-encrypted hash header; save it before
    // decrypting the 0x400-byte hash block with an all-zero IV.
    crypto::Aes128Iv data_iv{};
    std::copy_n(raw.begin()+0x3D0,data_iv.size(),data_iv.begin());
    crypto::Aes128Iv hash_iv{};
    std::span<u8> encrypted_hash(raw.data(),kWiiClusterHashSize);
    if(!crypto::aes128_cbc_decrypt(title_key_,hash_iv,encrypted_hash)) {
        error_="Wii partition: failed to decrypt cluster hash header";
        return false;
    }
    std::span<u8> encrypted_data(raw.data()+kWiiClusterHashSize,kWiiClusterDataSize);
    if(!crypto::aes128_cbc_decrypt(title_key_,data_iv,encrypted_data)) {
        error_="Wii partition: failed to decrypt cluster user data";
        return false;
    }
    // H0: 31 SHA-1 digests, one for each 0x400-byte user-data sector.
    for(std::size_t sector=0;sector<31U;++sector) {
        const auto bytes=std::span<const u8>(encrypted_data.data()+sector*0x400U,0x400U);
        const auto digest=crypto::sha1(bytes);
        const std::size_t slot=sector*20U;
        if(!std::equal(digest.begin(),digest.end(),
                       encrypted_hash.begin()+static_cast<std::ptrdiff_t>(slot))) {
            error_="Wii partition: H0 SHA-1 mismatch in cluster "+std::to_string(index)+
                   ", data sector "+std::to_string(sector);
            return false;
        }
    }
    std::copy(encrypted_data.begin(),encrypted_data.end(),out.begin());
    return true;
}
bool WiiPartitionReader::read(u64 offset,std::span<u8> out) {
    if(!valid_ || offset>logical_size_ || out.size()>logical_size_-offset) return false;
    std::array<u8,kWiiClusterDataSize> cluster{};
    std::size_t written=0;
    while(written<out.size()) {
        const u64 position=offset+written;
        const u64 index=position/kWiiClusterDataSize;
        const std::size_t in_cluster=static_cast<std::size_t>(position%kWiiClusterDataSize);
        if(!read_cluster(index,cluster)) return false;
        const std::size_t amount=std::min(out.size()-written,cluster.size()-in_cluster);
        std::copy_n(cluster.begin()+static_cast<std::ptrdiff_t>(in_cluster),amount,
                    out.begin()+static_cast<std::ptrdiff_t>(written));
        written+=amount;
    }
    return true;
}
} // namespace orca::disc
