// External Interface (EXI) and the RTC/SRAM/IPL-ROM device on EXI0/CS1.
#include "orca/hw/exi_ipl.hpp"
#include "orca/hw/exi_memcard.hpp"
#include "orca/hw/external_interface.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

using namespace orca;
using namespace orca::hw;

namespace {
constexpr u64 kCpuHz = 486000000ULL;
constexpr u32 kSelIpl8 = 0x130U; // CS1 | CLK 3 (8 MHz)

struct Rig {
    mem::Bus bus;
    core::Scheduler sched;
    ProcessorInterface pi{bus, nullptr};
    ExternalInterface exi{bus, sched, pi, kCpuHz};
    IplRtcSram ipl{sched, kCpuHz};
    Rig() {
        CHECK(bus.map_ram("MEM1", 0, 0x01800000U));
        CHECK(exi.attach(0, 1, ipl));
    }
    bool w(u32 o, u32 v) { return exi.mmio_write(o, 4, v); }
    u32 r(u32 o) { u64 v = 0; CHECK(exi.mmio_read(o, 4, v)); return static_cast<u32>(v); }
    // Runs the scheduler until channel `ch` finishes; returns elapsed cycles.
    u64 finish(u32 ch = 0) {
        const u64 t0 = sched.now();
        while (exi.busy(ch)) {
            const auto n = sched.next_event_time();
            CHECK(n.has_value());
            if (!n) break;
            sched.advance_to(*n);
        }
        return sched.now() - t0;
    }
    void imm_write(u32 word, u32 ch = 0) {
        CHECK(w(ch * 0x14U + 0x10U, word));
        CHECK(w(ch * 0x14U + 0x0CU, 0x35U));
        finish(ch);
    }
    u32 imm_read(u32 tlen = 3, u32 ch = 0) {
        CHECK(w(ch * 0x14U + 0x0CU, (tlen << 4) | 0x01U));
        finish(ch);
        return r(ch * 0x14U + 0x10U);
    }
    bool pi_exi() { u64 v = 0; CHECK(pi.mmio_read(0x00, 4, v)); return (v & pi::CAUSE_EXI) != 0U; }
};

struct NullDevice final : ExiDevice {
    bool ext = false;
    std::vector<u8> written;
    u8 next = 0x40;
    const char* exi_name() const noexcept override { return "null"; }
    bool external() const noexcept override { return ext; }
    bool write(std::span<const u8> b, std::string&) override { written.insert(written.end(), b.begin(), b.end()); return true; }
    bool read(std::span<u8> b, std::string&) override { for (u8& x : b) x = next++; return true; }
};
} // namespace

// ------------------------------------------------------------ registers
TEST_CASE(exi_reset_state_and_register_layout) {
    Rig t;
    for (u32 ch = 0; ch < 3; ++ch)
        for (u32 reg = 0; reg < 0x14; reg += 4) CHECK_EQ(t.r(ch * 0x14U + reg), 0U);
    u64 v = 0;
    CHECK(!t.exi.mmio_read(0x3C, 4, v)); // past channel 2
    CHECK(!t.exi.mmio_read(0x00, 2, v)); // 16-bit access
    CHECK(!t.exi.mmio_write(0x00, 1, 0));
    CHECK(t.exi.mmio_error().find("32-bit") != std::string::npos);
    // MAR / LENGTH: bits 0-4 read as zero
    CHECK(t.w(0x18, 0x0123457FU));
    CHECK_EQ(t.r(0x18), 0x01234560U);
    CHECK(t.w(0x30, 0xFFFFFFFFU));
    CHECK_EQ(t.r(0x30), 0xFFFFFFE0U);
    // DATA is plain storage
    CHECK(t.w(0x24, 0xCAFEF00DU));
    CHECK_EQ(t.r(0x24), 0xCAFEF00DU);
}

TEST_CASE(exi_csr_masks_w1c_and_readonly_bits) {
    Rig t;
    CHECK(t.w(0x00, 0x3FFFU & ~(7U << 7) & ~(7U << 4))); // everything except CS/CLK
    // EXT (12) read-only; status bits are w1c (were 0); ROMDIS set on channel 0
    CHECK_EQ(t.r(0x00), exi::CSR_ROMDIS | exi::CSR_EXTINTMASK | exi::CSR_TCINTMASK | exi::CSR_EXIINTMASK);
    CHECK(t.w(0x00, 0));
    CHECK_EQ(t.r(0x00), exi::CSR_ROMDIS); // ROMDIS sticky
    CHECK(t.ipl.descrambler_disabled());
    // ROMDIS does not exist on channels 1/2
    CHECK(t.w(0x14, exi::CSR_ROMDIS));
    CHECK_EQ(t.r(0x14), 0U);
    // CS and CLK are read/write
    CHECK(t.w(0x14, (2U << 7) | (5U << 4)));
    CHECK_EQ(t.r(0x14), (2U << 7) | (5U << 4));
}

TEST_CASE(exi_csr_rejects_two_cs_bits_and_reserved_clocks) {
    Rig t;
    CHECK(!t.w(0x00, 3U << 7));
    CHECK(t.exi.mmio_error().find("more than one CS") != std::string::npos);
    CHECK(!t.w(0x00, 6U << 4));
    CHECK(t.exi.mmio_error().find("reserved") != std::string::npos);
    CHECK(!t.w(0x00, 7U << 4));
    CHECK_EQ(t.r(0x00), 0U); // rejected writes change nothing
    CHECK(t.w(0x00, 5U << 4)); // 32 MHz is valid
}

TEST_CASE(exi_ext_bit_reflects_external_device_on_cs0) {
    Rig t;
    NullDevice card;
    CHECK(t.exi.attach(1, 0, card));
    CHECK(!t.exi.attach(1, 0, card)); // occupied
    CHECK(!t.exi.attach(3, 0, card));
    CHECK(!t.exi.attach(0, 3, card));
    CHECK_EQ(t.r(0x14) & exi::CSR_EXT, 0U);
    card.ext = true;
    CHECK_EQ(t.r(0x14) & exi::CSR_EXT, exi::CSR_EXT);
    CHECK_EQ(t.r(0x00) & exi::CSR_EXT, 0U); // IPL chip is internal
    CHECK(t.w(0x14, exi::CSR_EXT));           // writing EXT has no effect
    CHECK_EQ(t.r(0x14), exi::CSR_EXT);
}

// ------------------------------------------------------------ interrupts
TEST_CASE(exi_interrupt_sources_masks_and_pi_cause) {
    Rig t;
    t.exi.notify_slot_change(1);
    CHECK_EQ(t.r(0x14) & exi::CSR_EXTINT, exi::CSR_EXTINT);
    CHECK(!t.pi_exi()); // masked
    CHECK(t.w(0x14, exi::CSR_EXTINTMASK));
    CHECK(t.pi_exi());
    CHECK(t.w(0x14, exi::CSR_EXTINTMASK | exi::CSR_EXTINT)); // w1c
    CHECK(!t.pi_exi());
    CHECK_EQ(t.r(0x14) & exi::CSR_EXTINT, 0U);
    t.exi.set_device_interrupt(2);
    CHECK(!t.pi_exi());
    CHECK(t.w(0x28, exi::CSR_EXIINTMASK));
    CHECK(t.pi_exi());
    CHECK(t.w(0x28, exi::CSR_EXIINT)); // clear status and mask
    CHECK(!t.pi_exi());
    CHECK_EQ(t.r(0x28), 0U);
    // Writing a status bit as 1 never sets it
    CHECK(t.w(0x28, exi::CSR_TCINT | exi::CSR_EXIINT | exi::CSR_EXTINT));
    CHECK_EQ(t.r(0x28), 0U);
}

TEST_CASE(exi_transfer_complete_interrupt) {
    Rig t;
    CHECK(t.w(0x00, kSelIpl8 | exi::CSR_TCINTMASK));
    CHECK(t.w(0x10, 0x20000000U));
    CHECK(t.w(0x0C, 0x35U));
    CHECK(t.exi.busy(0));
    CHECK_EQ(t.r(0x0C), 0x35U);       // TSTART reads 1 while running
    CHECK_EQ(t.r(0x00) & exi::CSR_TCINT, 0U);
    CHECK(!t.pi_exi());
    t.finish();
    CHECK_EQ(t.r(0x0C), 0x34U);       // TSTART cleared by hardware
    CHECK_EQ(t.r(0x00) & exi::CSR_TCINT, exi::CSR_TCINT);
    CHECK(t.pi_exi());
    CHECK(t.w(0x00, kSelIpl8 | exi::CSR_TCINT)); // clear TCINT, mask off
    CHECK(!t.pi_exi());
    // TCINT still latches when masked, but does not reach the PI
    t.imm_read();
    CHECK_EQ(t.r(0x00) & exi::CSR_TCINT, exi::CSR_TCINT);
    CHECK(!t.pi_exi());
}

// ------------------------------------------------------------ timing
TEST_CASE(exi_transfer_time_is_bits_over_clock) {
    // 32 bits at 8 MHz on a 486 MHz CPU = 1944 cycles exactly.
    Rig t;
    CHECK(t.w(0x00, kSelIpl8));
    CHECK(t.w(0x10, 0x20000000U));
    CHECK(t.w(0x0C, 0x35U));
    CHECK_EQ(t.finish(), 1944U);
    // 1 byte at 1 MHz: 8 * 486 = 3888 cycles
    CHECK(t.w(0x00, 0x100U));
    CHECK(t.w(0x00, 0x100U)); // re-writing the same CS keeps the selection
    CHECK(t.w(0x0C, 0x01U));  // 1-byte read of the RTC
    CHECK_EQ(t.finish(), 3888U);
    // 64-byte DMA at 32 MHz: 512 bits * 486e6 / 32e6 = 7776 cycles
    CHECK(t.w(0x00, 0)); // deselect: new command
    CHECK(t.w(0x00, 0x150U));
    t.imm_write(0x20000100U);
    CHECK(t.w(0x04, 0x1000U));
    CHECK(t.w(0x08, 64U));
    CHECK(t.w(0x0C, 0x03U));
    CHECK_EQ(t.finish(), 7776U);
    // A clock that does not divide evenly rounds up: 8 bits @ 32 MHz on 486 MHz = 121.5 -> 122
    CHECK(t.w(0x00, 0));
    CHECK(t.w(0x00, 0x150U));
    t.imm_write(0x20000000U);
    CHECK(t.w(0x0C, 0x01U));
    CHECK_EQ(t.finish(), 122U);
}

TEST_CASE(exi_results_appear_only_at_completion) {
    Rig t;
    t.ipl.set_rtc(0x11223344U);
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x20000000U);
    CHECK(t.w(0x10, 0xAAAAAAAAU));
    CHECK(t.w(0x0C, 0x31U));
    CHECK_EQ(t.r(0x10), 0xAAAAAAAAU); // not yet
    t.finish();
    CHECK_EQ(t.r(0x10), 0x11223344U);
    // DMA read: RAM is untouched until completion
    CHECK(t.w(0x00, 0));
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x20000100U);
    std::array<u8, 64> s{};
    for (unsigned i = 0; i < 64; ++i) s[i] = static_cast<u8>(0xC0U + i);
    t.ipl.set_sram(s);
    CHECK(t.bus.write32(0x2000, 0x55555555U) == mem::BusStatus::Ok);
    CHECK(t.w(0x04, 0x2000U));
    CHECK(t.w(0x08, 64U));
    CHECK(t.w(0x0C, 0x03U));
    u32 v = 0;
    CHECK(t.bus.read32(0x2000, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, 0x55555555U);
    t.finish();
    CHECK(t.bus.read32(0x2000, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, 0xC0C1C2C3U);
    CHECK(t.bus.read32(0x203C, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, 0xFCFDFEFFU);
    CHECK_EQ(t.r(0x04), 0x2000U); // MAR / LENGTH unchanged (model)
    CHECK_EQ(t.r(0x08), 64U);
}

// ------------------------------------------------------------ errors
TEST_CASE(exi_transfer_errors_are_reported_not_invented) {
    Rig t;
    // no CS
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("no device selected") != std::string::npos);
    CHECK_EQ(t.r(0x0C), 0x34U); // TSTART not left set
    CHECK(!t.exi.busy(0));
    // CS with nothing attached (channel 0 CS0 = empty memory card slot): an empty
    // port, as on hardware (and in Dolphin): the transfer completes, a read gives 0
    CHECK(t.w(0x00, 0x080U));
    CHECK(t.w(0x10, 0xFFFFFFFFU));
    CHECK(t.w(0x0C, 0x35U));        // immediate write, 4 bytes: goes nowhere
    CHECK(t.exi.busy(0));
    CHECK(t.finish() > 0U);
    CHECK(t.w(0x10, 0xFFFFFFFFU));
    CHECK(t.w(0x0C, 0x31U));        // immediate read, 4 bytes
    (void)t.finish();
    CHECK_EQ(t.r(0x10), 0U);        // nothing drives the line
    CHECK_EQ(t.r(0x00) & exi::CSR_EXT, 0U); // EXT: no external device
    // DMA with a CPU virtual address (cached 0x80..., uncached 0xC0...): the DMA
    // engine ignores bits 30-31, so it reaches RAM (Resident Evil 0 does this)
    for (u32 i = 0; i < 0x40; i += 4) CHECK(t.bus.write32(0x4000 + i, 0xDEADBEEFU) == mem::BusStatus::Ok);
    CHECK(t.w(0x04, 0x80004000U));
    CHECK_EQ(t.r(0x04), 0x80004000U);  // MAR reads back as written
    CHECK(t.w(0x08, 0x40U));
    CHECK(t.w(0x0C, 0x07U));           // DMA write to the empty port
    CHECK(t.finish() > 0U);
    CHECK(t.w(0x04, 0xC0004000U));
    CHECK(t.w(0x0C, 0x03U));           // DMA read from the empty port: zeros into RAM
    (void)t.finish();
    u32 got = 0xFFFFFFFFU;
    CHECK(t.bus.read32(0x4000, got) == mem::BusStatus::Ok);
    CHECK_EQ(got, 0U);
    CHECK(t.bus.read32(0x403C, got) == mem::BusStatus::Ok);
    CHECK_EQ(got, 0U);
    CHECK(t.w(0x04, 0x81800000U));     // past MEM1 (24 MiB here): still refused
    CHECK(!t.w(0x0C, 0x03U));
    CHECK(t.exi.mmio_error().find("not RAM") != std::string::npos);
    CHECK(t.w(0x00, 0x000U));
    // read+write and undefined RW
    CHECK(t.w(0x00, kSelIpl8));
    CHECK(!t.w(0x0C, 0x39U));
    CHECK(t.exi.mmio_error().find("read+write") != std::string::npos);
    CHECK(!t.w(0x0C, 0x0BU));
    CHECK(t.exi.mmio_error().find("invalid") != std::string::npos);
    CHECK(!t.w(0x0C, 0x0DU));
    CHECK(t.exi.mmio_error().find("undefined") != std::string::npos);
    // DMA: zero length / outside RAM
    CHECK(t.w(0x08, 0));
    CHECK(!t.w(0x0C, 0x07U));
    CHECK(t.exi.mmio_error().find("LENGTH = 0") != std::string::npos);
    CHECK(t.w(0x04, 0x017FFFE0U));
    CHECK(t.w(0x08, 0x40U));
    CHECK(!t.w(0x0C, 0x07U));
    CHECK(t.exi.mmio_error().find("not RAM") != std::string::npos);
    // CR / CS changes during a transfer
    t.ipl.set_rtc(0);
    CHECK(t.w(0x10, 0x20000000U));
    CHECK(t.w(0x0C, 0x35U));
    CHECK(!t.w(0x0C, 0x31U));
    CHECK(t.exi.mmio_error().find("in progress") != std::string::npos);
    CHECK(!t.w(0x00, 0x100U | 0x080U));
    CHECK(!t.w(0x00, 0x030U)); // deselect while busy
    CHECK(t.exi.mmio_error().find("during a transfer") != std::string::npos);
    CHECK(t.w(0x00, kSelIpl8 | exi::CSR_TCINTMASK)); // same CS: masks may change
    t.finish();
}

// ------------------------------------------------------------ RTC / SRAM / ROM device
TEST_CASE(ipl_rtc_counts_emulated_seconds_and_accepts_writes) {
    Rig t;
    t.ipl.set_rtc(1000);
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(exi_ipl::command(exi_ipl::kRtcAddress, false));
    CHECK_EQ(exi_ipl::command(exi_ipl::kRtcAddress, false), 0x20000000U);
    CHECK_EQ(t.imm_read(), 1000U);
    t.sched.advance_to(t.sched.now() + 3ULL * kCpuHz);
    CHECK_EQ(t.ipl.rtc(), 1003U);
    CHECK(t.w(0x00, 0));
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0xA0000000U);
    t.imm_write(0xFFFFFFFFU);
    CHECK_EQ(t.ipl.rtc(), 0xFFFFFFFFU);
    t.sched.advance_to(t.sched.now() + kCpuHz);
    CHECK_EQ(t.ipl.rtc(), 0U); // 32-bit counter wraps
    // byte-wise streaming of a read: 1 + 3 bytes
    CHECK(t.w(0x00, 0));
    t.ipl.set_rtc(0x0A0B0C0DU);
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x20000000U);
    CHECK(t.w(0x10, 0x00000077U));
    CHECK_EQ(t.imm_read(0), 0x0A000077U); // only the top byte replaced (model)
    CHECK_EQ(t.imm_read(2) >> 8, 0x0B0C0DU);
    // a fifth byte runs past the counter
    CHECK(!t.w(0x0C, 0x01U));
    CHECK(t.exi.mmio_error().find("RTC") != std::string::npos);
}

TEST_CASE(ipl_sram_immediate_and_dma_access) {
    Rig t;
    // Linux rtc-gamecube: bias register 0x200004 << 8 = SRAM byte 0x0C
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0xA0000100U + (0x0CU << 6));
    t.imm_write(0x12345678U);
    CHECK(t.w(0x00, 0));
    CHECK_EQ(t.ipl.sram()[0x0C], 0x12U);
    CHECK_EQ(t.ipl.sram()[0x0F], 0x78U);
    CHECK_EQ(0x20000100U + (0x0CU << 6), 0x20000400U);
    // DMA write of the whole SRAM
    for (u32 i = 0; i < 64; i += 4) CHECK(t.bus.write32(0x3000 + i, 0x01020304U * (i / 4U + 1U)) == mem::BusStatus::Ok);
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0xA0000100U);
    CHECK(t.w(0x04, 0x3000U));
    CHECK(t.w(0x08, 64U));
    CHECK(t.w(0x0C, 0x07U)); // DMA write
    t.finish();
    CHECK(t.w(0x00, 0));
    CHECK_EQ(t.ipl.sram()[0], 0x01U);
    CHECK_EQ(t.ipl.sram()[63], static_cast<u8>(0x04U * 16U));
    // immediate read in the middle
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x20000100U + (4U << 6));
    CHECK_EQ(t.imm_read(), 0x02040608U);
    // DMA past the end of SRAM (offset 4 + 64)
    CHECK(t.w(0x04, 0x3000U));
    CHECK(t.w(0x08, 64U));
    CHECK(!t.w(0x0C, 0x03U));
    CHECK(t.exi.mmio_error().find("SRAM") != std::string::npos);
}

TEST_CASE(ipl_rom_reads_need_an_image_and_the_descrambler) {
    Rig t;
    CHECK(t.w(0x00, kSelIpl8));
    CHECK(t.w(0x10, 0x00000000U));
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("no ROM image") != std::string::npos);
    CHECK(t.w(0x00, 0));
    CHECK(!t.ipl.load_rom({}));
    CHECK(!t.ipl.load_rom(std::vector<u8>(exi_ipl::kRomSize + 1U)));
    std::vector<u8> rom(0x1000);
    for (std::size_t i = 0; i < rom.size(); ++i) rom[i] = static_cast<u8>(i * 7U);
    CHECK(t.ipl.load_rom(rom));
    // 21C3: "00000AAA AAAAAAAA AAAAAAAA AAxxxxxx" -> byte address << 6
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x100U << 6);
    CHECK_EQ(t.imm_read(), (static_cast<u32>(rom[0x100]) << 24) | (static_cast<u32>(rom[0x101]) << 16) |
                               (static_cast<u32>(rom[0x102]) << 8) | rom[0x103]);
    CHECK(t.w(0x00, 0));
    // DMA of the last 32 bytes, then one byte past the image
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0xFE0U << 6);
    CHECK(t.w(0x04, 0x4000U));
    CHECK(t.w(0x08, 32U));
    CHECK(t.w(0x0C, 0x03U));
    t.finish();
    u8 b = 0;
    CHECK(t.bus.read8(0x401F, b) == mem::BusStatus::Ok);
    CHECK_EQ(b, rom[0xFFF]);
    CHECK(!t.w(0x0C, 0x01U)); // one byte past the 4 KiB image
    CHECK(t.exi.mmio_error().find("IPL ROM image") != std::string::npos);
    CHECK(t.w(0x00, 0));
    // writing to ROM
    CHECK(t.w(0x00, kSelIpl8));
    CHECK(t.w(0x10, 0x80000000U));
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("read-only") != std::string::npos);
    CHECK(t.w(0x00, 0));
    // after ROMDIS the descrambled view is gone
    CHECK(t.w(0x00, exi::CSR_ROMDIS));
    CHECK(t.w(0x00, kSelIpl8 | exi::CSR_ROMDIS));
    CHECK(t.w(0x10, 0));
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("ROMDIS") != std::string::npos);
    // ...but RTC/SRAM still work
    CHECK(t.w(0x00, 0));
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x20000000U);
}

TEST_CASE(ipl_protocol_errors) {
    Rig t;
    CHECK(t.w(0x00, kSelIpl8));
    // read before a command word
    CHECK(!t.w(0x0C, 0x31U));
    CHECK(t.exi.mmio_error().find("before a complete") != std::string::npos);
    // a 2-byte write keeps the command incomplete; the next 2 bytes finish it
    CHECK(t.w(0x10, 0x20000000U));
    CHECK(t.w(0x0C, 0x15U));
    t.finish();
    CHECK(t.w(0x10, 0x00000000U));
    CHECK(t.w(0x0C, 0x15U));
    t.finish();
    CHECK(t.imm_read() == t.ipl.rtc());
    // writing data during a read sequence
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("during a read") != std::string::npos);
    CHECK(t.w(0x00, 0));
    // read during a write sequence
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0xA0000100U);
    CHECK(!t.w(0x0C, 0x31U));
    CHECK(t.exi.mmio_error().find("during a write") != std::string::npos);
    CHECK(t.w(0x00, 0));
    // unimplemented address (between ROM and RTC, and after SRAM)
    CHECK(t.w(0x00, kSelIpl8));
    CHECK(t.w(0x10, 0x10000000U));
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("not implemented") != std::string::npos);
    CHECK(t.w(0x00, 0));
    CHECK(t.w(0x00, kSelIpl8));
    CHECK(t.w(0x10, 0x20000000U | (0x44U << 6)));
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.w(0x00, 0));
    // deselect resets the sequence: a new command is accepted
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0x20000100U);
    CHECK(t.w(0x00, 0));
    CHECK(t.w(0x00, kSelIpl8));
    t.imm_write(0xA0000100U);
    t.imm_write(0x01020304U);
    CHECK_EQ(t.ipl.sram()[3], 0x04U);
}

TEST_CASE(exi_immediate_write_order_and_lengths) {
    Rig t;
    NullDevice dev;
    CHECK(t.exi.attach(2, 0, dev));
    CHECK(t.w(0x28, 0x080U)); // channel 2, CS0
    for (u32 tlen = 0; tlen < 4; ++tlen) {
        CHECK(t.w(0x38, 0xA1B2C3D4U));
        CHECK(t.w(0x34, (tlen << 4) | 0x05U));
        t.finish(2);
        CHECK_EQ(t.r(0x38), 0xA1B2C3D4U); // DATA unchanged by a write (model)
    }
    const std::vector<u8> expect{0xA1, 0xA1, 0xB2, 0xA1, 0xB2, 0xC3, 0xA1, 0xB2, 0xC3, 0xD4};
    CHECK(dev.written == expect);
    CHECK(t.w(0x38, 0));
    CHECK_EQ(t.imm_read(3, 2), 0x40414243U);
    CHECK_EQ(t.imm_read(1, 2), 0x44454243U);
}


// ------------------------------------------------------------ memory card (M012)
namespace {
struct CardRig : Rig {
    std::unique_ptr<MemoryCard> card;
    int irqs = 0;
    explicit CardRig(u32 mbit = 16) {
        std::vector<u8> img(static_cast<std::size_t>(mbit) * memcard::kBytesPerMbit);
        for (std::size_t i = 0; i < img.size(); ++i) img[i] = static_cast<u8>(i * 31U + (i >> 9));
        std::string err;
        card = MemoryCard::create(sched, kCpuHz, mbit, img, err);
        CHECK(card != nullptr);
        CHECK(exi.attach(0, 0, *card));
        card->set_interrupt_callback([this] { exi.set_device_interrupt(0); ++irqs; });
    }
    void sel() { CHECK(w(0x00, 0x0B0U)); } // CS0, 8 MHz
    void desel() { CHECK(w(0x00, 0x030U)); }
    // immediate write of n (1-4) bytes, MSB first
    void put(u32 word, u32 n) { CHECK(w(0x10, word)); CHECK(w(0x0C, ((n - 1U) << 4) | 0x05U)); finish(); }
    u32 get(u32 n) { CHECK(w(0x0C, ((n - 1U) << 4) | 0x01U)); finish(); return r(0x10); }
    u8 at(u32 a) const { return card->image()[a]; }
    void cmd_addr(u8 cmd, u32 a) { // command + AD1 AD2 AD3 BA
        put((static_cast<u32>(cmd) << 24) | ((a >> 17) & 0xFFU) << 16 | ((a >> 9) & 0xFFU) << 8 | ((a >> 7) & 3U), 4);
        put((a & 0x7FU) << 24, 1);
    }
};
} // namespace

TEST_CASE(memcard_id_status_and_ext) {
    CardRig t;
    CHECK_EQ(t.r(0x00) & exi::CSR_EXT, exi::CSR_EXT); // card present in slot A
    t.sel();
    t.put(0x00000000U, 2);           // EXI ID command + one byte (answers 0x80)
    CHECK_EQ(t.get(4), 0x00000010U); // 16 Mbit
    t.desel();
    t.sel();
    t.put(0x83000000U, 1);
    CHECK_EQ(t.get(1) >> 24, 0xC1U);  // BUSY | UNLOCKED | READY at power-on
    t.desel();
    CardRig small(4);
    small.sel();
    small.put(0x00000000U, 2);
    CHECK_EQ(small.get(4), 0x00000004U);
}

TEST_CASE(memcard_read_array_immediate_and_sector_wrap) {
    CardRig t;
    const u32 a = 0x12345; // AD1 0x00, AD2 0x91, AD3 2, BA 0x45
    t.sel();
    t.cmd_addr(0x52, a);
    t.put(0, 4);                       // positions 5-8: dummy bytes
    const u32 v = t.get(4);            // positions 9-12: data, address advancing
    CHECK_EQ(v, (static_cast<u32>(t.at(a)) << 24) | (static_cast<u32>(t.at(a + 1)) << 16) |
                    (static_cast<u32>(t.at(a + 2)) << 8) | t.at(a + 3));
    t.desel();
    // inside a 512-byte sector the address wraps
    t.sel();
    t.cmd_addr(0x52, 0x21FE);
    t.put(0, 4);
    const u32 w3 = t.get(3);
    CHECK_EQ(w3 >> 8, (static_cast<u32>(t.at(0x21FE)) << 16) | (static_cast<u32>(t.at(0x21FF)) << 8) | t.at(0x2000));
    t.desel();
}

TEST_CASE(memcard_dma_read_and_timing) {
    CardRig t;
    t.sel();
    t.cmd_addr(0x52, 0x4200);
    t.put(0, 4);
    CHECK(t.w(0x04, 0x8000U));
    CHECK(t.w(0x08, 512U));
    CHECK(t.w(0x0C, 0x03U)); // DMA read
    CHECK_EQ(t.finish(), 512U * (kCpuHz / 524288U)); // Dolphin's read rate: 474112 cycles
    t.desel();
    unsigned bad = 0;
    for (u32 i = 0; i < 512; ++i) {
        u8 b = 0;
        CHECK(t.bus.read8(0x8000 + i, b) == mem::BusStatus::Ok);
        bad += b != t.at(0x4200 + i) ? 1U : 0U;
    }
    CHECK_EQ(bad, 0U);
}

TEST_CASE(memcard_program_erase_status_and_interrupt) {
    CardRig t;
    t.sel(); t.put(0x81010000U, 2); t.desel();      // enable the card interrupt
    t.sel(); t.put(0x89000000U, 1); t.desel();      // clear status
    // page program, immediate data
    t.sel();
    t.cmd_addr(0xF2, 0x0400);
    t.put(0xA1B2C300U, 3);
    CHECK_EQ(t.at(0x400), static_cast<u8>(0x400 * 31U + 2U)); // not written before CS release
    t.desel();
    CHECK_EQ(t.at(0x400), 0xA1U);
    CHECK_EQ(t.at(0x402), 0xC3U);
    CHECK(t.card->dirty());
    CHECK_EQ(t.irqs, 0);
    t.sched.advance_to(t.sched.now() + memcard::kCommandDoneCycles);
    CHECK_EQ(t.irqs, 1);
    CHECK_EQ(t.r(0x00) & exi::CSR_EXIINT, exi::CSR_EXIINT);
    CHECK_EQ(t.card->status() & memcard::STATUS_BUSY, 0U);
    // page program by DMA (128 bytes)
    for (u32 i = 0; i < 128; ++i) CHECK(t.bus.write8(0x9000 + i, static_cast<u8>(0x55 ^ i)) == mem::BusStatus::Ok);
    t.sel();
    t.cmd_addr(0xF2, 0x0600);
    CHECK(t.w(0x04, 0x9000U));
    CHECK(t.w(0x08, 128U));
    CHECK(t.w(0x0C, 0x07U)); // DMA write
    CHECK_EQ(t.finish(), 128U * (kCpuHz / 98432U));
    t.desel();
    CHECK_EQ(t.at(0x600), 0x55U);
    CHECK_EQ(t.at(0x67F), static_cast<u8>(0x55 ^ 127));
    // sector erase: the 8 KiB block becomes 0xFF; BUSY until the command completes
    t.sel();
    t.put(0xF1000400U, 3); // AD1 0, AD2 4 -> address 0x800 inside block 0
    t.desel();
    CHECK_EQ(t.at(0x0000), 0xFFU);
    CHECK_EQ(t.at(0x1FFF), 0xFFU);
    CHECK_EQ(t.at(0x2000), static_cast<u8>(0x2000U * 31U + (0x2000U >> 9))); // next block untouched (0x10)
    CHECK_EQ(t.card->status() & (memcard::STATUS_BUSY | memcard::STATUS_READY), memcard::STATUS_BUSY);
    t.sched.advance_to(t.sched.now() + memcard::kCommandDoneCycles);
    CHECK_EQ(t.card->status() & (memcard::STATUS_BUSY | memcard::STATUS_READY), memcard::STATUS_READY);
}

TEST_CASE(memcard_id_byte_buffer_wrap_and_interrupt_switch) {
    CardRig t;
    t.sel();
    t.put(0x00000000U, 1);
    CHECK_EQ(t.get(1) >> 24, 0x80U); // first answer of the ID command
    t.desel();
    // interrupt switch off (power-on): a finished command does not interrupt
    t.sel(); t.cmd_addr(0xF2, 0x0800); t.put(0x77000000U, 1); t.desel();
    t.sched.advance_to(t.sched.now() + memcard::kCommandDoneCycles);
    CHECK_EQ(t.irqs, 0);
    CHECK_EQ(t.card->status() & memcard::STATUS_READY, memcard::STATUS_READY);
    // a program with no data bytes still completes (BUSY cleared, READY) and interrupts once enabled
    t.sel(); t.put(0x81010000U, 2); t.desel();
    t.sel(); t.cmd_addr(0xF2, 0x0A00); t.desel();
    CHECK_EQ(t.at(0x0A00), static_cast<u8>(0x0A00U * 31U + 5U)); // nothing written
    t.sched.advance_to(t.sched.now() + memcard::kCommandDoneCycles);
    CHECK_EQ(t.irqs, 1);
    // 128 immediate data bytes fill the whole page buffer
    t.sel();
    t.cmd_addr(0xF2, 0x0C00);
    for (u32 i = 0; i < 128; i += 4) t.put((i << 24) | ((i + 1U) << 16) | ((i + 2U) << 8) | (i + 3U), 4);
    t.desel();
    unsigned bad = 0;
    for (u32 i = 0; i < 128; ++i) bad += t.at(0x0C00 + i) != i ? 1U : 0U;
    CHECK_EQ(bad, 0U);
}

TEST_CASE(memcard_errors_are_reported) {
    CardRig t;
    t.sel();
    CHECK(t.w(0x10, 0x85000000U));
    CHECK(!t.w(0x0C, 0x05U)); // read ID 0x85: not implemented
    CHECK(t.exi.mmio_error().find("command 85 is not implemented") != std::string::npos);
    t.desel();
    t.sel();
    CHECK(!t.w(0x0C, 0x01U)); // read before a command
    CHECK(t.exi.mmio_error().find("before a command") != std::string::npos);
    t.desel();
    std::string err;
    core::Scheduler s2;
    CHECK(MemoryCard::create(s2, kCpuHz, 12, {}, err) == nullptr);
    CHECK(err.find("4, 8, 16") != std::string::npos);
    CHECK(MemoryCard::create(s2, kCpuHz, 4, std::vector<u8>(100), err) == nullptr);
    CHECK(err.find("needs") != std::string::npos);
    auto blank = MemoryCard::create(s2, kCpuHz, 4, {}, err);
    CHECK(blank != nullptr);
    if (blank) CHECK_EQ(blank->image()[12345], 0xFFU);
}

TEST_CASE(system_memory_card_slots) {
    system::System sys({});
    std::string err;
    CHECK(sys.insert_memory_card(0, 16, {}, err));
    CHECK(!sys.insert_memory_card(0, 16, {}, err));
    CHECK(!sys.insert_memory_card(2, 16, {}, err));
    CHECK(sys.memory_card(0) != nullptr);
    CHECK(sys.memory_card(1) == nullptr);
    u32 v = 0;
    CHECK(sys.bus().read32(0x0C006800U, v) == mem::BusStatus::Ok);
    CHECK_EQ(v & (exi::CSR_EXT | exi::CSR_EXTINT), exi::CSR_EXT | exi::CSR_EXTINT);
    CHECK(sys.bus().read32(0x0C006814U, v) == mem::BusStatus::Ok);
    CHECK_EQ(v & exi::CSR_EXT, 0U); // slot B empty
}

// ------------------------------------------------------------ system integration
TEST_CASE(system_maps_exi_at_0c006800_with_ipl_on_cs1) {
    system::System sys({});
    sys.ipl().set_rtc(42);
    CHECK(sys.bus().write32(0x0C006800U, kSelIpl8) == mem::BusStatus::Ok);
    CHECK(sys.bus().write32(0x0C006810U, 0x20000000U) == mem::BusStatus::Ok);
    CHECK(sys.bus().write32(0x0C00680CU, 0x35U) == mem::BusStatus::Ok);
    sys.scheduler().advance_to(sys.scheduler().now() + 1944U);
    CHECK(sys.bus().write32(0x0C00680CU, 0x31U) == mem::BusStatus::Ok);
    sys.scheduler().advance_to(sys.scheduler().now() + 1944U);
    u32 v = 0;
    CHECK(sys.bus().read32(0x0C006810U, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, 42U);
    // Other units on the same page still answer
    CHECK(sys.bus().read32(0x0C006C00U, v) == mem::BusStatus::Ok); // AI
    CHECK(sys.bus().read32(0x0C006000U, v) == mem::BusStatus::Ok); // DI
    CHECK(sys.bus().read32(0x0C00683CU, v) != mem::BusStatus::Ok); // end of the EXI window
}

#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(exi_suite_program_rtc_sram_by_immediate_and_dma) {
    std::ifstream in(ORCA_PROGRAMS_DIR "/exi_suite.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    sys.ipl().set_rtc(0x12345678U);
    std::array<u8, 64> s{};
    for (unsigned i = 0; i < 64; ++i) s[i] = static_cast<u8>(i);
    sys.ipl().set_sram(s);
    std::string err;
    CHECK(sys.load_executable(file, err));
    const auto r = sys.run(50000000ULL);
    if (r.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    exi_suite stopped: %s (%s) pc=%08X\n", ppc::to_string(r.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc);
    CHECK(r.stop == ppc::StepResult::Halted);
    CHECK_EQ(port.exit_code, 0U);
    const auto& L = port.log;
    for (u32 v : L) std::printf("    exi_suite value %08X (%u)\n", v, v);
    CHECK_EQ(L.size(), 11U);
    if (L.size() != 11U) return;
    CHECK_EQ(L[0], 0x12345678U);               // RTC (well under 1 s of emulated time)
    CHECK(L[1] >= 162U && L[1] <= 175U);       // 1944 CPU cycles = 162 TB ticks + polling
    CHECK_EQ(L[2], 2U);                        // CR after DMA: DMA bit kept, TSTART clear
    CHECK_EQ(L[3], 8U);                        // TCINT
    CHECK_EQ(L[4], 0U);                        // PI EXI cause dropped after w1c
    CHECK_EQ(L[5], 0xDEADBEEFU);               // SRAM bytes 0-3 written by the program
    CHECK_EQ(L[6], 0x04050607U);               // host-provided bytes 4-7
    CHECK_EQ(L[7], 0x3C3D3E3FU);               // bytes 60-63
    CHECK_EQ(L[8], 0x00000100U);               // RTC written then read back
    CHECK_EQ(L[9], 0U);                        // no card in slot A
    CHECK_EQ(L[10], 0U);                       // no card in slot B
    CHECK_EQ(sys.ipl().sram()[0], 0xDEU);
}
#endif

TEST_MAIN("exi")
