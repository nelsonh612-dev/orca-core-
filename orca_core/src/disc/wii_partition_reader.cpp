#include "orca/disc/wii_partition_reader.hpp"
#include "orca/crypto/sha1.hpp"
#include <algorithm>
#include <array>
#include <limits>

namespace orca::disc {
WiiPartitionReader::WiiPartitionReader(DiscReader& raw,const WiiPartitionInfo& p,
                                       const crypto::Aes128Key& key)
    : raw_(raw),data_offset_(p.data_offset),title_key_(key) {
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
    valid_=true;
}
WiiPartitionReader::~WiiPartitionReader() {
    volatile u8* p=title_key_.data();
    for(std::size_t i=0;i<title_key_.size();++i) p[i]=0;
}
bool WiiPartitionReader::read_cluster(u64 index,std::array<u8,kWiiClusterDataSize>& out) {
    if(!valid_ || index>=cluster_count_) return false;
    std::array<u8,kWiiClusterRawSize> raw{};
    const u64 offset=data_offset_+index*kWiiClusterRawSize;
    if(!raw_.read(offset,raw)) { error_="Wii partition: failed to read encrypted cluster"; return false; }

    // The IV for the user-data region is stored in the still-encrypted hash
    // header. Save it before decrypting the 0x400-byte hash area with IV=0.
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
    // Reject corrupt/incorrect-key data before exposing it to the FST loader.
    for(std::size_t sector=0;sector<31U;++sector) {
        const auto sector_bytes=std::span<const u8>(encrypted_data.data()+sector*0x400U,0x400U);
        const auto digest=crypto::sha1(sector_bytes);
        const std::size_t hash_offset=sector*20U;
        if(!std::equal(digest.begin(),digest.end(),encrypted_hash.begin()+static_cast<std::ptrdiff_t>(hash_offset))) {
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
