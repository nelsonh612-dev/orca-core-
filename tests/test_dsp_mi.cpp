// Memory Interface, DSP interface block (mailboxes, CSR, ARAM DMA, AI DMA
// registers) and the disc FST parser.
#include "orca/disc/fst.hpp"
#include "orca/hw/dsp_interface.hpp"
#include "orca/hw/memory_interface.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

using namespace orca;
using namespace orca::hw;

namespace {
struct Rig {
    mem::Bus bus;
    core::Scheduler sched;
    ProcessorInterface pi{bus, nullptr};
    MemoryInterface mi{pi};
    DspInterface dsp{bus, sched, pi, true};
    Rig() { CHECK(bus.map_ram("MEM1", 0, 0x01800000U)); }
    bool cause(u32 c) { u64 v = 0; CHECK(pi.mmio_read(0x00, 4, v)); return (v & c) != 0U; }
    bool dw(u32 o, u16 v) { return dsp.mmio_write(o, 2, v); }
    bool dw32(u32 o, u32 v) { return dsp.mmio_write(o, 4, v); }
    u16 dr(u32 o) { u64 v = 0; CHECK(dsp.mmio_read(o, 2, v)); return static_cast<u16>(v); }
    void settle() { while (auto n = sched.next_event_time()) sched.advance_to(*n); }
};

void put32(std::vector<u8>& b, std::size_t o, u32 v) {
    b[o] = static_cast<u8>(v >> 24); b[o + 1] = static_cast<u8>(v >> 16); b[o + 2] = static_cast<u8>(v >> 8); b[o + 3] = static_cast<u8>(v);
}
} // namespace

// ------------------------------------------------------------ MI
TEST_CASE(mi_registers_and_interrupts) {
    Rig t;
    u64 v = 0;
    CHECK(t.mi.mmio_read(mi::TYPE, 2, v));
    CHECK_EQ(v, 0xFFU); // every channel read/write at reset (model)
    CHECK(t.mi.mmio_write(mi::REGION0, 4, 0x00100020U));
    CHECK(t.mi.mmio_read(mi::REGION0, 4, v));
    CHECK_EQ(v, 0x00100020U);
    CHECK(t.mi.warnings().empty()); // type is still 3 everywhere
    CHECK(t.mi.mmio_write(mi::IMASK, 2, 0xFFFF));
    CHECK(t.mi.mmio_read(mi::IMASK, 2, v));
    CHECK_EQ(v, 0x1FU);
    CHECK(!t.cause(pi::CAUSE_MEM)); // nothing pending
    CHECK(t.mi.mmio_write(mi::ICAUSE, 2, 0x1F)); // w1c on a clear register
    CHECK(t.mi.mmio_read(mi::ICAUSE, 2, v));
    CHECK_EQ(v, 0U);
    CHECK(t.mi.mmio_write(mi::ADDRLO, 2, 0x1234)); // read-only: ignored
    CHECK(t.mi.mmio_read(mi::ADDRLO, 2, v));
    CHECK_EQ(v, 0U);
    // timers: writes reset them; reads are not implemented
    CHECK(t.mi.mmio_write(0x32, 2, 0));
    CHECK(!t.mi.mmio_read(0x32, 2, v));
    CHECK(t.mi.mmio_error().find("timer") != std::string::npos);
    CHECK(!t.mi.mmio_read(0x60, 2, v));
    CHECK(!t.mi.mmio_read(0x00, 1, v));
}

TEST_CASE(mi_protection_is_reported_as_not_enforced) {
    Rig t;
    CHECK(t.mi.mmio_write(mi::REGION0 + 4U, 4, 0x00400050U)); // channel 1: pages 0x40-0x50
    CHECK(t.mi.mmio_write(mi::TYPE, 2, 0xF7));                // channel 1 = 1 (read only)
    CHECK_EQ(t.mi.warnings().size(), 1U);
    CHECK(t.mi.warnings()[0].find("channel 1 protection type 1 over pages 0x0040-0x0050") != std::string::npos);
    CHECK(t.mi.warnings()[0].find("NOT enforced") != std::string::npos);
    for (int i = 0; i < 100; ++i) CHECK(t.mi.mmio_write(mi::TYPE, 2, 0xF7));
    CHECK_EQ(t.mi.warnings().size(), 64U); // bounded
    u64 v = 0;
    CHECK(t.mi.mmio_read(mi::TYPE, 2, v));
    CHECK_EQ(v, 0xF7U);
}

// ------------------------------------------------------------ DSP CSR / mailboxes
TEST_CASE(dsp_csr_masks_w1c_and_pi_cause) {
    Rig t;
    CHECK_EQ(t.dr(hw::dsp::CSR), hw::dsp::CSR_HALT); // the DSP starts halted (model)
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_HALT | hw::dsp::CSR_AIDINTMASK | hw::dsp::CSR_ARINTMASK | hw::dsp::CSR_DSPINTMASK | hw::dsp::CSR_DMABUSY));
    CHECK_EQ(t.dr(hw::dsp::CSR), hw::dsp::CSR_HALT | hw::dsp::CSR_AIDINTMASK | hw::dsp::CSR_ARINTMASK | hw::dsp::CSR_DSPINTMASK); // busy is RO
    t.dsp.raise_dspint();
    CHECK(t.cause(pi::CAUSE_DSP));
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_HALT | hw::dsp::CSR_DSPINT)); // w1c, masks cleared
    CHECK(!t.cause(pi::CAUSE_DSP));
    CHECK_EQ(t.dr(hw::dsp::CSR), hw::dsp::CSR_HALT);
    t.dsp.raise_dspint();
    CHECK(!t.cause(pi::CAUSE_DSP)); // masked: status latched, no PI cause
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_DSPINT, hw::dsp::CSR_DSPINT);
}

TEST_CASE(dsp_mailboxes_connect_to_the_core) {
    Rig t;
    // CPU -> DSP: the pending bit (31) shows until the DSP reads CMBL
    CHECK(t.dw(hw::dsp::DSPMBH, 0x8012));
    CHECK(t.dw(hw::dsp::DSPMBL, 0x3456));
    CHECK_EQ(t.dr(hw::dsp::DSPMBH), 0x8012U);
    CHECK_EQ(t.dr(hw::dsp::DSPMBL), 0x3456U);
    CHECK_EQ(t.dsp.core().cpu_read_cmb_high(), 0x8012U);
    // DSP -> CPU: empty, then written by DSP code; reading the low half consumes it
    CHECK_EQ(t.dr(hw::dsp::CPUMBH), 0U);
    t.dsp.core().iram()[0] = 0x16FC; t.dsp.core().iram()[1] = 0x0071; // SI @DMBH
    t.dsp.core().iram()[2] = 0x16FD; t.dsp.core().iram()[3] = 0xFEED; // SI @DMBL
    t.dsp.core().iram()[4] = 0x0021;                                  // HALT
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_RES | hw::dsp::CSR_BIT11 | hw::dsp::CSR_HALT)); // boot copies main 0x01000000
    CHECK_EQ(t.dsp.core().iram()[0], 0U); // the boot DMA replaced IRAM with main memory (zeros here)
    t.dsp.core().iram()[0] = 0x16FC; t.dsp.core().iram()[1] = 0x0071;
    t.dsp.core().iram()[2] = 0x16FD; t.dsp.core().iram()[3] = 0xFEED;
    t.dsp.core().iram()[4] = 0x0021;
    CHECK(t.dw(hw::dsp::CSR, 0)); // release HALT
    t.settle();
    CHECK_EQ(t.dr(hw::dsp::CPUMBH), 0x8071U);
    CHECK_EQ(t.dr(hw::dsp::CPUMBL), 0xFEEDU);
    CHECK_EQ(t.dr(hw::dsp::CPUMBH), 0x0071U);
    CHECK(!t.dw(hw::dsp::CPUMBH, 0));
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_HALT, hw::dsp::CSR_HALT); // set by the HALT instruction
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_RES, 0U);
    // RES clears the mailboxes
    CHECK(t.dw(hw::dsp::DSPMBH, 0x8000));
    CHECK(t.dw(hw::dsp::DSPMBL, 0x0001));
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_RES | hw::dsp::CSR_HALT));
    CHECK_EQ(t.dr(hw::dsp::DSPMBH), 0U);
    // PIINT requests the DSP external interrupt
    CHECK(!t.dsp.core().external_interrupt_pending());
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_PIINT | hw::dsp::CSR_HALT));
    CHECK(t.dsp.core().external_interrupt_pending());
}

// ------------------------------------------------------------ ARAM DMA
TEST_CASE(dsp_hle_boot_rom_and_init_protocol) {
    // Ector's hash: crc ^= b; crc = rotl(crc, 3)
    const u8 one[] = {1}, two[] = {1, 2};
    CHECK_EQ(orca::dsp::hle::ucode_hash(one), 8U);
    CHECK_EQ(orca::dsp::hle::ucode_hash(two), 80U);   // ((8 ^ 2) << 3)
    const u8 wrap[] = {0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}; // 11 rotations by 3 = 33 = rotl 1 (mod 32)
    CHECK_EQ(orca::dsp::hle::ucode_hash(wrap), 0x1FEU);

    Rig r;
    r.dsp.use_hle(true);
    CHECK(r.dsp.hle());
    // reset while halted: the boot ROM's mail stays invisible until HALT is cleared
    CHECK(r.dw(0x0A, orca::hw::dsp::CSR_RES | orca::hw::dsp::CSR_HALT | orca::hw::dsp::CSR_DSPINTMASK));
    CHECK_EQ(r.dr(0x04) & 0x8000U, 0U);
    CHECK(r.dw(0x0A, orca::hw::dsp::CSR_DSPINTMASK));
    CHECK_EQ(r.dr(0x04), 0x8071U);
    CHECK_EQ(r.dr(0x06), 0xFEEDU);
    CHECK_EQ(r.dr(0x04), 0x0071U);                    // taken: bit 15 clears, the rest stays
    // a mail that is not a boot parameter is echoed as 0xFEEExxxx
    CHECK(r.dw(0x00, 0x1234)); CHECK(r.dw(0x02, 0x5678));
    CHECK_EQ(r.dr(0x00) & 0x8000U, 0U);               // taken at once by the HLE DSP
    CHECK_EQ(r.dr(0x04), 0xFEEEU); CHECK_EQ(r.dr(0x06), 0x5678U);
    // upload of a ucode Orca does not know: clear stop with its hash
    std::vector<u8> code(64);
    for (std::size_t i = 0; i < code.size(); ++i) code[i] = static_cast<u8>(i * 7U);
    CHECK(r.bus.dma_write(0x00100000U, code) == mem::BusStatus::Ok);
    auto mail = [&](u32 m) { CHECK(r.dw(0x00, static_cast<u16>(m >> 16))); CHECK(r.dw(0x02, static_cast<u16>(m))); };
    mail(0x80F3A001U); mail(0x80100000U);
    mail(0x80F3A002U); mail(64U);
    mail(0x80F3C002U); mail(0U);
    mail(0x80F3B002U); mail(0U);
    CHECK(!r.dsp.dsp_failed());
    mail(0x80F3D001U); mail(0x10U);
    CHECK(r.dsp.dsp_failed());
    char expect[16];
    std::snprintf(expect, sizeof expect, "%08X", orca::dsp::hle::ucode_hash(code));
    CHECK(r.dsp.dsp_error().find(expect) != std::string::npos);
    // CSR bit 11 with RES: the init ucode answers 0x80544348; the failure is cleared
    CHECK(r.dw(0x0A, orca::hw::dsp::CSR_RES | orca::hw::dsp::CSR_BIT11 | orca::hw::dsp::CSR_DSPINTMASK));
    CHECK(!r.dsp.dsp_failed());
    CHECK_EQ(r.dr(0x04), 0x8054U);
    CHECK_EQ(r.dr(0x06), 0x4348U);
    // the HLE is updated every millisecond of emulated time (one event pending)
    CHECK(r.sched.next_event_time().has_value());
    r.dsp.use_hle(false);
}

TEST_CASE(aram_dma_both_directions_with_register_updates) {
    Rig t;
    for (u32 i = 0; i < 0x100; i += 4) CHECK(t.bus.write32(0x1000 + i, 0xA0000000U + i) == mem::BusStatus::Ok);
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_ARINTMASK));
    // main -> ARAM, 0x100 bytes, using 32-bit register writes like Linux gcn-aram
    CHECK(t.dw32(hw::dsp::MMADDR_H, 0x00001000U));
    CHECK(t.dw32(hw::dsp::ARADDR_H, 0x00400000U));
    CHECK(t.dw32(hw::dsp::CNT_H, 0x00000100U)); // the write of CNT_L starts the DMA
    CHECK(t.dsp.aram_busy());
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_DMABUSY, hw::dsp::CSR_DMABUSY);
    CHECK(!t.cause(pi::CAUSE_DSP));
    CHECK(!t.dw(hw::dsp::MMADDR_L, 0)); // registers locked while busy
    t.settle();
    CHECK(!t.dsp.aram_busy());
    CHECK(t.cause(pi::CAUSE_DSP));
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_ARINT, hw::dsp::CSR_ARINT);
    CHECK_EQ(t.dsp.aram()[0x400000], 0xA0U);
    CHECK_EQ(t.dsp.aram()[0x4000FF], 0xFCU);
    // WiiBrew: count drops to 0, both addresses advanced by the length
    CHECK_EQ(t.dr(hw::dsp::MMADDR_L), 0x1100U);
    CHECK_EQ(t.dr(hw::dsp::ARADDR_L), 0x0100U);
    CHECK_EQ(t.dr(hw::dsp::ARADDR_H), 0x0040U);
    CHECK_EQ(t.dr(hw::dsp::CNT_L), 0U);
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_ARINT)); // ack
    CHECK(!t.cause(pi::CAUSE_DSP));
    // ARAM -> main into a different buffer
    CHECK(t.dw32(hw::dsp::MMADDR_H, 0x00002000U));
    CHECK(t.dw32(hw::dsp::ARADDR_H, 0x00400020U));
    CHECK(t.dw32(hw::dsp::CNT_H, 0x80000040U));
    t.settle();
    u32 w = 0;
    CHECK(t.bus.read32(0x2000, w) == mem::BusStatus::Ok);
    CHECK_EQ(w, 0xA0000020U);
    CHECK(t.bus.read32(0x203C, w) == mem::BusStatus::Ok);
    CHECK_EQ(w, 0xA000005CU);
    CHECK_EQ(t.dr(hw::dsp::CNT_H), 0x8000U); // direction kept (model)
}

TEST_CASE(aram_dma_alignment_timing_and_errors) {
    Rig t;
    // low five bits of the addresses and length are hard-wired to 0
    CHECK(t.dw(hw::dsp::MMADDR_L, 0x123F));
    CHECK_EQ(t.dr(hw::dsp::MMADDR_L), 0x1220U);
    CHECK(t.dw(hw::dsp::ARADDR_L, 0x001F));
    CHECK_EQ(t.dr(hw::dsp::ARADDR_L), 0U);
    // length 0
    CHECK(t.dw(hw::dsp::CNT_H, 0));
    CHECK(!t.dw(hw::dsp::CNT_L, 0x001F));
    CHECK(t.dsp.mmio_error().find("length 0") != std::string::npos);
    CHECK(!t.dsp.aram_busy());
    // outside ARAM / outside RAM
    CHECK(t.dw32(hw::dsp::ARADDR_H, 0x00FFFFE0U));
    CHECK(!t.dw(hw::dsp::CNT_L, 0x40));
    CHECK(t.dsp.mmio_error().find("outside the 16 MiB ARAM") != std::string::npos);
    CHECK(t.dw32(hw::dsp::ARADDR_H, 0));
    CHECK(t.dw32(hw::dsp::MMADDR_H, 0x017FFFE0U));
    CHECK(!t.dw(hw::dsp::CNT_L, 0x40));
    CHECK(t.dsp.mmio_error().find("not RAM") != std::string::npos);
    // timing model: cycles per 32-byte unit
    t.dsp.set_aram_cycles_per_unit(100);
    CHECK(t.dw32(hw::dsp::MMADDR_H, 0x3000U));
    const u64 t0 = t.sched.now();
    CHECK(t.dw(hw::dsp::CNT_L, 0x80)); // 4 units
    CHECK_EQ(*t.sched.next_event_time() - t0, 400U);
    t.settle();
    // no ARAM on Wii
    mem::Bus bus2;
    core::Scheduler s2;
    ProcessorInterface pi2{bus2, nullptr};
    DspInterface wii{bus2, s2, pi2, false};
    CHECK(wii.aram().empty());
    CHECK(wii.mmio_write(hw::dsp::CNT_H, 2, 0));
    CHECK(!wii.mmio_write(hw::dsp::CNT_L, 2, 0x20));
    CHECK(wii.mmio_error().find("Wii") != std::string::npos);
}

TEST_CASE(aram_register_masks_reset_values_and_timing) {
    Rig t;
    CHECK_EQ(t.dr(hw::dsp::AR_MODE), 1U);      // read-only: controller initialised
    CHECK(t.dw(hw::dsp::AR_MODE, 0x1234));
    CHECK_EQ(t.dr(hw::dsp::AR_MODE), 1U);
    CHECK_EQ(t.dr(hw::dsp::AR_REFRESH), 156U);
    CHECK(t.dw(hw::dsp::AR_REFRESH, 0xFFFF));
    CHECK_EQ(t.dr(hw::dsp::AR_REFRESH), 0x07FFU);
    CHECK(t.dw(hw::dsp::AR_SIZE, 0x0143));
    CHECK_EQ(t.dr(hw::dsp::AR_SIZE), 0x0043U);
    CHECK(t.dw(hw::dsp::MMADDR_H, 0xFFFF));
    CHECK_EQ(t.dr(hw::dsp::MMADDR_H), 0x03FFU);
    CHECK(t.dw(hw::dsp::ARADDR_H, 0xFFFF));
    CHECK_EQ(t.dr(hw::dsp::ARADDR_H), 0x03FFU);
    CHECK(t.dw(hw::dsp::CNT_H, 0xFFFF));
    CHECK_EQ(t.dr(hw::dsp::CNT_H), 0x83FFU);
    // default timing: 246 CPU cycles per 32 bytes
    Rig u;
    CHECK(u.dw32(hw::dsp::MMADDR_H, 0x1000U));
    CHECK(u.dw32(hw::dsp::ARADDR_H, 0x0100U));
    const u64 t0 = u.sched.now();
    CHECK(u.dw32(hw::dsp::CNT_H, 0x40U));
    CHECK_EQ(*u.sched.next_event_time() - t0, 492U);
    u.settle();
    // (AR_SIZE & 0xF) == 4: writes below 4 MiB are mirrored 4 MiB higher
    Rig v;
    for (u32 i = 0; i < 32; i += 4) CHECK(v.bus.write32(0x2000 + i, 0xC0DE0000U + i) == mem::BusStatus::Ok);
    CHECK(v.dw(hw::dsp::AR_SIZE, 0x0004));
    CHECK(v.dw32(hw::dsp::MMADDR_H, 0x2000U));
    CHECK(v.dw32(hw::dsp::ARADDR_H, 0x0100U));
    CHECK(v.dw32(hw::dsp::CNT_H, 0x20U));
    v.settle();
    CHECK_EQ(v.dsp.aram()[0x100], 0xC0U);
    CHECK_EQ(v.dsp.aram()[0x400100], 0xC0U);
    CHECK_EQ(v.dsp.aram()[0x40011F], 0x1CU);
}

TEST_CASE(ai_dma_blocks_interrupts_and_relatch) {
    Rig t;
    std::vector<u32> seen;
    t.dsp.set_audio_sink([&](std::span<const u8> b) {
        seen.push_back((static_cast<u32>(b[0]) << 24) | (static_cast<u32>(b[1]) << 16) | (static_cast<u32>(b[2]) << 8) | b[3]);
    });
    bool is32 = true;
    t.dsp.set_aid_rate_source([&] { return is32; });
    CHECK_EQ(t.dsp.aid_block_cycles(), 121392U); // 8 * 486e6 * 3372 / 108e6
    is32 = false;
    CHECK_EQ(t.dsp.aid_block_cycles(), 80928U);  // 8 * 486e6 * 2248 / 108e6
    is32 = true;
    for (u32 blk = 0; blk < 3; ++blk) CHECK(t.bus.write32(0x1000 + blk * 32, 0xB0000000U + blk) == mem::BusStatus::Ok);
    CHECK(t.dw(hw::dsp::AIDMA_H, 0xFFFF));
    CHECK_EQ(t.dr(hw::dsp::AIDMA_H), 0x03FFU); // GameCube mask
    CHECK(t.dw32(hw::dsp::AIDMA_H, 0x00001000U));
    CHECK_EQ(t.dr(hw::dsp::AIDMA_L), 0x1000U);
    CHECK(t.dw(hw::dsp::AIDMA_CTRL, 0x8003)); // enable, 3 blocks
    CHECK_EQ(t.dr(hw::dsp::AIDMA_LEFT), 2U);
    const u64 t0 = t.sched.now();
    t.sched.advance_to(t0 + 199);
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_AIDINT, 0U);
    t.sched.advance_to(t0 + 200);
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_AIDINT, hw::dsp::CSR_AIDINT); // raised when the DMA latches
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_HALT | hw::dsp::CSR_AIDINT));
    t.sched.advance_to(t0 + 121392);
    CHECK_EQ(seen.size(), 1U);
    CHECK_EQ(t.dr(hw::dsp::AIDMA_LEFT), 1U);
    t.sched.advance_to(t0 + 3 * 121392);
    CHECK_EQ(seen.size(), 3U);
    CHECK_EQ(t.dr(hw::dsp::CSR) & hw::dsp::CSR_AIDINT, hw::dsp::CSR_AIDINT); // count ran out: relatch + interrupt
    CHECK_EQ(t.dr(hw::dsp::AIDMA_LEFT), 2U);
    t.sched.advance_to(t0 + 4 * 121392);
    CHECK_EQ(seen.size(), 4U);
    CHECK_EQ(seen[0], 0xB0000000U);
    CHECK_EQ(seen[1], 0xB0000001U);
    CHECK_EQ(seen[2], 0xB0000002U);
    CHECK_EQ(seen[3], 0xB0000000U); // started again from the latched address
    // disabling stops the blocks; RES clears the control register
    CHECK(t.dw(hw::dsp::AIDMA_CTRL, 0x0003));
    t.sched.advance_to(t0 + 8 * 121392);
    CHECK_EQ(seen.size(), 4U);
    CHECK(t.dw(hw::dsp::AIDMA_CTRL, 0x8003));
    CHECK(t.dw(hw::dsp::CSR, hw::dsp::CSR_RES | hw::dsp::CSR_HALT));
    CHECK_EQ(t.dr(hw::dsp::AIDMA_CTRL), 0U);
    t.sched.advance_to(t0 + 12 * 121392);
    CHECK_EQ(seen.size(), 4U);
    CHECK_EQ(t.dsp.audio_blocks_played(), 4U);
    // Wii mask for the start address
    mem::Bus bus2;
    core::Scheduler s2;
    ProcessorInterface pi2{bus2, nullptr};
    hw::DspInterface wii{bus2, s2, pi2, false, 729000000ULL};
    CHECK(wii.mmio_write(hw::dsp::AIDMA_H, 2, 0xFFFF));
    u64 v = 0;
    CHECK(wii.mmio_read(hw::dsp::AIDMA_H, 2, v));
    CHECK_EQ(v, 0x1FFFU);
    CHECK_EQ(wii.aid_block_cycles(), 8ULL * 729000000ULL * 3375ULL / 108000000ULL);
    // undocumented offsets still stop
    CHECK(!t.dsp.mmio_read(0x40, 2, v));
    CHECK(!t.dsp.mmio_read(0x00, 1, v));
}

TEST_CASE(system_ai_dma_rate_follows_aicr_bit6) {
    system::System gc({});
    CHECK_EQ(gc.dsp().aid_block_cycles(), 80928U); // AICR = 0: 48 kHz
    CHECK(gc.bus().write32(0x0C006C00U, hw::ai::DSPRATE) == mem::BusStatus::Ok);
    CHECK_EQ(gc.dsp().aid_block_cycles(), 121392U); // bit 6 set: 32 kHz
}

TEST_CASE(system_maps_mi_and_dsp) {
    system::System gc({});
    CHECK(gc.bus().write16(0x0C00500AU, hw::dsp::CSR_HALT) == mem::BusStatus::Ok);
    u16 v = 0;
    CHECK(gc.bus().read16(0x0C00500AU, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, hw::dsp::CSR_HALT);
    CHECK(gc.bus().read16(0x0C004010U, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, 0xFFU);
    CHECK_EQ(gc.dsp().aram().size(), static_cast<std::size_t>(hw::dsp::kAramSize));
    system::System wii({system::Console::Wii, 0, 1});
    CHECK(wii.dsp().aram().empty());
}

// ------------------------------------------------------------ FST
namespace {
// root, /a.bin, /dir, /dir/b.bin, /dir/sub, /dir/sub/c.bin, /z.bin
std::vector<u8> sample_fst() {
    const char names[] = "a.bin\0dir\0b.bin\0sub\0c.bin\0z.bin";
    const u32 count = 7;
    std::vector<u8> f(count * 12U + sizeof names, 0);
    auto ent = [&](u32 i, u8 type, u32 name, u32 a, u32 b) {
        put32(f, i * 12U, (static_cast<u32>(type) << 24) | name);
        put32(f, i * 12U + 4U, a);
        put32(f, i * 12U + 8U, b);
    };
    ent(0, 1, 0, 0, count);
    ent(1, 0, 0, 0x8000, 0x10);   // /a.bin
    ent(2, 1, 6, 0, 6);           // /dir (parent 0, next 6)
    ent(3, 0, 10, 0x8020, 0x20);  // /dir/b.bin
    ent(4, 1, 16, 2, 6);          // /dir/sub
    ent(5, 0, 20, 0x8040, 4);     // /dir/sub/c.bin
    ent(6, 0, 26, 0x8060, 8);     // /z.bin
    std::memcpy(f.data() + count * 12U, names, sizeof names);
    return f;
}
} // namespace

TEST_CASE(fst_parses_nested_directories) {
    std::string err;
    const auto fst = disc::parse_fst_bytes(sample_fst(), 0x10000, err);
    CHECK(fst.has_value());
    if (!fst) { std::fprintf(stderr, "    %s\n", err.c_str()); return; }
    CHECK_EQ(fst->size(), 7U);
    const char* expect[] = {"/", "/a.bin", "/dir", "/dir/b.bin", "/dir/sub", "/dir/sub/c.bin", "/z.bin"};
    for (std::size_t i = 0; i < 7; ++i) CHECK((*fst)[i].path == expect[i]);
    const auto* c = disc::find_file(*fst, "/dir/sub/c.bin");
    CHECK(c != nullptr);
    if (c) { CHECK_EQ(c->offset, 0x8040U); CHECK_EQ(c->size, 4U); }
    CHECK(disc::find_file(*fst, "/dir") == nullptr);
    CHECK(disc::find_file(*fst, "/missing") == nullptr);
}

TEST_CASE(fst_rejects_malformed_tables) {
    std::string err;
    auto f = sample_fst();
    CHECK(!disc::parse_fst_bytes(std::span<const u8>(f.data(), 8), 0x10000, err));
    auto g = f; g[0] = 0;                                  // root is not a directory
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
    g = f; put32(g, 8, 1000);                              // count too large
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
    g = f; g[12] = 2;                                      // unknown type
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
    CHECK(err.find("unknown type") != std::string::npos);
    g = f; put32(g, 4 * 12 + 8, 9);                        // /dir/sub next past its parent's end
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
    CHECK(err.find("out of range") != std::string::npos);
    g = f; put32(g, 4 * 12 + 4, 5);                        // parent after itself
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
    CHECK(!disc::parse_fst_bytes(f, 0x8062, err));         // /z.bin starts inside but ends past the disc
    CHECK(err.find("/z.bin") != std::string::npos);
    CHECK(err.find("outside the disc") != std::string::npos);
    CHECK(disc::parse_fst_bytes(f, 0x8068, err).has_value()); // exactly at the end is fine
    g = f; put32(g, 12, 0x00000FFFU);                      // name offset past the string table
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
    g = f; g.back() = 'x';                                 // last name not terminated
    CHECK(!disc::parse_fst_bytes(g, 0x10000, err));
}

TEST_CASE(fst_name_offsets_use_24_bits) {
    // two entries; the file name lives at string offset 0x10004 (needs bit 16)
    std::vector<u8> f(2U * 12U + 0x10010U, 0);
    put32(f, 0, 0x01000000U);
    put32(f, 8, 2);
    put32(f, 12, 0x00010004U);
    put32(f, 16, 0x100);
    put32(f, 20, 4);
    std::memcpy(f.data() + 24 + 0x10004, "far.bin", 8);
    f[24 + 4] = 'x'; // a different name at offset 0x0004 (what a 16-bit offset would find)
    f[24 + 5] = 0;
    std::string err;
    const auto fst = disc::parse_fst_bytes(f, 0x1000, err);
    CHECK(fst.has_value());
    if (fst) CHECK((*fst)[1].path == "/far.bin");
}

TEST_CASE(fst_read_from_a_disc_image) {
    std::vector<u8> img(0x10000, 0);
    put32(img, 0x1C, 0xC2339F3DU);
    const auto f = sample_fst();
    put32(img, 0x420, 0x9000); // DOL offset (only has to be inside the image)
    put32(img, 0x424, 0x4000);
    put32(img, 0x428, static_cast<u32>(f.size()));
    std::memcpy(img.data() + 0x4000, f.data(), f.size());
    disc::MemoryDisc d(img);
    std::string err;
    const auto h = disc::parse_header(d, err);
    CHECK(h.has_value());
    if (!h) return;
    const auto fst = disc::parse_fst(d, *h, err);
    CHECK(fst.has_value());
    if (fst) CHECK_EQ(fst->size(), 7U);
    disc::DiscHeader bad = *h;
    bad.fst_size = 0;
    CHECK(!disc::parse_fst(d, bad, err));
    bad = *h;
    bad.fst_offset = 0xFFF0;
    CHECK(!disc::parse_fst(d, bad, err));
}

#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(aram_suite_program_dma_round_trip) {
    std::ifstream in(ORCA_PROGRAMS_DIR "/aram_suite.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    sys.dsp().set_aram_cycles_per_unit(64);
    std::string err;
    CHECK(sys.load_executable(file, err));
    const auto r = sys.run(50000000ULL);
    if (r.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    aram_suite stopped: %s (%s) pc=%08X\n", ppc::to_string(r.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc);
    CHECK(r.stop == ppc::StepResult::Halted);
    CHECK_EQ(port.exit_code, 0U);
    const auto& L = port.log;
    for (u32 v : L) std::printf("    aram_suite value %08X (%u)\n", v, v);
    CHECK_EQ(L.size(), 7U);
    if (L.size() != 7U) return;
    CHECK_EQ(L[0], 1U);           // busy bit seen right after the start
    CHECK_EQ(L[1], 0U);           // busy bit clear after polling
    CHECK_EQ(L[2], 0x20U);        // ARINT set
    CHECK_EQ(L[3], 0U);           // 0 mismatching words after the round trip
    CHECK_EQ(L[4], 0x00000400U);  // ARADDR advanced by 0x400 (low half)
    CHECK_EQ(L[5], 0U);           // count back to 0
    CHECK_EQ(L[6], 0U);           // PI DSP cause cleared after the ack
    CHECK_EQ(sys.dsp().aram()[0x100000], 0x00U);
    CHECK_EQ(sys.dsp().aram()[0x100007], 0x01U); // src[1] = 1, big-endian
}
#endif

TEST_MAIN("dsp_mi")
