#include "orca/disc/wii_ticket.hpp"
#include <array>
#include <algorithm>

namespace orca::disc {
namespace {
int hex_digit(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
void wipe_key(crypto::Aes128Key& key) noexcept {
    volatile u8* p = key.data();
    for (std::size_t i = 0; i < key.size(); ++i) p[i] = 0;
}
} // namespace

WiiCommonKeyFileProvider::~WiiCommonKeyFileProvider() { clear(); }

void WiiCommonKeyFileProvider::clear() noexcept {
    for (auto& entry : keys_) {
        if (entry) wipe_key(*entry);
        entry.reset();
    }
}

bool WiiCommonKeyFileProvider::get_common_key(u8 index, crypto::Aes128Key& key) {
    const auto& entry = keys_[index];
    if (!entry) return false;
    key = *entry;
    return true;
}

bool WiiCommonKeyFileProvider::load_text(std::string_view text, std::string& error) {
    clear();
    std::array<std::optional<crypto::Aes128Key>, 256> parsed{};
    const auto fail = [&](std::string message) {
        for (auto& entry : parsed) if (entry) wipe_key(*entry);
        error = std::move(message);
        return false;
    };
    std::size_t line_number = 0;
    std::size_t begin = 0;
    while (begin <= text.size()) {
        ++line_number;
        const std::size_t end = text.find('\n', begin);
        std::string_view line = text.substr(begin, end == std::string_view::npos ? text.size() - begin : end - begin);
        if (const auto comment = line.find('#'); comment != std::string_view::npos) line = line.substr(0, comment);
        const auto first = line.find_first_not_of(" \t\r");
        if (first != std::string_view::npos) {
            line.remove_prefix(first);
            const auto last = line.find_last_not_of(" \t\r");
            line = line.substr(0, last + 1U);
            const auto equals = line.find('=');
            if (equals == std::string_view::npos) {
                return fail("Wii keys: expected index=32-hex-digit-key at line " + std::to_string(line_number));
            }
            auto index_text = line.substr(0, equals);
            auto key_text = line.substr(equals + 1U);
            const auto trim = [](std::string_view value) {
                const auto a = value.find_first_not_of(" \t\r");
                if (a == std::string_view::npos) return std::string_view{};
                const auto b = value.find_last_not_of(" \t\r");
                return value.substr(a, b - a + 1U);
            };
            index_text = trim(index_text);
            key_text = trim(key_text);
            unsigned index = 0;
            for (char ch : index_text) {
                if (ch < '0' || ch > '9' || index > 25U) {
                    return fail("Wii keys: invalid decimal key index at line " + std::to_string(line_number));
                }
                index = index * 10U + static_cast<unsigned>(ch - '0');
            }
            if (index_text.empty() || index > 255U || key_text.size() != 32U) {
                return fail("Wii keys: invalid index or key length at line " + std::to_string(line_number));
            }
            crypto::Aes128Key key{};
            for (std::size_t i = 0; i < key.size(); ++i) {
                const int hi = hex_digit(key_text[i * 2U]);
                const int lo = hex_digit(key_text[i * 2U + 1U]);
                if (hi < 0 || lo < 0) {
                    wipe_key(key);
                    return fail("Wii keys: non-hexadecimal key at line " + std::to_string(line_number));
                }
                key[i] = static_cast<u8>((hi << 4) | lo);
            }
            if (parsed[index]) wipe_key(*parsed[index]);
            parsed[index] = key;
            wipe_key(key);
        }
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    keys_ = parsed;
    for (auto& entry : parsed) if (entry) wipe_key(*entry);
    error.clear();
    return true;
}

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
        wipe_key(common_key);
        error="Wii ticket: no user-provided common key for slot "+std::to_string(index);
        return false;
    }
    crypto::Aes128Iv iv{};
    std::copy_n(ticket.begin()+static_cast<std::ptrdiff_t>(kTitleIdOffset),8,iv.begin());
    std::array<u8,16> encrypted{};
    std::copy_n(ticket.begin()+static_cast<std::ptrdiff_t>(kEncryptedTitleKeyOffset),16,encrypted.begin());
    if(!crypto::aes128_cbc_decrypt(common_key,iv,encrypted)) {
        wipe_key(common_key);
        error="Wii ticket: encrypted title key has invalid AES block length";
        return false;
    }
    title_key=encrypted;
    volatile u8* wipe=common_key.data();
    for(std::size_t i=0;i<common_key.size();++i) wipe[i]=0;
    return true;
}
} // namespace orca::disc
