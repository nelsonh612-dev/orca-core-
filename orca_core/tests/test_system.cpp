// Scheduler, Processor Interface, gather pipe, executable loaders, system run loop.
#include "orca/core/scheduler.hpp"
#include "orca/hw/processor_interface.hpp"
#include "orca/loader/executable.hpp"
#include "orca/memory/console_maps.hpp"
#include "orca/system/system.hpp"
#include "ppc_asm.hpp"
#include "test_framework.hpp"

#include <string>
#include <vector>

using namespace orca;
using namespace orca::ppc;
namespace A = orca::test::ppc_asm;

// ------------------------------------------------------------ scheduler
TEST_CASE(scheduler_orders_by_time_then_insertion) {
    core::Scheduler s;
    std::vector<std::string> log;
    s.schedule_at(30, "c", [&](u64, u64) { log.push_back("c"); });
    s.schedule_at(10, "a", [&](u64, u64) { log.push_back("a"); });
    s.schedule_at(10, "b", [&](u64, u64) { log.push_back("b"); });
    CHECK_EQ(*s.next_event_time(), 10U);
    s.advance_to(9);
    CHECK(log.empty());
    s.advance_to(30);
    CHECK(log == (std::vector<std::string>{"a", "b", "c"}));
    CHECK_EQ(s.now(), 30U);
    CHECK(!s.next_event_time());
}

TEST_CASE(scheduler_cancel_reschedule_and_lateness) {
    core::Scheduler s;
    std::vector<u64> late;
    const auto id = s.schedule_at(5, "x", [&](u64, u64) { late.push_back(999); });
    CHECK(s.cancel(id));
    CHECK(!s.cancel(id));
    // a periodic event that reschedules itself relative to its *due* time (now - late),
    // so it does not drift when it fires late
    std::vector<u64> seen_now;
    std::function<void(u64, u64)> tick = [&](u64 now, u64 l) {
        seen_now.push_back(now);
        late.push_back(l);
        const u64 due = now - l;
        if (due < 40) s.schedule_at(due + 10, "tick", tick);
    };
    s.schedule_at(10, "tick", tick);
    s.advance_to(23);           // fires at its due time 10, reschedules for 20, which also fires within this advance
    CHECK(late == (std::vector<u64>{0, 0}));   // each callback runs at its exact due time
    s.schedule_at(0, "past", [&](u64, u64 l) { late.push_back(1000 + l); });
    s.advance_to(23);           // time does not move, but the past event fires
    CHECK_EQ(late.back(), 1023U);
    s.advance_to(100);          // due 30 and due 40 fire on time; due 40 does not reschedule
    CHECK(late == (std::vector<u64>{0, 0, 1023, 0, 0}));
    CHECK(seen_now == (std::vector<u64>{10, 20, 30, 40}));
    s.advance_to(50);           // never goes backwards
    CHECK_EQ(s.now(), 100U);
}

// ------------------------------------------------------------ PI
TEST_CASE(pi_interrupt_line_follows_cause_and_mask) {
    mem::Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    std::vector<bool> edges;
    hw::ProcessorInterface pi(bus, [&](bool l) { edges.push_back(l); });
    pi.raise(hw::pi::CAUSE_VI);
    CHECK(!pi.line());                                   // masked
    u64 v = 0;
    CHECK(pi.mmio_write(0x04, 4, hw::pi::CAUSE_VI | hw::pi::CAUSE_SI));
    CHECK(pi.line());
    CHECK(pi.mmio_read(0x00, 4, v)); CHECK_EQ(v, hw::pi::CAUSE_VI);
    CHECK(pi.mmio_read(0x04, 4, v)); CHECK_EQ(v, hw::pi::CAUSE_VI | hw::pi::CAUSE_SI);
    pi.lower(hw::pi::CAUSE_VI);
    CHECK(!pi.line());
    CHECK(edges == (std::vector<bool>{true, false}));
    pi.raise(hw::pi::CAUSE_RSW);
    CHECK(pi.mmio_write(0x00, 4, hw::pi::CAUSE_VI));     // writing other bits does not acknowledge
    CHECK_EQ(pi.causes(), hw::pi::CAUSE_RSW);
    CHECK(pi.mmio_write(0x00, 4, hw::pi::CAUSE_RSW));
    CHECK_EQ(pi.causes(), 0U);
    CHECK(!pi.mmio_read(0x2C, 4, v));                    // unimplemented register: device error
    CHECK(!pi.mmio_read(0x00, 2, v));                    // 16-bit access not supported
}

TEST_CASE(gather_pipe_bursts_into_fifo_and_wraps) {
    mem::Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    hw::ProcessorInterface pi(bus, nullptr);
    hw::GatherPipe gp(pi);
    std::vector<u32> bursts;
    pi.set_burst_listener([&](u32 a) { bursts.push_back(a); });
    pi.mmio_write(0x0C, 4, 0x1000); pi.mmio_write(0x10, 4, 0x1040); pi.mmio_write(0x14, 4, 0x1000);
    for (u32 i = 0; i < 7; ++i) CHECK(gp.mmio_write(0, 4, 0x11111111U * (i + 1)));
    CHECK_EQ(gp.pending_bytes(), 28U);
    CHECK(bursts.empty());
    CHECK(gp.mmio_write(0, 8, 0x0102030405060708ULL)); // completes 32 bytes, 4 left over
    CHECK_EQ(bursts.size(), 1U);
    CHECK_EQ(gp.pending_bytes(), 4U);
    u32 w = 0;
    bus.read32(0x1000, w); CHECK_EQ(w, 0x11111111U);
    bus.read32(0x101C, w); CHECK_EQ(w, 0x01020304U);
    CHECK_EQ(pi.fifo_write_pointer(), 0x1020U);
    for (u32 i = 0; i < 7; ++i) gp.mmio_write(0, 4, 0xAAAAAAAAU);  // second burst -> reaches END, wraps
    CHECK_EQ(bursts.size(), 2U);
    bus.read32(0x1020, w); CHECK_EQ(w, 0x05060708U);
    CHECK_EQ(pi.fifo_write_pointer(), 0x1000U | hw::pi::FIFO_WRAP);
    u64 v = 0;
    CHECK(!gp.mmio_read(0, 4, v));                     // write-only
    // FIFO pointing outside RAM: the completing store fails and changes nothing
    pi.mmio_write(0x14, 4, 0x03000000U);
    for (u32 i = 0; i < 7; ++i) gp.mmio_write(0, 4, 0);
    const u32 before = gp.pending_bytes();
    CHECK(!gp.mmio_write(0, 4, 0));
    CHECK_EQ(gp.pending_bytes(), before);
}

// ------------------------------------------------------------ loaders
namespace {
void put32(std::vector<u8>& v, std::size_t off, u32 x) { v[off] = u8(x >> 24); v[off + 1] = u8(x >> 16); v[off + 2] = u8(x >> 8); v[off + 3] = u8(x); }
void put16(std::vector<u8>& v, std::size_t off, u16 x) { v[off] = u8(x >> 8); v[off + 1] = u8(x); }

std::vector<u8> tiny_dol() {
    std::vector<u8> f(0x100 + 0x20, 0);
    put32(f, 0x00, 0x100); put32(f, 0x48, 0x80003100); put32(f, 0x90, 0x10);  // text0
    put32(f, 0x1C, 0x110); put32(f, 0x64, 0x80004000); put32(f, 0xAC, 0x10);  // data0
    put32(f, 0xD8, 0x80005000); put32(f, 0xDC, 0x40);
    put32(f, 0xE0, 0x80003100);
    for (int i = 0; i < 0x20; ++i) f[0x100 + static_cast<std::size_t>(i)] = static_cast<u8>(0xA0 + i);
    return f;
}

std::vector<u8> tiny_elf() {
    std::vector<u8> f(0x200, 0);
    f[0] = 0x7F; f[1] = 'E'; f[2] = 'L'; f[3] = 'F'; f[4] = 1; f[5] = 2; f[6] = 1;
    put16(f, 0x10, 2); put16(f, 0x12, 20); put32(f, 0x14, 1);
    put32(f, 0x18, 0x80003000); put32(f, 0x1C, 0x34); put16(f, 0x2A, 32); put16(f, 0x2C, 2);
    // PT_LOAD text: file 0x100..0x110 -> 0x80003000, R X
    put32(f, 0x34, 1); put32(f, 0x38, 0x100); put32(f, 0x3C, 0x80003000); put32(f, 0x40, 0x80003000);
    put32(f, 0x44, 0x10); put32(f, 0x48, 0x10); put32(f, 0x4C, 5);
    // PT_LOAD data: 8 bytes file + 0x18 bss -> 0x80006000, R W
    put32(f, 0x54, 1); put32(f, 0x58, 0x180); put32(f, 0x5C, 0x80006000); put32(f, 0x60, 0x80006000);
    put32(f, 0x64, 0x8); put32(f, 0x68, 0x20); put32(f, 0x6C, 6);
    for (int i = 0; i < 0x10; ++i) f[0x100 + static_cast<std::size_t>(i)] = static_cast<u8>(i);
    for (int i = 0; i < 8; ++i) f[0x180 + static_cast<std::size_t>(i)] = 0xEE;
    return f;
}
} // namespace

TEST_CASE(dol_parse_and_load) {
    std::string err;
    const auto img = loader::parse_dol(tiny_dol(), err);
    CHECK(img.has_value());
    if (!img) return;
    CHECK_EQ(img->segments.size(), 2U);
    CHECK_EQ(img->entry, 0x80003100U);
    mem::Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    for (u32 a = 0x5000; a < 0x5040; a += 4) bus.write32(a, 0xFFFFFFFFU);
    CHECK(loader::load_into(bus, *img, err));
    u32 w = 0;
    bus.read32(0x3100, w); CHECK_EQ(w, 0xA0A1A2A3U);
    bus.read32(0x4000, w); CHECK_EQ(w, 0xB0B1B2B3U);
    bus.read32(0x503C, w); CHECK_EQ(w, 0U); // BSS cleared
}

TEST_CASE(dol_rejects_malformed_files) {
    std::string err;
    std::vector<u8> small(0x80, 0);
    CHECK(!loader::parse_dol(small, err).has_value());
    auto f = tiny_dol();
    put32(f, 0x90, 0x10000); // text0 size runs past the end of the file
    CHECK(!loader::parse_dol(f, err).has_value());
    CHECK(err.find("outside") != std::string::npos);
    auto g = tiny_dol();
    put32(g, 0x48, 0xFFFFFFF8U); // wraps the address space
    CHECK(!loader::parse_dol(g, err).has_value());
    std::vector<u8> empty(0x100, 0);
    CHECK(!loader::parse_dol(empty, err).has_value());
}

TEST_CASE(elf_parse_validate_and_load) {
    std::string err;
    const auto f = tiny_elf();
    const auto img = loader::parse_executable(f, err);
    CHECK(img.has_value());
    if (!img) return;
    CHECK_EQ(img->segments.size(), 2U);
    CHECK(img->segments[0].executable);
    CHECK(!img->segments[1].executable);
    CHECK_EQ(img->segments[1].memory_size, 0x20U);
    mem::Bus bus;
    bus.map_ram("ram", 0, 0x10000);
    bus.write32(0x6010, 0x12345678U);
    CHECK(loader::load_into(bus, *img, err));
    u32 w = 0;
    bus.read32(0x3004, w); CHECK_EQ(w, 0x04050607U);
    bus.read32(0x6004, w); CHECK_EQ(w, 0xEEEEEEEEU);
    bus.read32(0x6010, w); CHECK_EQ(w, 0U); // zero-filled tail
    auto bad = f; bad[5] = 1;               // little-endian
    CHECK(!loader::parse_elf(bad, err).has_value());
    auto bad2 = f; put16(bad2, 0x12, 3);    // x86
    CHECK(!loader::parse_elf(bad2, err).has_value());
    auto bad3 = f; put32(bad3, 0x64, 0x1000); // filesz past end of file
    CHECK(!loader::parse_elf(bad3, err).has_value());
}

TEST_CASE(elf_to_dol_round_trip) {
    std::string err;
    const auto img = loader::parse_elf(tiny_elf(), err);
    CHECK(img.has_value());
    if (!img) return;
    const auto dol = loader::make_dol(*img, err);
    CHECK(dol.has_value());
    if (!dol) return;
    const auto back = loader::parse_dol(*dol, err);
    CHECK(back.has_value());
    if (!back) return;
    CHECK_EQ(back->entry, 0x80003000U);
    CHECK_EQ(back->bss_address, 0x80006008U);
    CHECK_EQ(back->bss_size, 0x18U);
    mem::Bus a, b;
    a.map_ram("ram", 0, 0x10000); b.map_ram("ram", 0, 0x10000);
    for (u32 x = 0; x < 0x10000; x += 4) { a.write32(x, 0xCDCDCDCDU); b.write32(x, 0xCDCDCDCDU); }
    CHECK(loader::load_into(a, *img, err));
    CHECK(loader::load_into(b, *back, err));
    bool same = true;
    for (u32 x = 0; x < 0x10000; x += 4) { u32 p = 0, q = 0; a.read32(x, p); b.read32(x, q); same = same && p == q; }
    CHECK(same); // ELF and converted DOL produce identical memory
}

TEST_CASE(load_outside_ram_writes_nothing) {
    std::string err;
    auto f = tiny_dol();
    put32(f, 0x64, 0x817FFFF8U); // data0 straddles the end of MEM1 (24 MiB)
    const auto img = loader::parse_dol(f, err);
    CHECK(img.has_value());
    system::System sys({});
    sys.bus().write32(0x3100, 0x11111111U);
    CHECK(!loader::load_into(sys.bus(), *img, err));
    u32 w = 0; sys.bus().read32(0x3100, w);
    CHECK_EQ(w, 0x11111111U);
}

// ------------------------------------------------------------ system loop
namespace {
void code(system::System& s, u32 pa, std::initializer_list<u32> words) { for (u32 w : words) { s.bus().write32(pa, w); pa += 4; } }
} // namespace

TEST_CASE(timebase_follows_cpu_cycles) {
    system::System sys({});
    CHECK_EQ(sys.clocks().cpu_hz, 486000000ULL);
    code(sys, 0x3000, {A::b(0)}); // infinite loop
    sys.cpu().state().pc = 0x3000;
    const auto r = sys.run(1200);
    CHECK(r.stop == StepResult::Executed);
    CHECK_EQ(r.cycles, 1200U);
    CHECK_EQ(r.instructions, 1200U);
    CHECK_EQ(sys.cpu().state().tb, 100U); // 12 CPU cycles per tick
    sys.run(11);
    CHECK_EQ(sys.cpu().state().tb, 100U); // remainder carried
    sys.run(1);
    CHECK_EQ(sys.cpu().state().tb, 101U);
    system::System wii({system::Console::Wii, 0, 1});
    CHECK_EQ(wii.clocks().cpu_hz, 729000000ULL);
    CHECK(wii.bus().is_ram(mem::kMem2Base, mem::kMem2Size));
}

TEST_CASE(wii_hle_boot_maps_cached_mem2_for_code_and_data) {
    system::System wii({system::Console::Wii, 0, 1});
    // MEM2 physical data and code are reached through the cached 0x90000000
    // alias installed by HLE boot. This exercises both IBAT and DBAT setup.
    CHECK(wii.bus().write32(mem::kMem2Base, 0x12345678U) == mem::BusStatus::Ok);
    code(wii, mem::kMem2Base + 0x1000U,
         {A::addi(3, 0, 42), A::addis(4, 0, 0x9000), 0x80A40000U}); // lwz r5,0(r4)
    wii.hle_boot(0x90001000U);
    const auto result = wii.run(3U);
    CHECK(result.stop == StepResult::Executed);
    CHECK_EQ(result.instructions, 3U);
    CHECK_EQ(wii.cpu().state().gpr[3], 42U);
    CHECK_EQ(wii.cpu().state().gpr[4], 0x90000000U);
    CHECK_EQ(wii.cpu().state().gpr[5], 0x12345678U);
}

TEST_CASE(wii_hle_boot_maps_uncached_mem2_data_alias) {
    system::System wii({system::Console::Wii, 0, 1});
    CHECK(wii.bus().write32(mem::kMem2Base + 0x20U, 0x89ABCDEFU) == mem::BusStatus::Ok);
    // Execute from MEM1, then access physical MEM2 through Wii's 0xD0000000
    // uncached alias installed by the dedicated DBAT3 mapping during HLE boot.
    code(wii, 0x3000U, {A::addis(4U, 0U, 0xD000), 0x80A40020U}); // lwz r5,0x20(r4)
    wii.hle_boot(0x80003000U);
    const auto result = wii.run(2U);
    CHECK(result.stop == StepResult::Executed);
    CHECK_EQ(result.instructions, 2U);
    CHECK_EQ(wii.cpu().state().gpr[4], 0xD0000000U);
    CHECK_EQ(wii.cpu().state().gpr[5], 0x89ABCDEFU);
}

TEST_CASE(si_standard_controller_forwards_rumble_state_changes) {
    hw::StandardController pad;
    std::vector<u8> commands;
    pad.set_rumble_sink([&](u8 command) { commands.push_back(command); });
    std::string error;
    const auto start = pad.transfer({0x40U, 3U, 1U}, error);
    CHECK(start.has_value());
    CHECK_EQ(pad.motor, 1U);
    CHECK_EQ(commands.size(), 1U);
    CHECK_EQ(commands[0], 1U);

    // Repeating the same poll state must not generate redundant haptic events.
    const auto repeated = pad.transfer({0x40U, 3U, 1U}, error);
    CHECK(repeated.has_value());
    CHECK_EQ(commands.size(), 1U);

    const auto stop = pad.transfer({0x40U, 3U, 0U}, error);
    CHECK(stop.has_value());
    CHECK_EQ(pad.motor, 0U);
    CHECK_EQ(commands.size(), 2U);
    CHECK_EQ(commands[1], 0U);
}

TEST_CASE(decrementer_interrupt_through_system_loop) {
    system::System sys({});
    auto& s = sys.cpu().state();
    // main: addi r3,r3,1 ; b main      handler @0x900: addi r4,r4,1 ; set DEC = 9 ; rfi
    code(sys, 0x3000, {A::addi(3, 3, 1), A::b(-4)});
    code(sys, 0x900, {A::addi(4, 4, 1), A::addi(5, 0, 9), A::mtspr(spr::DEC, 5), A::rfi()});
    s.pc = 0x3000; s.msr = MSR_EE; s.dec = 9;
    sys.run(12 * 10 * 5); // 600 cycles = 50 TB ticks -> DEC fires every 10 ticks
    CHECK(s.gpr[4] >= 4U && s.gpr[4] <= 5U);
    CHECK(s.gpr[3] > 250U && s.gpr[3] < 300U); // ~600 instructions, 2 per main-loop iteration
    CHECK((s.msr & MSR_EE) != 0U); // rfi restored EE
}

TEST_CASE(idle_cpu_skips_to_next_event_and_pi_interrupt_wakes_it) {
    system::System sys({});
    auto& s = sys.cpu().state();
    code(sys, 0x500, {A::addi(6, 0, 77), A::b(0)}); // external interrupt handler
    s.hid0 = 0x00800000U; // DOZE
    s.msr = MSR_POW | MSR_EE;
    sys.pi().mmio_write(0x04, 4, hw::pi::CAUSE_VI);
    bool fired = false;
    sys.scheduler().schedule_at(1000000, "vi", [&](u64, u64) { fired = true; sys.pi().raise(hw::pi::CAUSE_VI); });
    const auto r = sys.run(2000000);
    CHECK(fired);
    CHECK(sys.cpu().last_exception() == Exception::External);
    CHECK_EQ(s.gpr[6], 77U);
    CHECK(r.instructions < 2000000U);       // the idle period was skipped, not executed
    CHECK_EQ(s.msr & MSR_POW, 0U);          // the exception cleared MSR[POW] (SRR1 does not save POW)
    system::System dead({});
    dead.cpu().state().hid0 = 0x00800000U;
    dead.cpu().state().msr = MSR_POW;
    CHECK(dead.run(1000).stop == StepResult::Idle); // nothing can wake it: reported
}

TEST_CASE(cpu_access_to_pi_and_gather_pipe_through_mmu) {
    system::System sys({});
    auto& s = sys.cpu().state();
    sys.hle_boot(0x80003000U);
    // r3 = 0xCC003000 (uncached PI), enable VI mask, read it back; write 32 bytes to the gather pipe
    code(sys, 0x3000, {A::addis(3, 0, static_cast<int>(0xCC00U)), A::ori(3, 3, 0x3000), A::addi(4, 0, 0x100), A::stw(4, 4, 3),
                       A::lwz(5, 4, 3), A::addis(6, 0, static_cast<int>(0xCC00U)), A::ori(6, 6, 0x8000)});
    for (int i = 0; i < 8; ++i) code(sys, 0x301C + static_cast<u32>(i) * 4, {A::stw(4, 0, 6)});
    code(sys, 0x303C, {A::lwz(7, 0x2C, 3)}); // unimplemented PI register
    sys.pi().mmio_write(0x0C, 4, 0x8000); sys.pi().mmio_write(0x10, 4, 0x9000); sys.pi().mmio_write(0x14, 4, 0x8000);
    const auto r = sys.run(100);
    CHECK_EQ(s.gpr[5], 0x100U);
    CHECK_EQ(sys.pi().mask(), hw::pi::CAUSE_VI);
    u32 w = 0; sys.bus().read32(0x801C, w);
    CHECK_EQ(w, 0x100U);
    CHECK_EQ(sys.pi().fifo_write_pointer(), 0x8020U);
    CHECK(r.stop == StepResult::MemoryFault);           // clear stop at the unimplemented register
    CHECK_EQ(s.pc, 0x8000303CU);
    CHECK_EQ(sys.cpu().last_stop().address, 0x0C00302CU);
}

TEST_MAIN("system")

TEST_CASE(system_exposes_frontend_controller_on_channel_zero) {
    system::System sys({});
    auto& pad = sys.controller();
    pad.state.buttons = hw::PadButtons::A | hw::PadButtons::UP;
    pad.state.stick_x = 0x91;
    pad.state.stick_y = 0x72;
    std::string error;
    const auto reply = pad.transfer({0x40, 0x03, 0x00}, error);
    CHECK(reply.has_value());
    if (!reply) return;
    CHECK_EQ(reply->size(), 8U);
    CHECK_EQ((*reply)[0], static_cast<u8>(0x21U)); // A + NEEDORIGIN
    CHECK_EQ((*reply)[1], static_cast<u8>(0x88U)); // UP
    CHECK_EQ((*reply)[2], static_cast<u8>(0x91U));
    CHECK_EQ((*reply)[3], static_cast<u8>(0x72U));
}
