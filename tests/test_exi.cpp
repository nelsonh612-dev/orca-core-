// External Interface (EXI) and the RTC/SRAM/IPL-ROM device on EXI0/CS1.
#include "orca/hw/exi_ipl.hpp"
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
    // CS with nothing attached (channel 0 CS0 = empty memory card slot)
    CHECK(t.w(0x00, 0x080U));
    CHECK(!t.w(0x0C, 0x35U));
    CHECK(t.exi.mmio_error().find("no device is attached") != std::string::npos);
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
