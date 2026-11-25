// BootMii keys.bin: parsing and validation of the 1024-byte dump, conversion to
// WiiKeys, the separation of common keys from the console's own keys, key choice
// by ticket index, title-key IV, the whole disc flow with a dump as the only key
// source, priority against the text key file, and reports that never show values.
// Every dump here is synthetic (tests/bootmii_dump.hpp): no real keys.
#include "orca/crypto/aes.hpp"
#include "orca/disc/bootmii_keys.hpp"
#include "orca/disc/wii_disc.hpp"
#include "orca/ios/iosc.hpp"
#include "bootmii_dump.hpp"
#include "test_framework.hpp"
#include "wii_disc_builder.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace orca;
using ios::Iosc;
using test_bootmii::DumpSpec;
using test_bootmii::make_dump;

namespace {
constexpr disc::AesKey kKorean{0x5A, 0x5A, 0x5A, 0x5A, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};

bool contains(const std::string& s, const char* what) {
    const bool ok = s.find(what) != std::string::npos;
    if (!ok) std::printf("    expected \"%s\" in: %s\n", what, s.c_str());
    return ok;
}
std::string hex(std::span<const u8> b, bool upper) {
    std::string s;
    char t[3];
    for (const u8 x : b) { std::snprintf(t, sizeof t, upper ? "%02X" : "%02x", x); s += t; }
    return s;
}
// none of the key values may appear in a text (report, error)
bool reveals(const std::string& text, std::span<const u8> key) {
    return text.find(hex(key, false)) != std::string::npos || text.find(hex(key, true)) != std::string::npos;
}
void write(const std::filesystem::path& f, const std::vector<u8>& d) {
    std::ofstream o(f, std::ios::binary);
    o.write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
}
DumpSpec spec_with(const disc::AesKey& common, std::optional<disc::AesKey> korean = std::nullopt) {
    DumpSpec s;
    s.common = common;
    s.korean = korean;
    return s;
}
} // namespace

TEST_CASE(valid_dump_fields_and_report) {
    const auto dump = make_dump(spec_with(test_wii::kTestCommon, kKorean));
    std::string err;
    const auto k = disc::parse_bootmii_keys(dump, err);
    CHECK(k.has_value());
    if (!k) { std::printf("    %s\n", err.c_str()); return; }
    CHECK(k->common_key == test_wii::kTestCommon);                       // 0x114
    CHECK(k->korean_key.has_value() && *k->korean_key == kKorean);      // 0x274
    CHECK_EQ(k->console.ng_id, 0x0badc0deU);                            // 0x124 = header ID
    CHECK_EQ(k->console.ng_priv[0], 0x40U);                             // 0x128
    CHECK_EQ(k->console.nand_hmac[0], 0x40U + 0x1CU);                   // 0x144
    CHECK_EQ(k->console.nand_key[0], 0xA0U);                            // 0x158
    CHECK_EQ(k->console.backup_key[0], 0xC0U);                          // 0x168
    CHECK_EQ(k->console.ms_id, 2U);                                     // 0x200
    CHECK_EQ(k->console.ca_id, 1U);                                     // 0x204
    CHECK_EQ(k->console.ng_key_id, 0x12345678U);                        // 0x208
    CHECK_EQ(k->console.boot2_version, 7U);                             // 0x248
    const std::string report = disc::describe_bootmii_keys(*k);
    CHECK(contains(report, "console ID in the header = OTP NG ID"));
    CHECK(contains(report, "Korean common key present"));
    CHECK(contains(report, "retail certificate IDs"));
    // the report names fields, never values
    CHECK(!reveals(report, k->common_key) && !reveals(report, kKorean) && !reveals(report, k->console.nand_key) &&
          !reveals(report, k->console.backup_key));
    CHECK(report.find("0badc0de") == std::string::npos); // not even the console ID
    // no Korean key: field zero
    const auto plain = disc::parse_bootmii_keys(make_dump(spec_with(test_wii::kTestCommon)), err);
    CHECK(plain.has_value() && !plain->korean_key.has_value());
    if (plain) CHECK(contains(disc::describe_bootmii_keys(*plain), "not a Korean console"));
}

TEST_CASE(invalid_dumps_are_refused_without_revealing_values) {
    std::string err;
    const auto good = make_dump(spec_with(test_wii::kTestCommon, kKorean));
    struct Case { const char* what; std::vector<u8> data; const char* expect; };
    std::vector<Case> cases;
    { auto d = good; d.resize(1000); cases.push_back({"truncated", d, "1000 bytes instead of 1024"}); }
    { auto d = good; d.push_back(0); cases.push_back({"too long", d, "1025 bytes"}); }
    { auto d = good; d[10] = 'X'; cases.push_back({"header", d, "does not start with"}); }
    { auto d = good; d[25] = 'z'; cases.push_back({"console ID not hex", d, "8 hex digits"}); }
    { auto d = good; d[33] = ' '; cases.push_back({"no newline", d, "not terminated"}); }
    { auto d = good; d[0x127] ^= 1; cases.push_back({"ID mismatch", d, "differs from the one in the OTP"}); }
    { auto d = good; std::memset(&d[0x114], 0, 16); cases.push_back({"common zero", d, "common key field (0x114) is empty"}); }
    { auto d = good; std::memcpy(&d[0x114], &d[0x158], 16); cases.push_back({"common = NAND key", d, "equals a console key"}); }
    { auto d = good; std::memcpy(&d[0x274], &d[0x114], 16); cases.push_back({"Korean = common", d, "Korean key field equals"}); }
    { auto d = good; std::memset(&d[0x158], 0, 16); cases.push_back({"NAND key zero", d, "NAND key field (0x158) is empty"}); }
    for (const Case& c : cases) {
        disc::WiiKeys k;
        CHECK(!disc::parse_wii_keys(c.data, k, 0, err)); // the one path every source uses
        if (err.find(c.expect) == std::string::npos) std::printf("    %s: ", c.what);
        CHECK(contains(err, c.expect));
        CHECK(!k.any() && !k.console.has_value()); // nothing half-loaded
        CHECK(!reveals(err, test_wii::kTestCommon) && !reveals(err, kKorean));
    }
    // 1024 bytes that are not a dump; and a file that does not exist
    disc::WiiKeys k;
    CHECK(!disc::parse_wii_keys(std::vector<u8>(0x400, 0x33), k, 0, err));
    CHECK(contains(err, "BackupMii"));
    CHECK(!disc::load_wii_key_file("/tmp/orca/no/such/keys.bin", k, 0, err));
    CHECK(contains(err, "cannot open"));
}

TEST_CASE(dump_converts_to_wiikeys_with_console_keys_apart) {
    std::string err;
    disc::WiiKeys k;
    CHECK(disc::parse_wii_keys(make_dump(spec_with(test_wii::kTestCommon, kKorean)), k, 0, err));
    CHECK(k.common[0].has_value() && *k.common[0] == test_wii::kTestCommon);
    CHECK(k.common[1].has_value() && *k.common[1] == kKorean);
    CHECK(!k.common[2].has_value() && !k.dev_common.has_value());
    CHECK(k.console.has_value());
    // OrcaIOSC: common keys in the common slots, the console's keys in the console entries
    Iosc iosc(Iosc::ConsoleType::Retail, k, {});
    CHECK(std::equal(iosc.entry(Iosc::HANDLE_COMMON_KEY)->data.begin(), iosc.entry(Iosc::HANDLE_COMMON_KEY)->data.end(), test_wii::kTestCommon.begin()));
    CHECK(std::equal(iosc.entry(Iosc::HANDLE_NEW_COMMON_KEY)->data.begin(), iosc.entry(Iosc::HANDLE_NEW_COMMON_KEY)->data.end(), kKorean.begin()));
    CHECK(std::equal(iosc.entry(Iosc::HANDLE_FS_KEY)->data.begin(), iosc.entry(Iosc::HANDLE_FS_KEY)->data.end(), k.console->nand_key.begin()));
    CHECK(std::equal(iosc.entry(Iosc::HANDLE_FS_MAC)->data.begin(), iosc.entry(Iosc::HANDLE_FS_MAC)->data.end(), k.console->nand_hmac.begin()));
    CHECK(std::equal(iosc.entry(Iosc::HANDLE_PRNG_KEY)->data.begin(), iosc.entry(Iosc::HANDLE_PRNG_KEY)->data.end(), k.console->backup_key.begin()));
    CHECK_EQ(iosc.entry(Iosc::HANDLE_CONSOLE_KEY)->data.size(), 30U);
    CHECK_EQ(iosc.entry(Iosc::HANDLE_CONSOLE_ID)->misc_data, 0x0badc0deU);
    CHECK_EQ(iosc.entry(Iosc::HANDLE_BOOT2_VERSION)->misc_data, 7U);
    CHECK_EQ(iosc.ms_id(), 2U);
    CHECK_EQ(iosc.ca_id(), 1U);
    CHECK(std::string(iosc.entry(Iosc::HANDLE_FS_KEY)->source) == "session (user): keys.bin");
    // a build-time bundle made from a dump keeps its common keys but never its console keys
    Iosc built(Iosc::ConsoleType::Retail, disc::WiiKeys{}, make_dump(spec_with(test_wii::kTestCommon, kKorean)));
    CHECK(built.entry(Iosc::HANDLE_COMMON_KEY)->provided);
    CHECK(std::string(built.entry(Iosc::HANDLE_FS_KEY)->source) == "fixed default");
    CHECK(!built.entry(Iosc::HANDLE_CONSOLE_KEY)->provided);
}

TEST_CASE(ticket_index_selects_the_key_and_iv_is_the_title_id) {
    std::string err;
    disc::WiiKeys k;
    CHECK(disc::parse_wii_keys(make_dump(spec_with(test_wii::kTestCommon, kKorean)), k, 0, err));
    Iosc iosc(Iosc::ConsoleType::Retail, k, {});
    const u64 tid = 0x0001000052534245ULL;
    // expected: AES-128-CBC-decrypt(key, IV = title ID || 8 zero bytes, encrypted)
    const auto encrypt_with = [&](const disc::AesKey& common) {
        std::array<u8, 16> e = test_wii::kTitleKey;
        crypto::Aes128 aes(common);
        crypto::Aes128::Block iv{0x00, 0x01, 0x00, 0x00, 0x52, 0x53, 0x42, 0x45, 0, 0, 0, 0, 0, 0, 0, 0}; // written out by hand
        (void)aes.cbc_encrypt(e, iv);
        return e;
    };
    const auto e0 = encrypt_with(test_wii::kTestCommon), e1 = encrypt_with(kKorean);
    CHECK(iosc.title_key(tid, e0, 0, err) == std::optional<disc::AesKey>(test_wii::kTitleKey)); // index 0: common
    CHECK(iosc.title_key(tid, e1, 1, err) == std::optional<disc::AesKey>(test_wii::kTitleKey)); // index 1: Korean
    CHECK(iosc.title_key(tid, e0, 1, err) != std::optional<disc::AesKey>(test_wii::kTitleKey)); // wrong key: other result
    CHECK(iosc.title_key(tid + 1U, e0, 0, err) != std::optional<disc::AesKey>(test_wii::kTitleKey)); // other IV
    CHECK(!iosc.title_key(tid, e0, 2, err).has_value());                                        // vWii: not in a keys.bin
    CHECK(contains(err, "vWii common key (index 2)"));
    CHECK(contains(err, "not provided"));
    CHECK(!iosc.title_key(tid, e0, 3, err).has_value());
    CHECK(contains(err, "unsupported common key index 3"));
}

TEST_CASE(disc_flow_with_keys_bin_as_the_only_source) {
    using namespace test_wii;
    DiscSpec spec;
    PartitionSpec a, b;
    a.plain = plain_data(3, 1);
    b.plain = plain_data(3, 2);
    b.type = 1;
    b.common_index = 1;
    b.common = kKorean;
    b.title_id = 0x0000000100000002ULL;
    b.title_key = AesKey{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00};
    spec.partitions = {a, b};
    const auto img = build_disc(spec);
    disc::MemoryDisc d(img);
    std::string err;
    const auto parts = disc::list_wii_partitions(d, err);
    CHECK(parts.has_value() && parts->size() == 2U);
    if (!parts) return;
    const auto open_both = [&](const std::vector<u8>& dump, bool expect_ok) {
        disc::WiiKeys k;
        CHECK(disc::parse_wii_keys(dump, k, 0, err));
        for (std::size_t i = 0; i < 2; ++i) {
            auto p = disc::WiiPartition::open(d, (*parts)[i].offset, k, err);
            if (!expect_ok) { CHECK(p == nullptr); continue; }
            CHECK(p != nullptr);
            if (!p) { std::printf("    %s\n", err.c_str()); continue; }
            std::vector<u8> all(p->size());
            CHECK(p->read(0, all));
            CHECK(all == (i == 0 ? a.plain : b.plain)); // ticket -> title key -> decrypted, H0-H3 checked
        }
    };
    open_both(make_dump(spec_with(kTestCommon, kKorean)), true);
    // the NAND key is never taken as the common key: a dump whose NAND key field holds the
    // disc's common key but whose common key field holds something else cannot open it
    DumpSpec wrong = spec_with(AesKey{7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7}, kKorean);
    auto dump = make_dump(wrong);
    std::memcpy(&dump[0x158], kTestCommon.data(), 16);
    disc::WiiKeys k;
    CHECK(disc::parse_wii_keys(dump, k, 0, err));
    CHECK(disc::WiiPartition::open(d, (*parts)[0].offset, k, err) == nullptr);
    CHECK(contains(err, "common key is wrong"));
    CHECK(!reveals(err, kTestCommon));
    // the Korean partition needs the Korean key: a dump of a console that is not Korean lacks it
    disc::WiiKeys nk;
    CHECK(disc::parse_wii_keys(make_dump(spec_with(kTestCommon)), nk, 0, err));
    CHECK(disc::WiiPartition::open(d, (*parts)[0].offset, nk, err) != nullptr);
    CHECK(disc::WiiPartition::open(d, (*parts)[1].offset, nk, err) == nullptr);
    CHECK(contains(err, "Korean common key (index 1)"));
}

TEST_CASE(keys_bin_with_the_text_key_file_and_import) {
    namespace fs = std::filesystem;
    ::unsetenv("ORCA_WII_KEYS");
    const fs::path dir = fs::temp_directory_path() / "orca_bootmii_keys";
    fs::remove_all(dir);
    fs::create_directories(dir);
    ::setenv("ORCA_KEYS_DIR", (dir / "none").c_str(), 1);
    ::setenv("XDG_CONFIG_HOME", (dir / "none").c_str(), 1);
    ::setenv("HOME", (dir / "none").c_str(), 1);
    std::string report, err;
    // both forms in one place: the text file comes first; a different common key in
    // keys.bin is reported, not applied (and not printed)
    write(dir / "keys.bin", make_dump(spec_with(disc::AesKey{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}, kKorean)));
    {
        std::ofstream t(dir / "wii_keys.txt");
        t << "common_key = " << hex(test_wii::kTestCommon, false) << "\n";
    }
    disc::WiiKeys k;
    CHECK(ios::load_default_key_provision({dir.string()}, k, report, err));
    CHECK(k.common[0].has_value() && *k.common[0] == test_wii::kTestCommon);
    CHECK(k.common[1].has_value() && *k.common[1] == kKorean); // only keys.bin has it
    CHECK(k.console.has_value());
    CHECK(contains(report, "common ignored: differs"));
    CHECK(!reveals(report, test_wii::kTestCommon) && !reveals(report, kKorean));
    // importing a dump: kept whole (owner-only); the text entry that differs from it is replaced
    std::string summary;
    const auto dump = make_dump(spec_with(test_wii::kTestCommon, kKorean));
    {
        std::ofstream t(dir / "wii_keys.txt");
        t << "common_key = 0102030405060708090a0b0c0d0e0f10\nvwii_common_key = " << hex(kKorean, false) << "\n";
    }
    CHECK(ios::import_key_provision(dir.string(), dump, 0, summary, err));
    CHECK(contains(summary, "keys.bin replaces the text entries: common"));
    CHECK(contains(summary, "console keys (keys.bin)"));
    CHECK(!reveals(summary, test_wii::kTestCommon));
    const auto perms = fs::status(dir / "keys.bin").permissions();
    CHECK((perms & (fs::perms::group_all | fs::perms::others_all)) == fs::perms::none);
    std::ifstream txt(dir / "wii_keys.txt");
    const std::string text((std::istreambuf_iterator<char>(txt)), std::istreambuf_iterator<char>());
    CHECK(text.find("common_key = 0102") == std::string::npos); // removed
    CHECK(text.find("vwii_common_key") != std::string::npos);    // other entries kept
    disc::WiiKeys after;
    report.clear();
    CHECK(ios::load_default_key_provision({dir.string()}, after, report, err));
    CHECK(after.common[0].has_value() && *after.common[0] == test_wii::kTestCommon);
    CHECK(after.common[2].has_value() && after.console.has_value());
    CHECK(report.find("ignored") == std::string::npos); // no conflict left
    // the earlier forms still work: a raw 16-byte key and the text file alone
    disc::WiiKeys raw;
    CHECK(disc::parse_wii_keys(test_wii::kTestCommon, raw, 0, err) && raw.common[0].has_value() && !raw.console.has_value());
    fs::remove_all(dir);
    ::unsetenv("ORCA_KEYS_DIR");
    ::unsetenv("XDG_CONFIG_HOME");
}

TEST_CASE(keys_bin_alone_in_an_empty_store) {
    // the phone case: a fresh store, only a keys.bin imported, then a Wii disc started
    namespace fs = std::filesystem;
    ::unsetenv("ORCA_WII_KEYS");
    const fs::path dir = fs::temp_directory_path() / "orca_bootmii_alone";
    fs::remove_all(dir);
    std::string summary, report, err;
    CHECK(ios::import_key_provision(dir.string(), make_dump(spec_with(test_wii::kTestCommon)), 0, summary, err));
    CHECK(contains(summary, "common"));
    CHECK(contains(summary, "console keys (keys.bin)"));
    CHECK(!fs::exists(dir / "wii_keys.txt")); // no empty text file left behind
    disc::WiiKeys k;
    CHECK(ios::load_default_key_provision({dir.string()}, k, report, err));
    CHECK(k.common[0].has_value() && *k.common[0] == test_wii::kTestCommon);
    // a store left by the earlier version: wii_keys.txt with only its comment line
    { std::ofstream t(dir / "wii_keys.txt"); t << "# Orca Core key store: keys from the user's own console\n"; }
    disc::WiiKeys k2;
    report.clear();
    CHECK(ios::load_default_key_provision({dir.string()}, k2, report, err));
    CHECK(k2.common[0].has_value() && k2.console.has_value());
    CHECK(contains(report, "no entries (skipped)"));
    // importing again over that store also works and removes the empty file
    CHECK(ios::import_key_provision(dir.string(), make_dump(spec_with(test_wii::kTestCommon)), 0, summary, err));
    CHECK(!fs::exists(dir / "wii_keys.txt"));
    fs::remove_all(dir);
}

TEST_MAIN("bootmii_keys")
