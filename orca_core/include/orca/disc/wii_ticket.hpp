// Wii ticket title-key derivation. Common keys are always supplied by the host.
#pragma once
#include "orca/crypto/aes128.hpp"
#include "orca/disc/wii_partition.hpp"
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace orca::disc {
class WiiCommonKeyProvider {
public:
    virtual ~WiiCommonKeyProvider() = default;
    // Return the user-provided key for the on-disc common-key slot.
    virtual bool get_common_key(u8 index, crypto::Aes128Key& key) = 0;
};

// Host-owned common keys loaded from an explicit user-provided text file.
// Format: one "index = 32-hex-digit-key" entry per line; blank lines and
// comments beginning with '#' are ignored. No Nintendo key is bundled.
class WiiCommonKeyFileProvider final : public WiiCommonKeyProvider {
public:
    WiiCommonKeyFileProvider() = default;
    ~WiiCommonKeyFileProvider() override;
    WiiCommonKeyFileProvider(const WiiCommonKeyFileProvider&) = delete;
    WiiCommonKeyFileProvider& operator=(const WiiCommonKeyFileProvider&) = delete;

    bool load_text(std::string_view text, std::string& error);
    void clear() noexcept;
    bool get_common_key(u8 index, crypto::Aes128Key& key) override;

private:
    std::array<std::optional<crypto::Aes128Key>, 256> keys_{};
};

// Reads the ticket at the partition start and decrypts its encrypted title key.
// Ticket IV is title_id[8] followed by eight zero bytes. No default key exists.
bool derive_wii_title_key(DiscReader& raw, const WiiPartitionInfo& partition,
                          WiiCommonKeyProvider& provider, crypto::Aes128Key& title_key,
                          std::string& error);
} // namespace orca::disc
