// Disc images, MMIO page splitter, Disc Interface, and HLE disc boot through a
// real apploader (compiled with clang) on synthetic GameCube discs.
#include "orca/disc/disc_image.hpp"
#include "orca/hw/disc_interface.hpp"
#include "orca/hw/mmio_splitter.hpp"
#include "orca/system/system.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace orca;
using namespace orca::ppc;
namespace A = orca::test::ppc_asm;

extern "C" {
int integer_suite_main(void);
static std::vector<u32>* g_host_log = nullptr;
void orca_emit(u32 v) { g_host_log->push_back(v); }
}

namespace {

void put32(std::vector<u8>& v, std::size_t off, u32 x) { v[off] = u8(x >> 24); v[off + 1] = u8(x >> 16); v[off + 2] = u8(x >> 8); v[off + 3] = u8(x); }

// Synthetic GameCube disc: header, apploader at 0x2440, main DOL at 0x20000,
// test pattern (words 0xD15C0000 + i) at 0x100000; 2 MiB.
std::vector<u8> build_disc(const std::vector<u8>& apploader, const std::vector<u8>& dol, u32 app_entry = 0x81200000U) {
    std::vector<u8> d(0x200000, 0);
    const char id[] = "ORCA01";
    for (int i = 0; i < 6; ++i) d[static_cast<std::size_t>(i)] = static_cast<u8>(id[i]);
    put32(d, 0x1C, disc::kGameCubeMagic);
    const char name[] = "Orca Core synthetic test disc";
    for (std::size_t i = 0; i < sizeof name; ++i) d[0x20 + i] = static_cast<u8>(name[i]);
    put32(d, 0x420, 0x20000);
    const char date[] = "2026/11/14";
    for (std::size_t i = 0; i < sizeof date; ++i) d[0x2440 + i] = static_cast<u8>(date[i]);
    const u32 app_size = static_cast<u32>((apploader.size() + 31U) & ~std::size_t{31});
    put32(d, 0x2450, app_entry);
    put32(d, 0x2454, app_size);
    put32(d, 0x2458, 0);
    std::copy(apploader.begin(), apploader.end(), d.begin() + 0x2460);
    std::copy(dol.begin(), dol.end(), d.begin() + 0x20000);
    for (u32 i = 0; i < 0x80; ++i) put32(d, 0x100000 + i * 4, 0xD15C0000U + i);
    return d;
}

std::vector<u8> read_file(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

} // namespace

// ------------------------------------------------------------ disc image parsing
TEST_CASE(disc_header_and_apploader_parsing) {
    std::vector<u8> app(64, 0x60);
    std::vector<u8> dol(0x100, 0);
    disc::MemoryDisc d(build_disc(app, dol));
    std::string err;
    const auto h = disc::parse_header(d, err);
    CHECK(h.has_value());
    if (!h) return;
    CHECK(h->game_id == "ORCA01");
    CHECK(h->name == "Orca Core synthetic test disc");
    CHECK(h->gamecube && !h->wii);
    CHECK_EQ(h->dol_offset, 0x20000U);
    const auto a = disc::parse_apploader_header(d, err);
    CHECK(a.has_value());
    if (a) { CHECK(a->date == "2026/11/14"); CHECK_EQ(a->entry, 0x81200000U); CHECK_EQ(a->size, 64U); }
    std::vector<u8> out(16);
    CHECK(d.read(0x1FFFF0, out));
    CHECK(!d.read(0x1FFFF1, out));
    std::span<u8> empty;
    CHECK(d.read(d.size(), empty)); // zero bytes at EOF is a valid read
    CHECK(!d.read(d.size() + 1U, empty));

    disc::MemoryDisc empty_disc(std::vector<u8>{});
    CHECK(empty_disc.read(0, empty));
    CHECK(!empty_disc.read(1, empty));
}

TEST_CASE(disc_rejections) {
    std::string err;
    std::vector<u8> app(64, 0), dol(0x100, 0);
    auto wii = build_disc(app, dol); put32(wii, 0x1C, 0); put32(wii, 0x18, disc::kWiiMagic);
    disc::MemoryDisc w(wii);
    CHECK(!disc::parse_header(w, err).has_value());
    CHECK(err.find("Wii") != std::string::npos);
    auto nomagic = build_disc(app, dol); put32(nomagic, 0x1C, 0);
    disc::MemoryDisc n(nomagic);
    CHECK(!disc::parse_header(n, err).has_value());
    auto baddol = build_disc(app, dol); put32(baddol, 0x420, 0x7FFFFFFF);
    disc::MemoryDisc b(baddol);
    CHECK(!disc::parse_header(b, err).has_value());
    disc::MemoryDisc badentry(build_disc(app, dol, 0x80003100U));
    CHECK(!disc::parse_apploader_header(badentry, err).has_value());
    disc::MemoryDisc tiny(std::vector<u8>(0x100, 0));
    CHECK(!disc::parse_header(tiny, err).has_value());
}

TEST_CASE(file_disc_reads_ranges) {
    std::vector<u8> app(64, 0), dol(0x100, 0);
    const auto image = build_disc(app, dol);
    const char* path = "orca_test_disc.gcm";
    { std::ofstream f(path, std::ios::binary); f.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size())); }
    std::string err;
    auto d = disc::FileDisc::open(path, err);
    CHECK(d != nullptr);
    if (!d) return;
    CHECK_EQ(d->size(), 0x200000U);
    std::vector<u8> w(4);
    CHECK(d->read(0x100004, w));
    CHECK_EQ(w[3], 0x01U);
    CHECK(!d->read(0x1FFFFE, w));
    CHECK(d->read(0, w)); // still usable after a failed read
    CHECK_EQ(w[0], static_cast<u8>('O'));
    std::span<u8> empty;
    CHECK(d->read(d->size(), empty)); // zero bytes at EOF must not touch the stream
    CHECK(!d->read(d->size() + 1U, empty));

    const char* empty_path = "orca_test_empty_disc.gcm";
    { std::ofstream f(empty_path, std::ios::binary); }
    auto empty_disc = disc::FileDisc::open(empty_path, err);
    CHECK(empty_disc != nullptr);
    if (empty_disc) {
        CHECK_EQ(empty_disc->size(), 0U);
        CHECK(empty_disc->read(0, empty));
        CHECK(!empty_disc->read(1, empty));
    }
    std::remove(empty_path);
    CHECK(disc::FileDisc::open("/nonexistent/disc.iso", err) == nullptr);
}

// ------------------------------------------------------------ MMIO splitter
TEST_CASE(splitter_routes_sub_ranges) {
    struct Reg final : mem::MmioDevice {
        u32 v = 0;
        bool mmio_read(u32 o, unsigned, u64& x) override { x = v + o; return true; }
        bool mmio_write(u32, unsigned, u64 x) override { v = static_cast<u32>(x); return true; }
        const char* mmio_name() const noexcept override { return "Reg"; }
    } a, b;
    hw::MmioSplitter s;
    CHECK(s.attach(0x000, 0x40, a));
    CHECK(s.attach(0x400, 0x40, b));
    CHECK(!s.attach(0x020, 0x40, b)); // overlap
    CHECK(!s.attach(0xFF0, 0x20, b)); // past the page
    u64 x = 0;
    CHECK(s.mmio_write(0x404, 4, 7));
    CHECK(s.mmio_read(0x408, 4, x)); CHECK_EQ(x, 15U); // b sees offset 8
    CHECK_EQ(a.v, 0U);
    CHECK(!s.mmio_read(0x800, 4, x));                  // nothing attached (EXI)
    CHECK(s.mmio_error().find("no device") != std::string::npos);
    CHECK(!s.mmio_read(0x03E, 4, x));                  // straddles the end of a
}

// ------------------------------------------------------------ Disc Interface
namespace {
struct DiRig {
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::DiscInterface di{bus, sched, pi};
    DiRig() {
        bus.map_ram("ram", 0, 0x100000);
        std::vector<u8> app(64, 0), dol(0x100, 0);
        di.insert(std::make_unique<disc::MemoryDisc>(build_disc(app, dol)));
    }
    void w(u32 off, u32 v) { CHECK(di.mmio_write(off, 4, v)); }
    u32 r(u32 off) { u64 v = 0; CHECK(di.mmio_read(off, 4, v)); return static_cast<u32>(v); }
    void read_cmd(u32 dst, u32 len, u32 offset) { w(0x14, dst); w(0x18, len); w(0x08, 0xA8000000U); w(0x0C, offset >> 2); w(0x10, len); }
};
} // namespace

TEST_CASE(di_dma_read_completes_asynchronously) {
    DiRig t;
    t.read_cmd(0x2000, 0x40, 0x100000);
    t.w(0x1C, 3);
    CHECK(t.di.busy());                    // not instantaneous: completes at the next scheduler step
    u32 word = 0; t.bus.read32(0x2000, word);
    CHECK_EQ(word, 0U);
    t.sched.advance_to(t.sched.now() + 1);
    CHECK(!t.di.busy());
    t.bus.read32(0x2000, word); CHECK_EQ(word, 0xD15C0000U);
    t.bus.read32(0x203C, word); CHECK_EQ(word, 0xD15C000FU);
    CHECK_EQ(t.r(0x14), 0x2040U);          // DIMAR advanced
    CHECK_EQ(t.r(0x18), 0U);               // DILENGTH consumed
    CHECK_EQ(t.r(0x00) & hw::di::DISR_TCINT, hw::di::DISR_TCINT);
    CHECK(!t.pi.line());                   // TCINT masked: no interrupt
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_DI, 0U);
    t.w(0x00, hw::di::DISR_TCINT);         // write 1 to clear
    CHECK_EQ(t.r(0x00), 0U);
}

TEST_CASE(di_interrupt_and_latency) {
    DiRig t;
    t.di.set_latency_cycles(500);
    t.pi.mmio_write(0x04, 4, hw::pi::CAUSE_DI);
    t.w(0x00, hw::di::DISR_TCINTMASK);
    t.read_cmd(0x3000, 0x20, 0x100020);
    t.w(0x1C, 3);
    t.sched.advance_to(499);
    CHECK(t.di.busy());
    t.sched.advance_to(500);
    CHECK(!t.di.busy());
    CHECK(t.pi.line());                    // TCINT & TCINTMASK -> PI DI cause -> CPU line
    CHECK_EQ(t.pi.causes(), hw::pi::CAUSE_DI);
    t.w(0x00, hw::di::DISR_TCINTMASK | hw::di::DISR_TCINT); // ack, keep mask
    CHECK(!t.pi.line());
    CHECK_EQ(t.r(0x00), hw::di::DISR_TCINTMASK);
}

TEST_CASE(di_writing_masks_keeps_pending_interrupts) {
    DiRig t;
    t.read_cmd(0x2000, 0x20, 0x100000);
    t.w(0x1C, 3);
    t.sched.advance_to(1);                  // TCINT pending, masked
    CHECK(!t.pi.line());
    t.pi.mmio_write(0x04, 4, hw::pi::CAUSE_DI);
    t.w(0x00, hw::di::DISR_TCINTMASK);      // enabling the mask must not clear TCINT (only 1-bits clear)
    CHECK_EQ(t.r(0x00) & hw::di::DISR_TCINT, hw::di::DISR_TCINT);
    CHECK(t.pi.line());                     // ...so the interrupt is delivered now
}

TEST_CASE(di_errors_are_explicit) {
    DiRig t;
    t.read_cmd(0x2000, 0x20, 0x00200000);  // past the end of the 2 MiB image
    t.w(0x1C, 3);
    t.sched.advance_to(1);
    CHECK_EQ(t.r(0x00) & hw::di::DISR_DEINT, hw::di::DISR_DEINT);
    CHECK_EQ(t.r(0x00) & hw::di::DISR_TCINT, 0U);
    t.w(0x08, 0x12000000U);                // inquiry: not implemented
    CHECK(!t.di.mmio_write(0x1C, 4, 1));
    CHECK(t.di.last_error().find("0x12") != std::string::npos);
    CHECK(!t.di.busy());
    t.read_cmd(0x2000, 0x20, 0);
    CHECK(!t.di.mmio_write(0x1C, 4, 1));   // immediate mode
    CHECK(!t.di.mmio_write(0x1C, 4, 7));   // write to drive
    CHECK(!t.di.mmio_write(0x00, 4, hw::di::DISR_BRK));
    t.read_cmd(0x00FFFFE0, 0x40, 0);       // destination runs past RAM
    CHECK(!t.di.mmio_write(0x1C, 4, 3));
    t.read_cmd(0x2000, 0x20, 0);
    t.w(0x1C, 3);
    CHECK(!t.di.mmio_write(0x1C, 4, 3));   // DICR while busy
    CHECK(t.di.last_error().find("in progress") != std::string::npos);
    u64 v = 0;
    CHECK(!t.di.mmio_read(0x24, 4, v));    // DICFG not implemented
    CHECK(!t.di.mmio_read(0x00, 2, v));
    hw::DiscInterface empty(t.bus, t.sched, t.pi);
    CHECK(empty.mmio_write(0x08, 4, 0xA8000000U));
    CHECK(empty.mmio_write(0x18, 4, 0x20));
    CHECK(!empty.mmio_write(0x1C, 4, 3));
    CHECK(empty.last_error().find("no disc") != std::string::npos);
}

TEST_CASE(cpu_stop_reason_names_the_device) {
    system::System sys({});
    sys.hle_boot(0x80003000U);
    sys.bus().write32(0x3000, A::addis(3, 0, static_cast<int>(0xCC00U)));
    sys.bus().write32(0x3004, A::ori(3, 3, 0x6000));
    sys.bus().write32(0x3008, A::addi(4, 0, 3));
    sys.bus().write32(0x300C, A::stw(4, 0x1C, 3));  // DICR = TSTART|DMA with CMDBUF0 = 0 -> command 0x00
    sys.bus().write32(0x3010, A::lwz(5, 0x400, 3)); // SI (not implemented)
    const auto r = sys.run(100);
    CHECK(r.stop == StepResult::MemoryFault);
    const std::string& why = sys.cpu().last_stop().reason;
    CHECK(why.find("DI") != std::string::npos);
    CHECK(why.find("command 0x00 not implemented") != std::string::npos);
}

// ------------------------------------------------------------ HLE disc boot
#ifdef ORCA_PROGRAMS_DIR
namespace {
struct BootRun { bool booted = false; std::string error; ppc::StepResult stop{}; std::vector<u32> log; u32 exit_code = 99; u32 entry = 0; };

BootRun boot_and_run(const char* dol_name) {
    BootRun r;
    const auto app = read_file(ORCA_PROGRAMS_DIR "/apploader.bin");
    const auto dol = read_file((std::string(ORCA_PROGRAMS_DIR) + "/" + dol_name).c_str());
    CHECK(!app.empty() && !dol.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    sys.insert_disc(std::make_unique<disc::MemoryDisc>(build_disc(app, dol)));
    r.booted = sys.boot_disc(r.error);
    if (!r.booted) { std::fprintf(stderr, "    boot_disc: %s\n", r.error.c_str()); return r; }
    r.entry = sys.cpu().state().pc;
    const auto res = sys.run(4000000000ULL);
    r.stop = res.stop;
    r.log = port.log;
    r.exit_code = port.exit_code;
    if (res.stop != StepResult::Halted)
        std::fprintf(stderr, "    %s stopped: %s (%s) pc=%08X ins=%08X srr0=%08X srr1=%08X last_exc=%03X\n", dol_name, to_string(res.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc, sys.cpu().last_stop().instruction,
                     sys.cpu().state().srr0, sys.cpu().state().srr1, static_cast<unsigned>(sys.cpu().last_exception()));
    return r;
}
} // namespace

TEST_CASE(boot_integer_suite_from_disc_via_apploader) {
    std::vector<u32> host;
    g_host_log = &host;
    CHECK_EQ(integer_suite_main(), 0);
    g_host_log = nullptr;
    const BootRun r = boot_and_run("integer_suite.dol");
    CHECK(r.booted);
    CHECK_EQ(r.entry, 0x80003100U);
    CHECK(r.stop == StepResult::Halted);
    CHECK_EQ(r.exit_code, 0U);
    CHECK(r.log == host);
    std::printf("[disc] integer_suite booted from disc via apploader: %zu values %s\n", r.log.size(), r.log == host ? "identical to host" : "MISMATCH");
}

TEST_CASE(boot_dvd_suite_reads_disc_through_di) {
    const BootRun r = boot_and_run("dvd_suite.dol");
    CHECK(r.booted);
    CHECK(r.stop == StepResult::Halted);
    CHECK_EQ(r.exit_code, 0U);
    const std::vector<u32> expected = {
        0x4F524341U,               // "ORCA": disc header copied to 0x80000000 by the IPL HLE
        486000000U,                // CPU clock global
        0xD15C0000U, 0xD15C000FU,  // polled DMA read
        0x10U, 0U, 0x40U,          // TCINT set, DILENGTH 0, DIMAR advanced
        1U,                        // one DI interrupt handled at vector 0x500
        0xD15C0010U, 0xD15C0017U,  // interrupt-driven read
        0U,                        // DI cause cleared after acknowledging TCINT
        1U,                        // reading past the disc end sets DEINT
    };
    CHECK(r.log == expected);
    if (r.log != expected) for (u32 v : r.log) std::fprintf(stderr, "    got %08X\n", v);
    std::printf("[disc] dvd_suite booted from disc: %zu values %s\n", r.log.size(), r.log == expected ? "as expected" : "MISMATCH");
}

TEST_CASE(boot_disc_failures_are_reported) {
    system::System sys({});
    std::string err;
    CHECK(!sys.boot_disc(err));
    CHECK(err.find("no disc") != std::string::npos);
    // apploader whose entry immediately executes an unimplemented instruction
    std::vector<u8> bad_app(64, 0); // 0x00000000 words
    std::vector<u8> dol(0x100, 0);
    sys.insert_disc(std::make_unique<disc::MemoryDisc>(build_disc(bad_app, dol)));
    CHECK(!sys.boot_disc(err));
    CHECK(err.find("did not return") != std::string::npos);
}
#endif

TEST_MAIN("disc")
