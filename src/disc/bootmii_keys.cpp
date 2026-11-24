// Orca Core — BootMii keys.bin. See bootmii_keys.hpp.
#include "orca/disc/bootmii_keys.hpp"

#include "orca/crypto/secure.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace orca::disc {

namespace {
u32 be32(const u8* p) noexcept { return u32{p[0]} << 24 | u32{p[1]} << 16 | u32{p[2]} << 8 | p[3]; }
bool zero(std::span<const u8> b) { return std::all_of(b.begin(), b.end(), [](u8 x) { return x == 0U; }); }
bool same(std::span<const u8> a, std::span<const u8> b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); }
constexpr char kHeader[] = "BackupMii v1, ConsoleID: ";
} // namespace

ConsoleKeys::~ConsoleKeys() {
    crypto::secure_zero(ng_priv.data(), ng_priv.size());
    crypto::secure_zero(nand_hmac.data(), nand_hmac.size());
    crypto::secure_zero(nand_key.data(), nand_key.size());
    crypto::secure_zero(backup_key.data(), backup_key.size());
}

BootMiiKeys::~BootMiiKeys() {
    crypto::secure_zero(common_key.data(), common_key.size());
    if (korean_key) crypto::secure_zero(korean_key->data(), korean_key->size());
}

std::optional<BootMiiKeys> parse_bootmii_keys(std::span<const u8> d, std::string& error) {
    if (d.size() != kBootMiiDumpSize) {
        error = "keys.bin: " + std::to_string(d.size()) + " bytes instead of 1024 (truncated or not a BootMii key dump)";
        return std::nullopt;
    }
    // header: "BackupMii v1, ConsoleID: XXXXXXXX\n", NUL-terminated within 0x100 bytes
    const std::size_t hl = sizeof kHeader - 1U;
    if (std::memcmp(d.data(), kHeader, hl) != 0) {
        error = "keys.bin: the header does not start with \"BackupMii v1, ConsoleID: \"";
        return std::nullopt;
    }
    u32 header_id = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const char c = static_cast<char>(std::tolower(d[hl + i]));
        if (!std::isxdigit(static_cast<unsigned char>(c))) { error = "keys.bin: the console ID in the header is not 8 hex digits"; return std::nullopt; }
        header_id = header_id << 4 | static_cast<u32>(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    if (d[hl + 8] != '\n' || std::find(d.begin() + static_cast<std::ptrdiff_t>(hl + 9), d.begin() + 0x100, u8{0}) == d.begin() + 0x100) {
        error = "keys.bin: the header text is not terminated as BootMii writes it";
        return std::nullopt;
    }
    BootMiiKeys k;
    k.checks.emplace_back("size 1024 bytes");
    k.checks.emplace_back("BackupMii v1 header");
    // OTP
    k.console.ng_id = be32(&d[0x124]);
    if (k.console.ng_id != header_id) {
        error = "keys.bin: the console ID in the header differs from the one in the OTP block (inconsistent dump)";
        return std::nullopt;
    }
    k.checks.emplace_back("console ID in the header = OTP NG ID");
    std::copy_n(&d[0x114], 16, k.common_key.begin());
    std::copy_n(&d[0x128], 30, k.console.ng_priv.begin());
    std::copy_n(&d[0x144], 20, k.console.nand_hmac.begin());
    std::copy_n(&d[0x158], 16, k.console.nand_key.begin());
    std::copy_n(&d[0x168], 16, k.console.backup_key.begin());
    if (zero(k.common_key)) { error = "keys.bin: the common key field (0x114) is empty"; return std::nullopt; }
    if (same(k.common_key, k.console.nand_key) || same(k.common_key, k.console.backup_key)) {
        error = "keys.bin: the common key field equals a console key field (damaged dump)";
        return std::nullopt;
    }
    if (zero(k.console.nand_key)) { error = "keys.bin: the NAND key field (0x158) is empty"; return std::nullopt; }
    k.checks.emplace_back("common key present (0x114), distinct from the NAND and backup keys");
    k.checks.emplace_back("console keys present: NG private key, NAND HMAC, NAND key, backup key");
    // SEEPROM
    k.console.ms_id = be32(&d[0x200]);
    k.console.ca_id = be32(&d[0x204]);
    k.console.ng_key_id = be32(&d[0x208]);
    k.console.boot2_version = d[0x248];
    if (k.console.ms_id == 2U && k.console.ca_id == 1U) k.checks.emplace_back("SEEPROM: retail certificate IDs (MS 2, CA 1)");
    else if (k.console.ms_id == 3U && k.console.ca_id == 2U) k.checks.emplace_back("SEEPROM: development certificate IDs (MS 3, CA 2)");
    else k.checks.emplace_back("SEEPROM: certificate IDs not recognised (not used for discs)");
    std::array<u8, 16> korean{};
    std::copy_n(&d[0x274], 16, korean.begin());
    if (!zero(korean)) {
        if (same(korean, k.common_key)) { error = "keys.bin: the Korean key field equals the common key (damaged dump)"; return std::nullopt; }
        k.korean_key = korean;
        k.checks.emplace_back("Korean common key present (0x274)");
    } else {
        k.checks.emplace_back("no Korean common key (0x274 is zero: not a Korean console)");
    }
    crypto::secure_zero(korean.data(), korean.size());
    return k;
}

std::string describe_bootmii_keys(const BootMiiKeys& k) {
    std::string s = "BootMii keys.bin:";
    for (const std::string& c : k.checks) s += "\n  ok: " + c;
    s += "\n  used for discs: common key";
    if (k.korean_key) s += ", Korean common key";
    s += "\n  console keys: given to OrcaIOSC's console entries only, never used as common keys";
    return s;
}

} // namespace orca::disc
