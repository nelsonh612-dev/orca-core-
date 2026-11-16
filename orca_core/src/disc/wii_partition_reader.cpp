#include "orca/disc/wii_partition_reader.hpp"
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
    crypto::Aes128Iv iv{};
    std::copy_n(raw.begin()+0x3D0,iv.size(),iv.begin());
    std::span<u8> encrypted(raw.data()+kWiiClusterHashSize,kWiiClusterDataSize);
    if(!crypto::aes128_cbc_decrypt(title_key_,iv,encrypted)) {
        error_="Wii partition: AES-CBC rejected a non-block-aligned cluster";
        return false;
    }
    std::copy(encrypted.begin(),encrypted.end(),out.begin());
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
