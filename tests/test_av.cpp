// Video Interface, Serial Interface + standard controller, Audio Interface.
// VI register values are the documented NTSC/PAL defaults (YAGCD 5.3).
#include "orca/hw/audio_interface.hpp"
#include "orca/hw/serial_interface.hpp"
#include "orca/hw/video_interface.hpp"
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
struct Rig {
    mem::Bus bus;
    core::Scheduler sched;
    ProcessorInterface pi{bus, nullptr};
    VideoInterface vi{sched, pi, 486000000ULL};
    void w16(u32 o, u16 v) { CHECK(vi.mmio_write(o, 2, v)); }
    void w32(u32 o, u32 v) { CHECK(vi.mmio_write(o, 4, v)); }
    u32 r32(u32 o) { u64 v = 0; CHECK(vi.mmio_read(o, 4, v)); return static_cast<u32>(v); }
    u16 r16(u32 o) { u64 v = 0; CHECK(vi.mmio_read(o, 2, v)); return static_cast<u16>(v); }
    void ntsc(bool interlaced = true) {
        w16(vi::VTR, 0x0F06); w32(vi::HTR0, 0x476901AD); w32(vi::HTR1, 0x02EA5140);
        w32(vi::VTO, 0x00030018); w32(vi::VTE, 0x00020019);
        w16(vi::DCR, static_cast<u16>(0x0001 | (interlaced ? 0 : vi::DCR_NIN)));
    }
    void pal() {
        w16(vi::VTR, 0x11F5); w32(vi::HTR0, 0x4B6A01B0); w32(vi::VTO, 0x00010023); w32(vi::VTE, 0x00000024);
        w16(vi::DCR, 0x0101);
    }
};
constexpr u64 kNtscField = 525ULL * 429ULL * 36ULL; // 8,108,100 CPU cycles = 59.94 Hz
constexpr u64 kPalField = 625ULL * 432ULL * 36ULL;  // 9,720,000 CPU cycles = 50 Hz exactly
} // namespace

// ------------------------------------------------------------ VI
TEST_CASE(vi_timing_from_documented_ntsc_and_pal_defaults) {
    Rig t;
    t.ntsc();
    const VideoTiming n = t.vi.timing();
    CHECK(n.valid);
    CHECK_EQ(n.half_line_pixels, 429U);
    CHECK_EQ(n.odd_half_lines, 525U);
    CHECK_EQ(n.even_half_lines, 525U);
    CHECK_EQ(t.vi.cycles_per_line(), 858U * 36U);
    CHECK_EQ(486000000ULL * 1000ULL / kNtscField, 59940ULL); // 59.940 Hz field rate
    Rig p;
    p.pal();
    const VideoTiming q = p.vi.timing();
    CHECK(q.valid);
    CHECK_EQ(q.odd_half_lines, 625U);
    CHECK_EQ(q.even_half_lines, 625U);
    CHECK_EQ(486000000ULL % kPalField, 0ULL);
    CHECK_EQ(486000000ULL / kPalField, 50ULL);              // exactly 50 Hz
}

TEST_CASE(vi_position_counters) {
    Rig t;
    CHECK_EQ(t.r16(vi::DPV), 0U); // timing off
    t.ntsc();
    CHECK_EQ(t.r16(vi::DPV), 1U);
    CHECK_EQ(t.r16(vi::DPH), 1U);
    t.sched.advance_to(36 * 100);              // 100 pixels
    CHECK_EQ(t.r16(vi::DPH), 101U);
    t.sched.advance_to(36 * 858);              // one full line
    CHECK_EQ(t.r16(vi::DPV), 2U);
    CHECK_EQ(t.r16(vi::DPH), 1U);
    t.sched.advance_to(36ULL * 429 * 525);     // start of the second field: half line 525
    CHECK_EQ(t.r16(vi::DPV), 263U);
    CHECK_EQ(t.r16(vi::DPH), 430U);
    CHECK(t.vi.bottom_field());
    t.sched.advance_to(36ULL * 429 * 526);     // first full line of the second field
    CHECK_EQ(t.r16(vi::DPV), 264U);            // as documented: vcount 264
    t.sched.advance_to(36ULL * 429 * 1050 - 36);
    CHECK_EQ(t.r16(vi::DPV), 525U);            // last line of the frame
    t.sched.advance_to(36ULL * 429 * 1050);
    CHECK_EQ(t.r16(vi::DPV), 1U);              // wraps per frame
    CHECK(!t.vi.bottom_field());
    CHECK(t.vi.mmio_write(vi::DPV, 2, 77));    // read-only: ignored
    CHECK_EQ(t.r16(vi::DPV), 1U);
}

TEST_CASE(vi_non_interlaced_counts_per_field) {
    Rig t;
    t.ntsc(false);
    t.sched.advance_to(36ULL * 429 * 524);
    CHECK_EQ(t.r16(vi::DPV), 263U);
    t.sched.advance_to(36ULL * 429 * 525);
    CHECK_EQ(t.r16(vi::DPV), 1U);              // field basis: 1-263
    CHECK(!t.vi.bottom_field());
}

TEST_CASE(vi_display_interrupt_fires_every_field_and_clears) {
    Rig t;
    std::vector<u64> times;
    t.pi.mmio_write(0x04, 4, pi::CAUSE_VI);
    t.ntsc();
    t.w32(vi::DI0, 0x110701AE);                // documented default: ENB, VCT 263, HCT 430 = field 2 start
    for (int i = 0; i < 3; ++i) {
        const u64 before = t.sched.now();
        while (!t.pi.line() && t.sched.now() < before + 2 * kNtscField) t.sched.advance_to(t.sched.now() + 1000);
        // find the exact firing time with the scheduler's next event logic
        times.push_back(t.sched.now());
        CHECK((t.r32(vi::DI0) & vi::DI_INT) != 0U);
        t.w32(vi::DI0, 0x110701AE | vi::DI_INT); // writing 1 to INT does not clear...
        CHECK(t.pi.line());
        t.w32(vi::DI0, 0x110701AE);              // ...writing 0 does
        CHECK(!t.pi.line());
    }
    CHECK(times[0] >= kNtscField && times[0] < kNtscField + 1000); // half line 525 = start of field 2
    CHECK(times[1] - times[0] > kNtscField * 2 - 2000 && times[1] - times[0] < kNtscField * 2 + 2000); // once per frame
    // exact period via the scheduler
    Rig e;
    e.pi.mmio_write(0x04, 4, pi::CAUSE_VI);
    e.ntsc();
    e.w32(vi::DI1, 0x10010001);                // VCT 1, HCT 1: frame start
    e.w32(vi::DI0, 0x110701AE);
    std::vector<u64> fires;
    for (int i = 0; i < 4; ++i) {
        while (e.sched.next_event_time() && !e.pi.line()) e.sched.advance_to(*e.sched.next_event_time());
        fires.push_back(e.sched.now());
        e.w32(vi::DI0, 0x110701AE); e.w32(vi::DI1, 0x10010001); // acknowledge both
    }
    CHECK_EQ(fires[1] - fires[0], kNtscField);  // DI1 (frame start) -> DI0 (field 2) = exactly one field
    CHECK_EQ(fires[2] - fires[1], kNtscField);
    CHECK_EQ(fires[3] - fires[2], kNtscField);
}

TEST_CASE(vi_software_cannot_set_int) {
    Rig t;
    t.pi.mmio_write(0x04, 4, pi::CAUSE_VI);
    t.ntsc();
    t.w32(vi::DI2, vi::DI_INT | vi::DI_ENB | (300U << 16) | 1U); // INT=1 written while INT is 0
    CHECK_EQ(t.r32(vi::DI2) & vi::DI_INT, 0U);
    CHECK(!t.pi.line());
    CHECK_EQ(t.r32(vi::DI2), vi::DI_ENB | (300U << 16) | 1U);
}

TEST_CASE(vi_field_listener_and_xfb) {
    Rig t;
    std::vector<std::pair<u64, bool>> fields;
    t.vi.set_field_listener([&](bool bottom) { fields.emplace_back(t.sched.now(), bottom); });
    t.ntsc();
    t.sched.advance_to(kNtscField * 4);
    CHECK_EQ(fields.size(), 5U);               // fields at 0, 1, 2, 3, 4 x field length
    if (fields.size() == 5U) {
        CHECK(!fields[0].second); CHECK(fields[1].second); CHECK(!fields[2].second);
        CHECK_EQ(fields[1].first - fields[0].first, kNtscField);
    }
    t.w32(vi::TFBL, 0x10000000U | (0x00300000U >> 5));
    t.w32(vi::BFBL, 0x10000000U | (0x00300500U >> 5));
    CHECK_EQ(t.vi.xfb_address(false), 0x00300000U);
    CHECK_EQ(t.vi.xfb_address(true), 0x00300500U);
    t.w32(vi::TFBL, 0x00123400U);
    CHECK_EQ(t.vi.xfb_address(false), 0x00123400U);
    // the bottom field uses the page-offset bit of TFBL (Dolphin: "POFF for XFB bottom is
    // connected to POFF for XFB top"): TFBL without it -> BFBL is a byte address
    CHECK_EQ(t.vi.xfb_address(true), 0x00300500U >> 5);
    // scan-out description: PICCONF STD 40 (x 32 = 1280 bytes), WPL 40 (x 16 = 640 pixels), ACV 240
    t.w16(vi::PICCONF, static_cast<u16>((40U << 8) | 40U));
    t.w16(vi::VTR, static_cast<u16>((240U << 4) | 6U));
    const hw::XfbField f = t.vi.xfb_field(false);
    CHECK_EQ(f.address, 0x00123400U); CHECK_EQ(f.width, 640U); CHECK_EQ(f.stride, 1280U); CHECK_EQ(f.height, 240U);
}

TEST_CASE(vi_clock_select_and_disable) {
    Rig t;
    t.ntsc();
    CHECK(t.vi.mmio_write(vi::VICLK, 2, 1));   // 54 MHz: twice the pixel rate
    CHECK_EQ(t.vi.cycles_per_line(), 858U * 18U);
    t.w16(vi::DCR, 0);                         // ENB off
    CHECK(!t.vi.timing().valid);
    CHECK_EQ(t.r16(vi::DPV), 0U);
    u64 v = 0;
    CHECK(!t.vi.mmio_read(0x80, 2, v));
    CHECK(!t.vi.mmio_read(0x01, 2, v));
    CHECK(!t.vi.mmio_write(0x00, 1, 0));
}

// ------------------------------------------------------------ SI
namespace {
struct SiRig {
    mem::Bus bus;
    core::Scheduler sched;
    ProcessorInterface pi{bus, nullptr};
    VideoInterface vi{sched, pi, 486000000ULL};
    SerialInterface si{sched, pi, &vi};
    std::shared_ptr<StandardController> pad = std::make_shared<StandardController>();
    SiRig() { si.connect(0, pad); }
    void w(u32 o, u32 v) { CHECK(si.mmio_write(o, 4, v)); }
    u32 r(u32 o) { u64 v = 0; CHECK(si.mmio_read(o, 4, v)); return static_cast<u32>(v); }
    void transfer(unsigned ch, std::vector<u8> out, u32 in_len) {
        for (std::size_t i = 0; i < out.size(); i += 4) {
            u32 word = 0;
            for (std::size_t k = 0; k < 4; ++k) word = (word << 8) | (i + k < out.size() ? out[i + k] : 0);
            w(0x80 + static_cast<u32>(i), word);
        }
        const u32 masks = r(0x34) & (si::TCINTMSK | si::RDSTINTMSK); // keep the interrupt masks
        w(0x34, masks | (static_cast<u32>(out.size()) << 16) | (in_len << 8) | (ch << 1) | si::TSTART);
        CHECK((r(0x34) & si::TSTART) != 0U);
        sched.advance_to(sched.now() + 1);
        CHECK((r(0x34) & si::TSTART) == 0U);
    }
};
} // namespace

TEST_CASE(si_identify_origin_poll) {
    SiRig t;
    t.transfer(0, {0x00}, 3);
    CHECK_EQ(t.r(0x80) & 0xFFFFFF00U, 0x09002300U);  // standard controller, needs origin, analog mode 3
    CHECK((t.r(0x34) & si::TCINT) != 0U);
    CHECK((t.r(0x34) & si::COMERR) == 0U);
    t.pad->state.stick_x = 0x84; t.pad->state.stick_y = 0x7A;
    t.transfer(0, {0x41}, 10);
    // origin (never calibrated: the power-on neutral state): [0x00][0x80] stick 0x80 0x80
    CHECK_EQ(t.r(0x80), 0x00808080U);
    CHECK(!t.pad->need_origin);
    t.pad->state.buttons = PadButtons::A | PadButtons::START | PadButtons::Z | PadButtons::LEFT;
    t.pad->state.trigger_r = 0xC0;
    t.transfer(0, {0x40, 0x03, 0x00}, 8);
    // [000 START Y X B A] = 0x11 (need-origin now clear), [1 L R Z U D R L] = 0x80|Z|LEFT = 0x91, stick 0x84 0x7A
    CHECK_EQ(t.r(0x80), 0x1191847AU);
    CHECK_EQ(t.r(0x84), 0x80800000U | 0xC0U);         // cstick centred, L = 0, R = 0xC0
}

TEST_CASE(si_errors_and_interrupts) {
    SiRig t;
    t.pi.mmio_write(0x04, 4, pi::CAUSE_SI);
    t.w(0x34, si::TCINTMSK);
    t.transfer(1, {0x00}, 3);                          // nothing on channel 1
    CHECK((t.r(0x34) & si::COMERR) != 0U);
    CHECK((t.r(0x38) & (si::SR_NOREP << si::sisr_shift(1))) != 0U);
    CHECK(t.pi.line());                                // TCINT & TCINTMSK
    t.w(0x34, si::TCINTMSK | si::TCINT);               // ack
    CHECK(!t.pi.line());
    t.w(0x38, si::SR_NOREP << si::sisr_shift(1));      // w1c
    CHECK_EQ(t.r(0x38), 0U);
    t.transfer(0, {0x00}, 2);                          // device answers 3 bytes, 2 expected
    CHECK((t.r(0x38) & (si::SR_OVRUN << si::sisr_shift(0))) != 0U);
    t.transfer(0, {0x40, 0x03, 0x00}, 10);             // 8 bytes for 10 expected
    CHECK((t.r(0x38) & (si::SR_UNRUN << si::sisr_shift(0))) != 0U);
    CHECK(t.si.mmio_write(0x80, 4, 0x40000000U));      // poll with analog mode 0
    CHECK(t.si.mmio_write(0x84, 4, 0));
    CHECK(!t.si.mmio_write(0x34, 4, (3U << 16) | (8U << 8) | si::TSTART));
    CHECK(t.si.mmio_error().find("analog mode 0") != std::string::npos);
    CHECK(t.si.mmio_write(0x80, 4, 0x55000000U));
    CHECK(!t.si.mmio_write(0x34, 4, (1U << 16) | (8U << 8) | si::TSTART));
    CHECK(t.si.mmio_error().find("0x55") != std::string::npos);
    CHECK(!t.si.mmio_write(0x04, 4, 0));               // INBUF is read-only
}

TEST_CASE(si_automatic_polling_follows_video_lines) {
    SiRig t;
    t.pad->state.buttons = PadButtons::B;
    t.w(0x00, 0x00400300U);                            // SIC0OUTBUF: poll, mode 3, motor off
    t.w(0x30, (10U << 16) | 0x80U);                    // every 10 lines, channel 0 enabled
    t.sched.advance_to(t.sched.now() + 100000000);
    CHECK_EQ(t.r(0x38) & (si::SR_RDST << si::sisr_shift(0)), 0U); // no video timing: no polling
    // start NTSC video timing; polling starts at the next field
    t.vi.mmio_write(vi::VTR, 2, 0x0F06); t.vi.mmio_write(vi::HTR0, 4, 0x476901AD);
    t.vi.mmio_write(vi::VTO, 4, 0x00030018); t.vi.mmio_write(vi::VTE, 4, 0x00020019);
    t.vi.set_field_listener([&](bool) { t.si.on_video_field(); });
    t.vi.mmio_write(vi::DCR, 2, 0x0001);
    t.sched.advance_to(t.sched.now() + 10 * 858 * 36 + 10);
    CHECK((t.r(0x38) & (si::SR_RDST << si::sisr_shift(0))) != 0U);
    CHECK((t.r(0x34) & si::RDSTINT) != 0U);
    CHECK_EQ(t.r(0x04), 0x22808080U);                 // B + need-origin bit; reading clears RDST0
    CHECK_EQ(t.r(0x38) & (si::SR_RDST << si::sisr_shift(0)), 0U);
    CHECK_EQ(t.r(0x34) & si::RDSTINT, 0U);
}

// ------------------------------------------------------------ AI
TEST_CASE(ai_sample_counter_rates_and_interrupt) {
    mem::Bus bus;
    core::Scheduler sched;
    ProcessorInterface pi(bus, nullptr);
    AudioInterface ai(sched, pi, 486000000ULL);
    auto w = [&](u32 o, u32 v) { CHECK(ai.mmio_write(o, 4, v)); };
    auto r = [&](u32 o) { u64 v = 0; CHECK(ai.mmio_read(o, 4, v)); return static_cast<u32>(v); };
    sched.advance_to(1000);
    CHECK_EQ(r(0x08), 0U);
    // GameCube "48 kHz" = 108 MHz / 2248: one sample every 486e6 * 2248 / 108e6 = 10115.999.. cycles
    w(0x00, ai::PSTAT | ai::AISFR);
    sched.advance_to(1000 + 4855680 - 1);              // 480 samples = 480 * 2248 * 4.5 cycles = 4,855,680
    CHECK_EQ(r(0x08), 479U);
    sched.advance_to(1000 + 4855680);
    CHECK_EQ(r(0x08), 480U);
    w(0x00, 0);                                        // stop: counter frozen
    sched.advance_to(sched.now() + 1000000);
    CHECK_EQ(r(0x08), 480U);
    w(0x00, ai::SCRESET);
    CHECK_EQ(r(0x08), 0U);
    // 32 kHz with AIIT interrupt
    pi.mmio_write(0x04, 4, pi::CAUSE_AI);
    w(0x0C, 320);
    const u64 t0 = sched.now();
    w(0x00, ai::PSTAT | ai::AIINTMSK);                 // AISFR = 0: "32 kHz" = 108 MHz / 3372
    while (!pi.line() && sched.next_event_time()) sched.advance_to(*sched.next_event_time());
    CHECK(pi.line());
    CHECK_EQ(sched.now() - t0, 4855680ULL);            // 320 * 3372 * 4.5 cycles
    CHECK_EQ(r(0x08), 320U);
    CHECK((r(0x00) & ai::AIINT) != 0U);
    w(0x00, ai::PSTAT | ai::AIINTMSK | ai::AIINT);    // ack
    CHECK(!pi.line());
    // AIINTVLD: AIINT holds its value
    w(0x00, ai::PSTAT | ai::AIINTMSK | ai::AIINTVLD);
    w(0x0C, r(0x08) + 10);
    sched.advance_to(sched.now() + 15187 * 20);
    CHECK_EQ(r(0x00) & ai::AIINT, 0U);
    CHECK(!ai.mmio_write(0x08, 4, 0));                 // AISCNT is read-only
}

// ------------------------------------------------------------ whole program
#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(av_suite_program_video_input_audio) {
    std::ifstream in(ORCA_PROGRAMS_DIR "/av_suite.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    auto pad = std::make_shared<StandardController>();
    pad->state.buttons = PadButtons::A | PadButtons::START;
    pad->state.stick_x = 0xA0; pad->state.stick_y = 0x60;
    sys.si().connect(0, pad);
    std::string err;
    CHECK(sys.load_executable(file, err));
    const auto r = sys.run(400000000ULL);
    if (r.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    av_suite stopped: %s (%s) pc=%08X\n", ppc::to_string(r.stop), sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc);
    CHECK(r.stop == ppc::StepResult::Halted);
    const auto& L = port.log;
    for (u32 v : L) std::printf("    av_suite value %08X (%u)\n", v, v);
    CHECK_EQ(L.size(), 12U);
    if (L.size() != 12U) return;
    CHECK_EQ(L[0], 0x09002300U);                // SI identify: standard controller
    CHECK_EQ(L[1] >> 16, 0x0080U);              // origin buttons word
    CHECK_EQ(L[2], 0x1180A060U);                // poll: [START|A] [0x80] stick 0xA0 0x60
    CHECK_EQ(L[3] & 0x00080000U, 0x00080000U);  // channel 1: NOREP
    CHECK_EQ(L[4], 0x1180A060U);                // automatic polling: SIC0INBUFH (same bytes)
    CHECK_EQ(L[5], 3U);                         // three DI0 interrupts (field 2 start)
    CHECK(L[6] >= 2 * 675675U - 20U && L[6] <= 2 * 675675U + 20U); // one frame = 2 fields = 1,351,350 TB ticks
    CHECK(L[7] >= 2 * 675675U - 20U && L[7] <= 2 * 675675U + 20U);
    CHECK_EQ(L[8], 263U);                       // DPV latched in the handler at field 2 start
    CHECK_EQ(L[9], 1U);                         // AI interrupt flag seen
    CHECK(L[10] >= 480U && L[10] < 490U);       // AISCNT after the AIIT match
    CHECK(L[11] >= 404560U && L[11] <= 404660U); // 480 samples at 108 MHz / 2248 = 9.9911 ms = 404,640 TB ticks (40.5 MHz)
}
#endif

TEST_MAIN("av")
