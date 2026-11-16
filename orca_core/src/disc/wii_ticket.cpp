#include "orca/disc/wii_ticket.hpp"
#include <array>
#include <algorithm>

namespace orca::disc {
bool derive_wii_title_key(DiscReader& raw,const WiiPartitionInfo& partition,
                          WiiCommonKeyProvider& provider,crypto::Aes128Key& title_key,
                          std::string& error) {
    constexpr std::size_t kTicketSize=0x2A4;
    constexpr std::size_t kEncryptedTitleKeyOffset=0x1BF;
    constexpr std::size_t kTitleIdOffset=0x1DC;
    constexpr std::size_t kCommonKeyIndexOffset=0x1F1;
    std::array<u8,kTicketSize> ticket{};
    if(partition.offset>raw.size() || ticket.size()>raw.size()-partition.offset ||
       !raw.read(partition.offset,ticket)) {
        error="Wii ticket: ticket is truncated or outside the disc";
        return false;
    }
    const u8 index=ticket[kCommonKeyIndexOffset];
    crypto::Aes128Key common_key{};
    if(!provider.get_common_key(index,common_key)) {
        error="Wii ticket: no user-provided common key for slot "+std::to_string(index);
        return false;
    }
    crypto::Aes128Iv iv{};
    std::copy_n(ticket.begin()+static_cast<std::ptrdiff_t>(kTitleIdOffset),8,iv.begin());
    std::array<u8,16> encrypted{};
    std::copy_n(ticket.begin()+static_cast<std::ptrdiff_t>(kEncryptedTitleKeyOffset),16,encrypted.begin());
    if(!crypto::aes128_cbc_decrypt(common_key,iv,encrypted)) {
        error="Wii ticket: encrypted title key has invalid AES block length";
        return false;
    }
    title_key=encrypted;
    volatile u8* wipe=common_key.data();
    for(std::size_t i=0;i<common_key.size();++i) wipe[i]=0;
    return true;
}
} // namespace orca::disc
