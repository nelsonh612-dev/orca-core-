// Wii ticket title-key derivation. Common keys are always supplied by the host.
#pragma once
#include "orca/crypto/aes128.hpp"
#include "orca/disc/wii_partition.hpp"
#include <string>

namespace orca::disc {
class WiiCommonKeyProvider {
public:
    virtual ~WiiCommonKeyProvider() = default;
    // Return the user-provided key for the on-disc common-key slot.
    virtual bool get_common_key(u8 index, crypto::Aes128Key& key) = 0;
};

// Reads the ticket at the partition start and decrypts its encrypted title key.
// Ticket IV is title_id[8] followed by eight zero bytes. No default key exists.
bool derive_wii_title_key(DiscReader& raw, const WiiPartitionInfo& partition,
                          WiiCommonKeyProvider& provider, crypto::Aes128Key& title_key,
                          std::string& error);
} // namespace orca::disc
