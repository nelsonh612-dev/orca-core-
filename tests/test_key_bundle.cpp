// The build-time key bundle path of OrcaIOSC: this program is linked with a bundle
// made from tests/test_key_bundle.txt (test-only values, not Nintendo's) exactly as
// a build with -DORCA_DEFAULT_KEYS_FILE is. It checks that the default entries take
// those values, that the session's values replace them, and that encrypted Wii
// partitions open and a Wii program boots and reads its disc with no key given by
// the user at all (no key file, no key directory, automatic provision off).
#include "orca/disc/wii_disc.hpp"
#include "orca/ios/iosc.hpp"
#include "orca/ios/key_bundle.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"
#include "wii_disc_builder.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace orca;
using ios::Iosc;

namespace {
constexpr disc::AesKey kKorean{0x5A, 0x5A, 0x5A, 0x5A, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
} // namespace

TEST_CASE(this_build_carries_the_test_bundle) {
    CHECK(!ios::build_time_key_bundle().empty());
    CHECK(std::string(ios::build_time_key_bundle_origin()) == "test_key_bundle.txt");
    CHECK(ios::default_values_status() == "common, Korean");
}

TEST_CASE(default_entries_take_the_bundle_values) {
    Iosc retail(Iosc::ConsoleType::Retail, disc::WiiKeys{});
    CHECK(retail.bundle_error().empty());
    const auto* c = retail.entry(Iosc::HANDLE_COMMON_KEY);
    CHECK(c != nullptr && c->provided && std::string(c->source) == "build-time default values");
    if (c) CHECK(std::equal(c->data.begin(), c->data.end(), test_wii::kTestCommon.begin()));
    const auto* k = retail.entry(Iosc::HANDLE_NEW_COMMON_KEY);
    CHECK(k != nullptr && k->provided && std::equal(k->data.begin(), k->data.end(), kKorean.begin()));
    CHECK(!retail.entry(Iosc::kOrcaVwiiCommonKey)->provided); // not in the bundle
    // the RVT profile has its own values: the bundle has no development key
    Iosc rvt(Iosc::ConsoleType::RVT, disc::WiiKeys{});
    CHECK(!rvt.entry(Iosc::HANDLE_COMMON_KEY)->provided);
    // the session's values (the user's console) replace the defaults
    disc::WiiKeys session;
    session.common[0] = disc::AesKey{9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9};
    Iosc mixed(Iosc::ConsoleType::Retail, session);
    CHECK(std::string(mixed.entry(Iosc::HANDLE_COMMON_KEY)->source) == "session (user)");
    CHECK(mixed.entry(Iosc::HANDLE_COMMON_KEY)->data[0] == 9U);
    CHECK(std::string(mixed.entry(Iosc::HANDLE_NEW_COMMON_KEY)->source) == "build-time default values");
}

TEST_CASE(encrypted_partitions_open_without_user_keys) {
    using namespace test_wii;
    DiscSpec spec;
    PartitionSpec a, b;
    a.plain = plain_data(3, 1);
    b.plain = plain_data(3, 2);
    b.type = 1;
    b.common_index = 1;
    b.common = kKorean;
    b.title_id = 0x0000000100000002ULL;
    spec.partitions = {a, b};
    disc::MemoryDisc d(build_disc(spec));
    std::string err;
    const auto parts = disc::list_wii_partitions(d, err);
    CHECK(parts.has_value() && parts->size() == 2U);
    if (!parts) return;
    for (std::size_t i = 0; i < 2; ++i) {
        auto p = disc::WiiPartition::open(d, (*parts)[i].offset, disc::WiiKeys{}, err); // no key from the user
        CHECK(p != nullptr);
        if (!p) { std::printf("    %s\n", err.c_str()); continue; }
        std::vector<u8> all(p->size());
        CHECK(p->read(0, all));
        CHECK(all == (i == 0 ? a.plain : b.plain));
    }
}

#ifdef ORCA_PROGRAMS_DIR
namespace {
std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void be32(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); }
} // namespace

TEST_CASE(wii_program_boots_with_no_key_from_the_user) {
    const auto app = read_file(ORCA_PROGRAMS_DIR "/apploader_wii.bin");
    const auto dol = read_file(ORCA_PROGRAMS_DIR "/wii_dvd_suite.dol");
    CHECK(!app.empty() && !dol.empty());
    std::vector<u8> plain = test_wii::plain_data(10);
    std::fill(plain.begin() + 0x20, plain.begin() + 0x8000, u8{0});
    be32(plain, 0x420, 0x20000 >> 2);
    be32(plain, 0x2450, 0x81200000U);
    be32(plain, 0x2454, static_cast<u32>((app.size() + 31U) & ~std::size_t{31}));
    std::copy(app.begin(), app.end(), plain.begin() + 0x2460);
    std::copy(dol.begin(), dol.end(), plain.begin() + 0x20000);
    for (u32 i = 0; i < 0x1000; ++i) be32(plain, 0x44000 + 4 * i, 0xA5000000U + i);
    const auto root = std::filesystem::temp_directory_path() / "orca_key_bundle_nand";
    std::filesystem::remove_all(root);
    {
        ios::NandFs nand(root);
        std::string e;
        CHECK(nand.load(e));
        for (const char* p : {"/title", "/title/00000001", "/title/00000001/00000002", "/title/00000001/00000002/data"})
            CHECK_EQ(nand.create(0, 0, p, 0, {3, 3, 1}, false), 0);
        CHECK_EQ(nand.create(0, 0, "/title/00000001/00000002/data/setting.txt", 0, {3, 3, 1}, true), 0);
        const s32 h = nand.open(0, 0, "/title/00000001/00000002/data/setting.txt", 3);
        CHECK_EQ(nand.write(h, std::vector<u8>(0x100, 0x11)), 0x100);
        nand.close(h);
    }
    system::SystemConfig cfg;
    cfg.console = system::Console::Wii;
    cfg.nand_root = root.string();
    cfg.auto_keys = false; // no key file, no key directory: only the built-in default values
    system::System sys(cfg);
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    sys.insert_disc(std::make_unique<disc::MemoryDisc>(test_wii::build_disc(plain)));
    std::string error;
    const bool booted = sys.boot_disc(error);
    if (!booted) std::printf("    %s\n", error.c_str());
    CHECK(booted);
    if (booted) {
        const auto r = sys.run(4000000000ULL);
        CHECK(r.stop == ppc::StepResult::Halted && port.exited && port.exit_code == 0U);
        CHECK(port.log.size() == 16U && port.log[5] == 0xA5000000U);
        std::printf("[key_bundle] Wii program booted and read its disc with the built-in default values only: %zu values\n", port.log.size());
    }
    std::filesystem::remove_all(root);
}
#endif

TEST_MAIN("key_bundle")
