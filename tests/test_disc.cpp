// Disc images, MMIO page splitter, Disc Interface, and HLE disc boot through a
// real apploader (compiled with clang) on synthetic GameCube discs.
#include "orca/disc/blob.hpp"
#include "orca/disc/disc_image.hpp"
#include "orca/hw/disc_interface.hpp"
#include "orca/hw/mmio_splitter.hpp"
#include "orca/system/system.hpp"
#include "image_writers.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <fcntl.h>

#include <algorithm>
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
}

TEST_CASE(disc_rejections) {
    std::string err;
    std::vector<u8> app(64, 0), dol(0x100, 0);
    auto wii = build_disc(app, dol); put32(wii, 0x1C, 0); put32(wii, 0x18, disc::kWiiMagic);
    disc::MemoryDisc w(wii);
    // a Wii disc's outer header is recognised (its DOL and FST are inside the game partition)
    const auto wh = disc::parse_header(w, err);
    CHECK(wh.has_value() && wh->wii && !wh->gamecube);
    // ... but a GameCube system refuses to boot it, with the reason
    {
        system::System gc({});
        gc.insert_disc(std::make_unique<disc::MemoryDisc>(wii));
        CHECK(!gc.boot_disc(err));
        CHECK(err.find("is a Wii disc") != std::string::npos);
    }
    auto nomagic = build_disc(app, dol); put32(nomagic, 0x1C, 0);
    disc::MemoryDisc n(nomagic);
    CHECK(!disc::parse_header(n, err).has_value());
    CHECK(err.find("neither") != std::string::npos);
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
    CHECK(disc::FileDisc::open("/nonexistent/disc.iso", err) == nullptr);
}

// ------------------------------------------------------------ image formats
TEST_CASE(image_formats_read_like_the_raw_image) {
    std::vector<u8> app(64, 0), dol(0x100, 0);
    const auto raw = build_disc(app, dol);
    std::string err;
    struct Case { const char* name; std::vector<u8> file; disc::ImageFormat format; };
    std::vector<Case> cases;
    cases.push_back({"raw", raw, disc::ImageFormat::Raw});
    cases.push_back({"CISO 32 KiB", test_images::make_ciso(raw, 0x8000), disc::ImageFormat::Ciso});
    cases.push_back({"CISO 2 KiB", test_images::make_ciso(raw, 0x800), disc::ImageFormat::Ciso});
#if defined(ORCA_HAVE_ZLIB)
    cases.push_back({"GCZ 16 KiB", test_images::make_gcz(raw, 0x4000), disc::ImageFormat::Gcz});
    CHECK(disc::gcz_supported());
#endif
    for (const Case& c : cases) {
        disc::ImageFormat f{};
        auto d = disc::open_image(std::make_unique<disc::MemoryDisc>(c.file), &f, err);
        CHECK(d != nullptr);
        if (!d) { std::printf("    %s: %s\n", c.name, err.c_str()); continue; }
        CHECK(f == c.format);
        CHECK(d->size() >= raw.size());
        std::vector<u8> all(raw.size());
        CHECK(d->read(0, all));
        CHECK(all == raw);
        for (const u64 at : {u64{0x7FF}, u64{0x3FFF}, u64{0x100000 - 3}}) { // unaligned, across blocks
            std::vector<u8> piece(0x4005);
            CHECK(d->read(at, piece));
            CHECK(std::equal(piece.begin(), piece.end(), raw.begin() + static_cast<std::ptrdiff_t>(at)));
        }
        std::vector<u8> past(8);
        CHECK(!d->read(d->size() - 4U, past));
        const auto h = disc::parse_header(*d, err);
        CHECK(h.has_value() && h->game_id == "ORCA01");
    }
    { // NKit images are raw-compatible
        auto n = raw;
        std::memcpy(&n[0x200], "NKIT", 4);
        disc::ImageFormat f{};
        CHECK(disc::open_image(std::make_unique<disc::MemoryDisc>(n), &f, err) != nullptr);
        CHECK(f == disc::ImageFormat::NKit);
    }
    { // from a file and from a file descriptor
        const char* path = "/tmp/orca_test_image.ciso";
        {
            const auto c = test_images::make_ciso(raw, 0x8000);
            std::ofstream o(path, std::ios::binary);
            o.write(reinterpret_cast<const char*>(c.data()), static_cast<std::streamsize>(c.size()));
        }
        disc::ImageFormat f{};
        auto d = disc::open_image_file(path, &f, err);
        CHECK(d != nullptr && f == disc::ImageFormat::Ciso);
        auto e = disc::open_image_fd(::open(path, O_RDONLY), &f, err);
        CHECK(e != nullptr && f == disc::ImageFormat::Ciso);
        std::vector<u8> w(4);
        if (e) { CHECK(e->read(0x100004, w)); CHECK_EQ(w[3], 0x01U); }
        CHECK(disc::open_image_file("/nonexistent/x.iso", &f, err) == nullptr);
    }
}

TEST_CASE(image_format_errors_are_explicit) {
    std::vector<u8> app(64, 0), dol(0x100, 0);
    const auto raw = build_disc(app, dol);
    std::string err;
    const auto open = [&](std::vector<u8> file) { return disc::open_image(std::make_unique<disc::MemoryDisc>(std::move(file)), nullptr, err); };
    const auto refused = [&](std::vector<u8> file, const char* why) {
        CHECK(open(std::move(file)) == nullptr);
        const bool ok = err.find(why) != std::string::npos;
        if (!ok) std::printf("    expected \"%s\" in: %s\n", why, err.c_str());
        CHECK(ok);
    };
    std::vector<u8> m(0x1000, 0);
    std::memcpy(m.data(), "RVZ\x01", 4); refused(m, "RVZ images are not supported");
    std::memcpy(m.data(), "WIA\x01", 4); refused(m, "WIA images are not supported");
    std::memcpy(m.data(), "EGGS", 4); refused(m, "NFS");
    m[0] = 0xAE; m[1] = 0x0F; m[2] = 0x38; m[3] = 0xA2; refused(m, "TGC");
    std::fill(m.begin(), m.end(), 0x5A); refused(m, "not a GameCube or Wii disc");
    refused(std::vector<u8>(2, 0), "too small");
    { auto c = test_images::make_ciso(raw, 0x8000); c[8 + 3] = 7; refused(c, "invalid map entry"); }
    { auto c = test_images::make_ciso(raw, 0x8000); c[4] = 3; refused(c, "power of two"); }
    { auto c = test_images::make_ciso(raw, 0x8000); c.resize(c.size() - 0x100); refused(c, "truncated"); }
#if defined(ORCA_HAVE_ZLIB)
    { auto g = test_images::make_gcz(raw, 0x4000); g.resize(g.size() - 0x10); refused(g, "truncated"); }
    { auto g = test_images::make_gcz(raw, 0x4000); test_images::le32(g, 24, 0); refused(g, "block size"); }
    { // a corrupt block: reads of other blocks work, that one fails with the reason
        auto g = test_images::make_gcz(raw, 0x4000);
        const std::size_t data = 32 + 12U * (raw.size() / 0x4000);
        u64 p3 = 0;
        for (unsigned k = 0; k < 8; ++k) p3 |= u64{g[32 + 8 * 3 + k]} << (8 * k);
        g[data + static_cast<std::size_t>(p3 & ~(1ULL << 63)) + 2] ^= 0xFF;
        auto d = open(g);
        CHECK(d != nullptr);
        if (d) {
            std::vector<u8> b(0x100);
            CHECK(d->read(0, b));
            CHECK(!d->read(3U * 0x4000U + 8U, b));
            CHECK(d->failure_reason().find("block 3") != std::string::npos);
            CHECK(d->failure_reason().find("Adler-32") != std::string::npos);
        }
    }
#endif
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
        di.after_ipl_boot(false, 0); // as the IPL leaves the drive: ID read, ready
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
    t.w(0x08, 0x5A000000U);                // an undocumented command
    CHECK(!t.di.mmio_write(0x1C, 4, 1));
    CHECK(t.di.last_error().find("0x5A") != std::string::npos);
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
    // DICFG (0x24): read-only configuration, CONFIG = 1 after the boot ROM
    u64 cfg = 0xFFFFFFFFU;
    CHECK(t.di.mmio_read(0x24, 4, cfg));
    CHECK_EQ(cfg, 1U);
    CHECK(!t.di.mmio_write(0x24, 4, 0));
    CHECK(t.di.last_error().find("read-only") != std::string::npos);
    u64 none = 0;
    CHECK(!t.di.mmio_read(0x28, 4, none)); // past DICFG: no register
    u64 v = 0;
    CHECK(!t.di.mmio_read(0x00, 2, v));
    t.sched.advance_to(t.sched.now() + 1);
    t.w(0x08, 0xA8000080U);                // read sub-command not documented
    CHECK(!t.di.mmio_write(0x1C, 4, 3));
    t.w(0x08, 0xE3020000U);                // stop motor with eject
    CHECK(!t.di.mmio_write(0x1C, 4, 1));
    CHECK(t.di.last_error().find("eject") != std::string::npos);
    // no disc: the drive answers "medium not present" (DEINT), the cover reads open
    hw::DiscInterface empty(t.bus, t.sched, t.pi);
    u64 cvr = 0;
    CHECK(empty.mmio_read(0x04, 4, cvr));
    CHECK_EQ(cvr & hw::di::DICVR_CVR, hw::di::DICVR_CVR);
    CHECK(empty.mmio_write(0x08, 4, 0xA8000000U));
    CHECK(empty.mmio_write(0x18, 4, 0x20));
    CHECK(empty.mmio_write(0x1C, 4, 3));
    t.sched.advance_to(t.sched.now() + 1);
    u64 sr = 0;
    CHECK(empty.mmio_read(0x00, 4, sr));
    CHECK_EQ(sr & hw::di::DISR_DEINT, hw::di::DISR_DEINT);
    CHECK(empty.drive_error() == hw::di::DriveError::MediumNotPresent);
}

TEST_CASE(di_stops_when_the_image_cannot_deliver_data) {
    struct Broken final : disc::DiscReader {
        std::vector<u8> d;
        u64 size() const override { return d.size(); }
        bool read(u64 off, std::span<u8> out) override {
            if (off + out.size() > d.size() || (off <= 0x100000U && off + out.size() > 0x100000U)) return false;
            std::copy_n(d.begin() + static_cast<std::ptrdiff_t>(off), out.size(), out.begin());
            return true;
        }
        std::string failure_reason() const override { return "block 64 is corrupt"; }
    };
    DiRig t;
    auto b = std::make_unique<Broken>();
    std::vector<u8> app(64, 0), dol(0x100, 0);
    b->d = build_disc(app, dol);
    t.di.insert(std::move(b));
    t.di.after_ipl_boot(false, 0);
    t.read_cmd(0x2000, 0x40, 0x0FFFE0); // a range the image cannot deliver
    t.w(0x1C, 3);
    t.sched.advance_to(t.sched.now() + 1);
    CHECK(t.di.image_failed());
    CHECK(t.di.image_failure().find("block 64 is corrupt") != std::string::npos);
    CHECK(t.di.image_failure().find("0xFFFE0") != std::string::npos);
    // past the end of the disc is the game's business (DEINT), not an image failure
    DiRig u;
    u.read_cmd(0x2000, 0x20, 0x1FFFF0);
    u.w(0x1C, 3);
    u.sched.advance_to(u.sched.now() + 1);
    CHECK(!u.di.image_failed());
    CHECK((u.r(0x00) & hw::di::DISR_DEINT) != 0U);
}

TEST_CASE(di_drive_states_and_immediate_commands) {
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::DiscInterface di{bus, sched, pi};
    bus.map_ram("ram", 0, 0x100000);
    std::vector<u8> app(64, 0), dol(0x100, 0);
    auto img = build_disc(app, dol);
    img[8] = 1; // the header asks for streaming audio
    di.insert(std::make_unique<disc::MemoryDisc>(img));
    auto w = [&](u32 off, u32 v) { CHECK(di.mmio_write(off, 4, v)); };
    auto r = [&](u32 off) { u64 v = 0; CHECK(di.mmio_read(off, 4, v)); return static_cast<u32>(v); };
    auto run = [&](u32 c0, u32 c1, u32 c2, u32 dicr) {
        w(0x00, hw::di::DISR_TCINT | hw::di::DISR_DEINT);
        w(0x08, c0); w(0x0C, c1); w(0x10, c2); w(0x1C, dicr);
        sched.advance_to(sched.now() + 1);
        return (r(0x00) & hw::di::DISR_TCINT) != 0U;
    };
    auto request_error = [&] { CHECK(run(0xE0000000U, 0, 0, 1)); return r(0x20); };
    CHECK(di.drive_state() == hw::di::DriveState::DiscIdNotRead);
    // data before the disc ID: "disc ID not read" (state 6 is reported as 5)
    w(0x14, 0x2000); w(0x18, 0x20);
    CHECK(!run(0xA8000000U, 0, 0x20, 3));
    CHECK_EQ(request_error(), 0x05020401U);
    CHECK_EQ(request_error(), 0x05000000U);   // the error was cleared by the first request
    // inquiry: DMA of the drive's identification
    w(0x14, 0x3000); w(0x18, 0x20);
    CHECK(run(0x12000000U, 0, 0x20, 3));
    u32 word = 0;
    bus.read32(0x3000, word); CHECK_EQ(word, 0x00000002U);
    bus.read32(0x3004, word); CHECK_EQ(word, 0x20060526U);
    CHECK_EQ(r(0x14), 0x3020U);
    // the disc ID, then configuration of streaming audio (allowed before the first data read)
    w(0x14, 0x4000); w(0x18, 0x20);
    CHECK(run(0xA8000040U, 0, 0x20, 3));
    bus.read32(0x4000, word); CHECK_EQ(word, 0x4F524341U); // "ORCA"
    // diagnostics: the last commands with their outcome
    CHECK(di.recent_commands().find("[A8000000 00000000 00000020]error") != std::string::npos);
    CHECK(di.recent_commands().find("[A8000040 00000000 00000020]ok") != std::string::npos);
    CHECK(di.drive_state() == hw::di::DriveState::ReadyNoReadsMade);
    CHECK(run(0xE4010000U | 10U, 0, 0, 1));
    CHECK(di.streaming_enabled());
    w(0x14, 0x5000); w(0x18, 0x20);
    CHECK(run(0xA8000000U, 0x100000U >> 2, 0x20, 3));
    CHECK(di.drive_state() == hw::di::DriveState::Ready);
    CHECK(!run(0xE4000000U, 0, 0, 1));         // too late now
    CHECK_EQ(request_error(), 0x00052402U);
    // seek; past the end of the disc: "block out of bounds"
    CHECK(run(0xAB000000U, 0x1000, 0, 1));
    w(0x14, 0x5000); w(0x18, 0x20);
    CHECK(!run(0xA8000000U, 0x00200000U >> 2, 0x20, 3));
    CHECK_EQ(request_error(), 0x00052100U);
    // stopping the motor: reads fail until a new disc
    CHECK(run(0xE3000000U, 0, 0, 1));
    w(0x14, 0x5000); w(0x18, 0x20);
    CHECK(!run(0xA8000000U, 0, 0x20, 3));
    CHECK_EQ(request_error(), 0x04020400U);
}

TEST_CASE(dtk_streaming_audio) {
    // decoder: filter 0 with shift 12 gives the nibble itself (<< 12 >> 12)
    u8 block[32] = {0x0C, 0x0C, 0, 0};
    for (unsigned i = 0; i < 28; ++i) block[4 + i] = static_cast<u8>((i & 7U) | ((7U - (i & 7U)) << 4));
    s16 pcm[56];
    s32 hist[4] = {};
    hw::decode_dtk_block(block, pcm, hist);
    for (unsigned i = 0; i < 28; ++i) {
        CHECK_EQ(pcm[2 * i], static_cast<s16>(i & 7U));
        CHECK_EQ(pcm[2 * i + 1], static_cast<s16>(7U - (i & 7U)));
    }
    // filter 1 (h1 * 60/64) with shift 0: a nibble of 1 -> 4096, then the history decays
    u8 f1[32] = {0x10, 0x10, 0, 0};
    f1[4] = 0x11;
    s32 h[4] = {};
    hw::decode_dtk_block(f1, pcm, h);
    CHECK_EQ(pcm[0], 4096);
    CHECK_EQ(pcm[2], 3840);                   // 4096 * 60 / 64
    CHECK_EQ(pcm[4], 3600);                   // 3840 * 60 / 64
    // the drive plays a track, queues the next and stops at the end on request
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::DiscInterface di{bus, sched, pi};
    bus.map_ram("ram", 0, 0x100000);
    std::vector<u8> app(64, 0), dol(0x100, 0);
    auto img = build_disc(app, dol);
    for (unsigned b = 0; b < 4; ++b) { // two 64-byte tracks at 0x180000 and 0x180040
        u8* p = &img[0x180000U + 32U * b];
        std::fill(p, p + 32, 0);
        p[0] = p[1] = 0x0C;
        for (unsigned i = 0; i < 28; ++i) p[4 + i] = static_cast<u8>(b + 1U);
    }
    di.insert(std::make_unique<disc::MemoryDisc>(img));
    di.after_ipl_boot(true, 0);
    auto cmd = [&](u32 c0, u32 c1, u32 c2) { di.mmio_write(0x08, 4, c0); di.mmio_write(0x0C, 4, c1); di.mmio_write(0x10, 4, c2);
                                             CHECK(di.mmio_write(0x1C, 4, 1)); sched.advance_to(sched.now() + 1); };
    cmd(0xE1000000U, 0x180000U >> 2, 0x40);
    CHECK(di.stream_playing());
    cmd(0xE1000000U, 0x180040U >> 2, 0x40);   // queued after the first track
    std::vector<s16> out(2 * 28 * 8);
    CHECK_EQ(di.dtk_pull(out, 28), 28U);
    CHECK_EQ(out[0], 1);
    cmd(0xE2010000U, 0, 0);                   // current position (rounded down to 32 KiB) >> 2
    u64 imm = 0; di.mmio_read(0x20, 4, imm);
    CHECK_EQ(imm, u64{0x180000U >> 2});
    CHECK_EQ(di.dtk_pull(out, 28 * 2), 28U * 2U);
    CHECK_EQ(out[0], 2);
    CHECK_EQ(out[2 * 28], 3);                 // the queued track follows without a gap
    cmd(0xE1000000U, 0, 0);                   // stop at the end of the current track
    CHECK_EQ(di.dtk_pull(out, 28 * 4), 28U);  // its last block, then the drive stops
    CHECK_EQ(out[0], 4);
    CHECK(!di.stream_playing());
    cmd(0xE2000000U, 0, 0);
    di.mmio_read(0x20, 4, imm);
    CHECK_EQ(imm, 0U);
    // without streaming configured: "no audio buffer"
    hw::DiscInterface off{bus, sched, pi};
    off.insert(std::make_unique<disc::MemoryDisc>(img));
    off.after_ipl_boot(false, 0);
    off.mmio_write(0x08, 4, 0xE1000000U);
    CHECK(off.mmio_write(0x1C, 4, 1));
    sched.advance_to(sched.now() + 1);
    CHECK(off.drive_error() == hw::di::DriveError::NoAudioBuffer);
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
struct BootRun { bool booted = false; std::string error; ppc::StepResult stop{}; std::vector<u32> log; u32 exit_code = 99; u32 entry = 0; disc::ImageFormat format{};
                 u32 video_mode = 99, aram_size = 0; u64 watch_writes = 99; mem::Bus::WatchHit hit{}; };

enum class Wrap { Raw, Ciso, Gcz };

BootRun boot_and_run(const char* dol_name, Wrap wrap = Wrap::Raw, u8 bi2_region = 0, bool run = true) {
    BootRun r;
    const auto app = read_file(ORCA_PROGRAMS_DIR "/apploader.bin");
    const auto dol = read_file((std::string(ORCA_PROGRAMS_DIR) + "/" + dol_name).c_str());
    CHECK(!app.empty() && !dol.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    std::vector<u8> image = build_disc(app, dol);
    image[0x440 + 0x1B] = bi2_region; // bi2: region (0 JPN, 1 USA, 2 PAL)
    if (wrap == Wrap::Ciso) image = test_images::make_ciso(image, 0x8000);
#if defined(ORCA_HAVE_ZLIB)
    if (wrap == Wrap::Gcz) image = test_images::make_gcz(image, 0x4000);
#endif
    disc::ImageFormat format = disc::ImageFormat::Raw;
    auto d = disc::open_image(std::make_unique<disc::MemoryDisc>(std::move(image)), &format, r.error);
    if (!d) { std::fprintf(stderr, "    open_image: %s\n", r.error.c_str()); return r; }
    r.format = format;
    sys.insert_disc(std::move(d));
    r.booted = sys.boot_disc(r.error);
    if (!r.booted) { std::fprintf(stderr, "    boot_disc: %s\n", r.error.c_str()); return r; }
    r.entry = sys.cpu().state().pc;
    (void)sys.bus().read32(0xCC, r.video_mode);
    (void)sys.bus().read32(0xD0, r.aram_size);
    r.watch_writes = sys.bus().watch_hit().count;
    if (!run) {
        // a later write to the video mode word is recorded with its source
        sys.cpu().state().pc = 0x80001234U;
        (void)sys.bus().write32(0xCC, 7U);
        r.hit = sys.bus().watch_hit();
        return r;
    }
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

// What the IPL leaves for VIConfigure and ARInit (Dolphin Boot_BS2Emu): the video
// mode word follows the disc region (PAL discs: 1, others: 0) and 0xD0 holds the
// ARAM size. Later writes to the video mode word are recorded for the reports.
TEST_CASE(gamecube_boot_leaves_the_video_mode_and_aram_size) {
    const BootRun ntsc = boot_and_run("integer_suite.dol", Wrap::Raw, 1, false);
    CHECK(ntsc.booted);
    CHECK_EQ(ntsc.video_mode, 0U);
    CHECK_EQ(ntsc.aram_size, 0x01000000U);
    CHECK_EQ(ntsc.watch_writes, 0U); // the test apploader does not touch it
    const BootRun pal = boot_and_run("integer_suite.dol", Wrap::Raw, 2, false);
    CHECK(pal.booted);
    CHECK_EQ(pal.video_mode, 1U);
    CHECK_EQ(pal.hit.count, 1U);
    CHECK(pal.hit.source == mem::AccessSource::Cpu);
    CHECK_EQ(pal.hit.pc, 0x80001234U);
    CHECK_EQ(pal.hit.value, 7U);
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

TEST_CASE(boot_from_compressed_images) {
    // the same programs from a CISO and a GCZ image: same results as the raw image
    const BootRun c = boot_and_run("integer_suite.dol", Wrap::Ciso);
    CHECK(c.booted && c.format == disc::ImageFormat::Ciso);
    CHECK(c.stop == StepResult::Halted);
    CHECK_EQ(c.exit_code, 0U);
#if defined(ORCA_HAVE_ZLIB)
    const BootRun raw = boot_and_run("dvd_suite.dol");
    const BootRun g = boot_and_run("dvd_suite.dol", Wrap::Gcz); // reads the disc through the DI while it runs
    CHECK(g.booted && g.format == disc::ImageFormat::Gcz);
    CHECK(g.stop == StepResult::Halted);
    CHECK_EQ(g.exit_code, 0U);
    CHECK(!g.log.empty() && g.log == raw.log);
    std::printf("[disc] dvd_suite from a GCZ image: %zu values %s\n", g.log.size(), g.log == raw.log ? "identical to the raw image" : "MISMATCH");
#else
    CHECK(!disc::gcz_supported());
#endif
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
