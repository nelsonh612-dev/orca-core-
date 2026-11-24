// OrcaIOSC: default entries (handles, types, owner masks as IOS), retrieval,
// ownership and type checks, common key selection by ticket index (common,
// Korean, vWii extension, unknown), console types, title-key derivation with
// synthetic values, and the automatic key provision. Every key value here is a
// test value that exists only in the tests.
#include "orca/crypto/aes.hpp"
#include "orca/ios/iosc.hpp"
#include "test_framework.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace orca;
using namespace orca::ios;
using disc::AesKey;

namespace {
constexpr AesKey kCommon{0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr AesKey kKorean{0x5A, 0x5A, 0x5A, 0x5A, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
constexpr AesKey kDev{0xDE, 0xAD, 0xBE, 0xEF, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
constexpr AesKey kVwii{0x77, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
constexpr AesKey kTitle{0xA0, 0xA1, 0xA2, 0xA3, 0xB0, 0xB1, 0xB2, 0xB3, 0xC0, 0xC1, 0xC2, 0xC3, 0xD0, 0xD1, 0xD2, 0xD3};

std::array<u8, 16> encrypt_title_key(const AesKey& common, u64 title_id) {
    std::array<u8, 16> enc = kTitle;
    crypto::Aes128 aes(common);
    crypto::Aes128::Block iv{};
    for (unsigned k = 0; k < 8; ++k) iv[k] = static_cast<u8>(title_id >> (56U - 8U * k));
    (void)aes.cbc_encrypt(enc, iv);
    return enc;
}

bool contains(const std::string& s, const char* what) {
    const bool ok = s.find(what) != std::string::npos;
    if (!ok) std::printf("    expected \"%s\" in: %s\n", what, s.c_str());
    return ok;
}

struct TempDir {
    std::filesystem::path p;
    explicit TempDir(const char* n) : p(std::filesystem::temp_directory_path() / (std::string("orca_iosc_") + n)) {
        std::filesystem::remove_all(p);
        std::filesystem::create_directories(p);
    }
    ~TempDir() { std::filesystem::remove_all(p); }
};

void write(const std::filesystem::path& f, const void* data, std::size_t n) {
    std::ofstream o(f, std::ios::binary);
    o.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
}
} // namespace

TEST_CASE(iosc_default_entries_match_ios) {
    const Iosc iosc(Iosc::ConsoleType::Retail, disc::WiiKeys{});
    struct E { Iosc::Handle h; Iosc::ObjectType t; Iosc::ObjectSubType st; u32 owner; };
    const E expect[] = {
        {Iosc::HANDLE_CONSOLE_KEY, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::ECC233, 3},
        {Iosc::HANDLE_CONSOLE_ID, Iosc::TYPE_DATA, Iosc::ObjectSubType::Data, 0xFFFFFFF},
        {Iosc::HANDLE_FS_KEY, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::AES128, 5},
        {Iosc::HANDLE_FS_MAC, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::MAC, 5},
        {Iosc::HANDLE_COMMON_KEY, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::AES128, 3},
        {Iosc::HANDLE_PRNG_KEY, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::AES128, 3},
        {Iosc::HANDLE_SD_KEY, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::AES128, 3},
        {Iosc::HANDLE_BOOT2_VERSION, Iosc::TYPE_DATA, Iosc::ObjectSubType::Version, 3},
        {Iosc::HANDLE_UNKNOWN_8, Iosc::TYPE_DATA, Iosc::ObjectSubType::Version, 3},
        {Iosc::HANDLE_UNKNOWN_9, Iosc::TYPE_DATA, Iosc::ObjectSubType::Version, 3},
        {Iosc::HANDLE_FS_VERSION, Iosc::TYPE_DATA, Iosc::ObjectSubType::Version, 3},
        {Iosc::HANDLE_NEW_COMMON_KEY, Iosc::TYPE_SECRET_KEY, Iosc::ObjectSubType::AES128, 3},
    };
    for (const E& e : expect) {
        const auto* entry = iosc.entry(e.h);
        CHECK(entry != nullptr);
        if (!entry) continue;
        CHECK(entry->in_use);
        CHECK(entry->type == e.t);
        CHECK(entry->subtype == e.st);
        CHECK_EQ(entry->owner_mask, e.owner);
    }
    CHECK_EQ(static_cast<u32>(Iosc::HANDLE_COMMON_KEY), 4U);
    CHECK_EQ(static_cast<u32>(Iosc::HANDLE_NEW_COMMON_KEY), 11U);
    const auto* root = iosc.entry(Iosc::HANDLE_ROOT_KEY);
    CHECK(root != nullptr && root->type == Iosc::TYPE_PUBLIC_KEY && root->subtype == Iosc::ObjectSubType::RSA4096);
    CHECK(iosc.entry(12) == nullptr);
    CHECK(iosc.entry(0x1234) == nullptr);
    // Nintendo's values are not in Orca: without a provision they are "not provided"
    CHECK(!iosc.entry(Iosc::HANDLE_COMMON_KEY)->provided);
    CHECK(!iosc.entry(Iosc::HANDLE_NEW_COMMON_KEY)->provided);
    CHECK(!iosc.entry(Iosc::HANDLE_SD_KEY)->provided);
    CHECK(!root->provided);
    CHECK(iosc.entry(Iosc::HANDLE_FS_KEY)->provided); // zero, as Dolphin's defaults
}

TEST_CASE(iosc_decrypt_checks_ownership_type_and_presence) {
    disc::WiiKeys p;
    // NIST SP 800-38A F.2.2 (CBC-AES128.Decrypt), first block, as a synthetic "common key"
    p.common[0] = AesKey{0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    const Iosc iosc(Iosc::ConsoleType::Retail, p);
    std::string err;
    std::array<u8, 16> iv{0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    const u8 cipher[16] = {0x76, 0x49, 0xab, 0xac, 0x81, 0x19, 0xb2, 0x46, 0xce, 0xe9, 0x8e, 0x9b, 0x12, 0xe9, 0x19, 0x7d};
    const u8 plain[16] = {0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
    std::array<u8, 16> out{};
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_COMMON_KEY, iv, cipher, out, Iosc::PID_ES, err), Iosc::IPC_SUCCESS);
    CHECK(std::memcmp(out.data(), plain, 16) == 0);
    std::array<u8, 16> iv2{};
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_COMMON_KEY, iv2, cipher, out, Iosc::PID_FS, err), Iosc::IOSC_EACCES); // owner mask 3
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_FS_KEY, iv2, cipher, out, Iosc::PID_ES, err), Iosc::IOSC_EACCES);     // owner mask 5
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_FS_MAC, iv2, cipher, out, Iosc::PID_FS, err), Iosc::IOSC_INVALID_OBJTYPE);
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_CONSOLE_ID, iv2, cipher, out, Iosc::PID_KERNEL, err), Iosc::IOSC_INVALID_OBJTYPE);
    CHECK_EQ(iosc.decrypt(40, iv2, cipher, out, Iosc::PID_KERNEL, err), Iosc::IOSC_EACCES);
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_NEW_COMMON_KEY, iv2, cipher, out, Iosc::PID_ES, err), Iosc::IOSC_FAIL_INTERNAL);
    CHECK(contains(err, "Korean common key (index 1)"));
    CHECK(contains(err, "not provided"));
    std::array<u8, 8> odd{};
    CHECK_EQ(iosc.decrypt(Iosc::HANDLE_COMMON_KEY, iv2, odd, odd, Iosc::PID_ES, err), Iosc::IOSC_EINVAL);
}

TEST_CASE(iosc_selects_the_common_key_by_ticket_index) {
    disc::WiiKeys p;
    p.common[0] = kCommon;
    p.common[1] = kKorean;
    p.common[2] = kVwii;
    p.dev_common = kDev;
    const Iosc retail(Iosc::ConsoleType::Retail, p);
    std::string err;
    CHECK(retail.common_key_handle(0, err) == std::optional<Iosc::Handle>(Iosc::HANDLE_COMMON_KEY));
    CHECK(retail.common_key_handle(1, err) == std::optional<Iosc::Handle>(Iosc::HANDLE_NEW_COMMON_KEY));
    CHECK(retail.common_key_handle(2, err) == std::optional<Iosc::Handle>(Iosc::kOrcaVwiiCommonKey));
    CHECK(!retail.common_key_handle(3, err).has_value());
    CHECK(contains(err, "unsupported common key index 3"));
    const u64 tid = 0x0001000052534245ULL;
    for (const auto& [index, key] : {std::pair<u8, AesKey>{0, kCommon}, {1, kKorean}, {2, kVwii}}) {
        const auto enc = encrypt_title_key(key, tid);
        const auto tk = retail.title_key(tid, enc, index, err);
        CHECK(tk.has_value() && *tk == kTitle);
        // the same encrypted key under another index gives something else
        const auto other = retail.title_key(tid, enc, static_cast<u8>((index + 1) % 3), err);
        CHECK(other.has_value() && *other != kTitle);
    }
    // the IV is the title ID followed by 8 zero bytes
    const auto enc = encrypt_title_key(kCommon, tid);
    const auto shifted = retail.title_key(tid + 1, enc, 0, err);
    CHECK(shifted.has_value() && *shifted != kTitle);
    CHECK(!retail.title_key(tid, enc, 9, err).has_value());
    // development (RVT) consoles: their common key in handle 4
    CHECK(Iosc::console_type_of("Root-CA00000002-XS00000006") == Iosc::ConsoleType::RVT);
    CHECK(Iosc::console_type_of("Root-CA00000001-XS00000003") == Iosc::ConsoleType::Retail);
    const Iosc rvt(Iosc::ConsoleType::RVT, p);
    const auto dev_tk = rvt.title_key(tid, encrypt_title_key(kDev, tid), 0, err);
    CHECK(dev_tk.has_value() && *dev_tk == kTitle);
    // a missing key: precise error
    disc::WiiKeys only_common;
    only_common.common[0] = kCommon;
    const Iosc partial(Iosc::ConsoleType::Retail, only_common);
    CHECK(!partial.title_key(tid, encrypt_title_key(kKorean, tid), 1, err).has_value());
    CHECK(contains(err, "Korean"));
    const Iosc rvt_missing(Iosc::ConsoleType::RVT, only_common);
    CHECK(!rvt_missing.title_key(tid, encrypt_title_key(kDev, tid), 0, err).has_value());
    CHECK(contains(err, "development common key"));
}

TEST_CASE(iosc_title_keys_through_the_disc_layer) {
    // disc::decrypt_title_key goes through OrcaIOSC and names the missing entry
    disc::WiiTicket t;
    t.title_id = 0x0001000052534245ULL;
    t.encrypted_title_key = encrypt_title_key(kCommon, t.title_id);
    t.issuer = "Root-CA00000001-XS00000003";
    disc::WiiKeys none;
    std::string err;
    CHECK(!disc::decrypt_title_key(t, none, err).has_value());
    CHECK(contains(err, "common key (index 0) (IOSC handle 4)"));
    CHECK(contains(err, "not provided"));
    CHECK(contains(err, "orca_disc import-keys"));
    disc::WiiKeys k;
    k.common[0] = kCommon;
    const auto tk = disc::decrypt_title_key(t, k, err);
    CHECK(tk.has_value() && *tk == kTitle);
}

TEST_CASE(iosc_automatic_key_provision) {
    TempDir a("a"), b("b"), env("env");
    std::string report, err;
    { // nothing anywhere: success, nothing loaded
        disc::WiiKeys k;
        ::unsetenv("ORCA_WII_KEYS");
        ::setenv("ORCA_KEYS_DIR", (a.p / "missing").c_str(), 1);
        ::setenv("XDG_CONFIG_HOME", (a.p / "missing").c_str(), 1);
        ::setenv("HOME", (a.p / "missing").c_str(), 1);
        CHECK(load_default_key_provision({a.p.string()}, k, report, err));
        CHECK(!k.any());
        CHECK(report.empty());
    }
    // a: common-key.bin and korean-common-key.bin; b: wii_keys.txt with a different common key
    write(a.p / "common-key.bin", kCommon.data(), 16);
    write(a.p / "korean-common-key.bin", kKorean.data(), 16);
    {
        std::ofstream t(b.p / "wii_keys.txt");
        t << "common_key = 00000000000000000000000000000001\ndev_common_key = deadbeef000102030405060708090a0b\n";
    }
    {
        disc::WiiKeys k;
        report.clear();
        CHECK(load_default_key_provision({a.p.string(), b.p.string()}, k, report, err));
        CHECK(k.common[0].has_value() && *k.common[0] == kCommon); // the first place wins
        CHECK(k.common[1].has_value() && *k.common[1] == kKorean);
        CHECK(k.dev_common.has_value() && *k.dev_common == kDev);
        CHECK(contains(report, "common-key.bin: common"));
        CHECK(contains(report, "wii_keys.txt: dev"));
    }
    { // slots already set are kept; $ORCA_WII_KEYS comes first
        write(env.p / "mine.bin", kVwii.data(), 16);
        ::setenv("ORCA_WII_KEYS", (env.p / "mine.bin").c_str(), 1);
        disc::WiiKeys k;
        k.common[1] = kVwii;
        report.clear();
        CHECK(load_default_key_provision({a.p.string()}, k, report, err));
        CHECK(k.common[0].has_value() && *k.common[0] == kVwii); // from $ORCA_WII_KEYS
        CHECK(k.common[1].has_value() && *k.common[1] == kVwii); // kept
        ::unsetenv("ORCA_WII_KEYS");
    }
    { // BootMii's 0x400-byte dump contains common and Korean common keys
        TempDir c("c");
        std::vector<u8> dump(0x400, 0x33);
        std::memcpy(dump.data(), "BackupMii v1, ConsoleID: 0badc0de\n", 34);
        std::memcpy(&dump[0x114], kCommon.data(), 16);
        std::memcpy(&dump[0x274], kKorean.data(), 16);
        write(c.p / "keys.bin", dump.data(), dump.size());
        disc::WiiKeys k;
        CHECK(load_default_key_provision({c.p.string()}, k, report, err));
        CHECK(k.common[0].has_value() && *k.common[0] == kCommon);
        CHECK(k.common[1].has_value() && *k.common[1] == kKorean);
        CHECK(!k.common[2].has_value() && !k.dev_common.has_value());
        // A dump with one populated key slot remains useful.
        std::fill(dump.begin() + 0x114, dump.begin() + 0x124, 0);
        write(c.p / "keys.bin", dump.data(), dump.size());
        disc::WiiKeys korean_only;
        CHECK(load_default_key_provision({c.p.string()}, korean_only, report, err));
        CHECK(!korean_only.common[0].has_value());
        CHECK(korean_only.common[1].has_value() && *korean_only.common[1] == kKorean);
        std::vector<u8> bad(0x400, 0x33); // 1 KiB, not a BootMii dump
        write(c.p / "keys.bin", bad.data(), bad.size());
        disc::WiiKeys k2;
        CHECK(!load_default_key_provision({c.p.string()}, k2, report, err));
        CHECK(contains(err, "BackupMii"));
    }
    { // a broken file in an automatic place is reported, not ignored
        // (not 16 bytes long: a 16-byte file is read as a raw key)
        write(b.p / "wii_keys.txt", "common_key = 1234\n", 18);
        disc::WiiKeys k;
        CHECK(!load_default_key_provision({b.p.string()}, k, report, err));
        CHECK(contains(err, "32 hex digits"));
    }
    ::unsetenv("ORCA_KEYS_DIR");
    ::unsetenv("XDG_CONFIG_HOME");
}

TEST_MAIN("iosc")
