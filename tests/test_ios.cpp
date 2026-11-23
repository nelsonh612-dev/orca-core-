// Wii: Hollywood IPC registers and the IOS kernel (request decoding, the
// acknowledge/reply handshake, descriptors, ordering, explicit stops).
#include "orca/hw/hollywood_ipc.hpp"
#include "orca/ios/kernel.hpp"
#include "orca/ios/stm.hpp"
#include "orca/ios/es.hpp"
#include "orca/ios/fs_device.hpp"
#include "orca/ios/nand_fs.hpp"
#include "orca/ios/di_device.hpp"
#include "orca/ios/memory_values.hpp"
#include "orca/ios/bluetooth.hpp"
#include "orca/ios/nand_setup.hpp"
#include "orca/disc/disc_image.hpp"
#include "test_port.hpp"
#include "wii_disc_builder.hpp"
#include "orca/memory/console_maps.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::hw::hollywood;

namespace {
constexpr u32 kTick = 12; // Broadway cycles per timebase tick

// Test device: ioctl 1 copies `in` to `out` reversed and answers the length;
// ioctlv 2 answers the sum of all vector lengths and writes the input count to
// the first in/out vector; ioctl 3 answers later (deferred).
struct TestDevice final : ios::Device {
    TestDevice() : Device("/dev/test") {}
    std::vector<u32> deferred;
    ios::Result open(ios::Kernel&, const ios::Request&, const std::string&, u32 mode) override { return ios::Result::reply(mode == 7 ? -1 : 0, 300); }
    ios::Result close(ios::Kernel&, const ios::Request&) override { return ios::Result::reply(0, 200); }
    ios::Result ioctl(ios::Kernel& k, const ios::Request& r, u32 n, ios::IoVector in, ios::IoVector out) override {
        if (n == 3) { deferred.push_back(r.address); return ios::Result::deferred(); }
        if (n != 1) return Device::ioctl(k, r, n, in, out);
        std::vector<u8> b(in.size);
        if (!k.read_bytes(in.address, b)) return ios::Result::stop("bad in");
        std::vector<u8> rev(b.rbegin(), b.rend());
        rev.resize(out.size);
        if (!k.write_bytes(out.address, rev)) return ios::Result::stop("bad out");
        return ios::Result::reply(static_cast<s32>(in.size), 1000);
    }
    ios::Result ioctlv(ios::Kernel& k, const ios::Request& r, u32 n, std::span<const ios::IoVector> in,
                       std::span<const ios::IoVector> io) override {
        if (n != 2) return Device::ioctlv(k, r, n, in, io);
        u32 sum = 0;
        for (auto v : in) sum += v.size;
        for (auto v : io) sum += v.size;
        if (!io.empty() && !k.write32(io[0].address, static_cast<u32>(in.size()))) return ios::Result::stop("bad io");
        return ios::Result::reply(static_cast<s32>(sum), 400);
    }
};

struct Rig {
    mem::Bus bus;
    core::Scheduler sched;
    bool line = false;
    hw::ProcessorInterface pi{bus, [this](bool l) { line = l; }};
    hw::HollywoodIpc ipc{sched, pi, kTick};
    ios::Kernel kernel{bus, sched, ipc, kTick};
    TestDevice* dev = nullptr;
    Rig() {
        mem::map_wii_ram(bus);
        bus.map_mmio("Hollywood", kBase, 0x1000, ipc);
        auto d = std::make_unique<TestDevice>();
        dev = d.get();
        kernel.add_device(std::move(d));
        pi.mmio_write(0x04, 4, 0xFFFFFFFFU); // PI mask: everything
    }
    void ticks(u64 n) { sched.advance_to(sched.now() + n * kTick); }
    u32 reg(u32 off) { u32 v = 0; CHECK(bus.read32(kBase + off, v) == mem::BusStatus::Ok); return v; }
    void set(u32 off, u32 v) { CHECK(bus.write32(kBase + off, v) == mem::BusStatus::Ok); }
    u32 mem32(u32 a) { u32 v = 0; bus.read32(a, v); return v; }
    void put_string(u32 a, const std::string& s) {
        for (std::size_t i = 0; i <= s.size(); ++i) bus.write8(a + static_cast<u32>(i), i < s.size() ? static_cast<u8>(s[i]) : 0);
    }
    void block(u32 a, u32 cmd, u32 fd, std::vector<u32> args) {
        for (u32 i = 0; i < 16; ++i) bus.write32(a + 4 * i, 0);
        bus.write32(a, cmd);
        bus.write32(a + 8, fd);
        for (std::size_t i = 0; i < args.size(); ++i) bus.write32(a + 0x0C + 4 * static_cast<u32>(i), args[i]);
    }
    // What Broadway's IPC library does: send, then take the acknowledge
    void send(u32 a) {
        set(PPCMSG, a);
        set(PPCCTRL, (reg(PPCCTRL) & 0x30U) | CTRL_X1);
    }
    // Waits (at most `limit` ticks) for the IPC interrupt and handles it; returns
    // the reply address for a reply, 0 for an acknowledge.
    std::optional<u32> service(u64 limit) {
        const u64 end = sched.now() + limit * kTick;
        while (!line) {
            const auto next = sched.next_event_time();
            if (!next || *next > end) { sched.advance_to(end); break; }
            sched.advance_to(*next);
        }
        if (!line) return std::nullopt;
        const u32 c = reg(PPCCTRL);
        if ((c & CTRL_Y1) != 0U) {
            const u32 reply = reg(ARMMSG);
            set(PPCCTRL, (c & 0x30U) | CTRL_Y1);
            set(PPC_IRQFLAG, IRQ_IPC_BROADWAY);
            return reply;
        }
        set(PPCCTRL, (c & 0x30U) | CTRL_Y2);
        set(PPC_IRQFLAG, IRQ_IPC_BROADWAY);
        return 0U;
    }
    // full round trip of one request; returns the result word
    s32 call(u32 a) {
        send(a);
        CHECK(service(2000) == std::optional<u32>(0U));  // acknowledge
        CHECK(service(10000000) == std::optional<u32>(a)); // reply
        CHECK_EQ(mem32(a), 8U);
        return static_cast<s32>(mem32(a + 4));
    }
};
} // namespace

TEST_CASE(ipc_handshake_timing_and_reply_block) {
    Rig r;
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/test");
    r.block(0x1000, 1, 0, {0x2000, 3});
    r.send(0x1000);
    CHECK_EQ(r.reg(PPCCTRL), 0x31U);          // X1 set, waiting for IOS
    r.ticks(499);
    CHECK_EQ(r.reg(PPCCTRL) & CTRL_Y2, 0U);   // IOS takes the request after 500 ticks
    r.ticks(1);
    CHECK_EQ(r.reg(PPCCTRL), 0x32U);          // X1 cleared, Y2 (acknowledge) set
    CHECK(!r.line);
    r.ticks(99);
    CHECK(!r.line);                           // the interrupt follows ~100 ticks later
    r.ticks(1);
    CHECK(r.line);
    CHECK_EQ(r.pi.causes() & hw::pi::CAUSE_HOLLYWOOD, hw::pi::CAUSE_HOLLYWOOD);
    CHECK_EQ(r.reg(PPC_IRQFLAG), IRQ_IPC_BROADWAY);
    // The device answered at once (300 ticks), but the reply waits for the acknowledge
    r.ticks(1000);
    CHECK_EQ(r.reg(PPCCTRL) & CTRL_Y1, 0U);
    CHECK_EQ(r.mem32(0x1000), 8U);            // the block is already written back
    CHECK_EQ(r.mem32(0x1004), 0U);            // fd 0
    CHECK_EQ(r.mem32(0x1008), 1U);            // the command answered
    r.set(PPCCTRL, 0x30U | CTRL_Y2);
    CHECK(r.line);                            // the flag is still set
    r.set(PPC_IRQFLAG, IRQ_IPC_BROADWAY);
    CHECK(!r.line);
    CHECK_EQ(r.reg(PPCCTRL) & CTRL_Y1, CTRL_Y1); // ready again: the reply goes out
    CHECK_EQ(r.reg(ARMMSG), 0x1000U);
    r.ticks(99);
    CHECK(!r.line);
    r.ticks(1);
    CHECK(r.line);
    CHECK_EQ(r.kernel.requests_executed(), 1U);
}

TEST_CASE(ipc_writing_y_bits_with_interrupt_enabled_raises_the_flag) {
    Rig r;
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1);
    CHECK(!r.line);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_Y1);       // clearing an already clear Y1 still interrupts
    CHECK(r.line);
    r.set(PPC_IRQFLAG, IRQ_IPC_BROADWAY);
    CHECK(!r.line);
    r.set(PPCCTRL, CTRL_Y2);                  // IY2 off: no interrupt
    CHECK(!r.line);
    CHECK_EQ(r.reg(PPC_IRQFLAG), 0U);
    // mask off: the flag is set but the PI line stays low
    r.set(PPC_IRQMASK, 0);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_Y1);
    CHECK(!r.line);
    CHECK_EQ(r.reg(PPC_IRQFLAG), IRQ_IPC_BROADWAY);
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    CHECK(r.line);
}

TEST_CASE(ios_descriptors_ioctl_ioctlv_close) {
    Rig r;
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/test");
    r.block(0x1000, 1, 0, {0x2000, 1});
    CHECK_EQ(r.call(0x1000), 0);               // fd 0
    r.block(0x1000, 1, 0, {0x2000, 1});
    CHECK_EQ(r.call(0x1000), 1);               // fd 1
    r.block(0x1000, 1, 0, {0x2000, 7});
    CHECK_EQ(r.call(0x1000), -1);              // the device refuses: no descriptor used
    // ioctl 1: reverse 5 bytes
    const u8 in[5] = {1, 2, 3, 4, 5};
    for (u32 i = 0; i < 5; ++i) r.bus.write8(0x3000 + i, in[i]);
    r.block(0x1000, 6, 1, {1, 0x3000, 5, 0x3100, 5});
    CHECK_EQ(r.call(0x1000), 5);
    u8 b = 0;
    r.bus.read8(0x3100, b); CHECK_EQ(b, 5U);
    r.bus.read8(0x3104, b); CHECK_EQ(b, 1U);
    // ioctlv 2: 2 in + 1 io vectors in MEM2
    const u32 vt = 0x10000000U;
    const u32 vec[6] = {0x3000, 4, 0x3010, 8, 0x3200, 16};
    for (u32 i = 0; i < 6; ++i) r.bus.write32(vt + 4 * i, vec[i]);
    r.block(0x1000, 7, 1, {2, 2, 1, vt});
    CHECK_EQ(r.call(0x1000), 28);
    CHECK_EQ(r.mem32(0x3200), 2U);
    // close fd 1, then use it: invalid
    r.block(0x1000, 2, 1, {});
    CHECK_EQ(r.call(0x1000), 0);
    CHECK(r.kernel.fd_device(1) == nullptr);
    r.block(0x1000, 6, 1, {1, 0x3000, 5, 0x3100, 5});
    CHECK_EQ(r.call(0x1000), ios::err::kInvalid);
    r.block(0x1000, 6, 0x40, {1, 0, 0, 0, 0});
    CHECK_EQ(r.call(0x1000), ios::err::kInvalid);
    // unknown command number
    r.block(0x1000, 9, 0, {});
    CHECK_EQ(r.call(0x1000), ios::err::kInvalid);
    CHECK(!r.kernel.stopped());
}

TEST_CASE(ios_all_descriptors_in_use) {
    Rig r;
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/test");
    for (u32 i = 0; i < ios::kMaxFds; ++i) {
        r.block(0x1000, 1, 0, {0x2000, 1});
        CHECK_EQ(r.call(0x1000), static_cast<s32>(i));
    }
    r.block(0x1000, 1, 0, {0x2000, 1});
    CHECK_EQ(r.call(0x1000), ios::err::kMaxFds);
}

TEST_CASE(ios_deferred_replies_leave_in_order) {
    Rig r;
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/test");
    r.block(0x1000, 1, 0, {0x2000, 1});
    CHECK_EQ(r.call(0x1000), 0);
    // A: deferred ioctl; B: immediate ioctl (1000 ticks)
    r.block(0x1100, 6, 0, {3, 0, 0, 0, 0});
    r.send(0x1100);
    CHECK(r.service(2000) == std::optional<u32>(0U));
    CHECK_EQ(r.dev->deferred.size(), 1U);
    r.block(0x1200, 6, 0, {1, 0x3000, 0, 0x3100, 0});
    r.send(0x1200);
    CHECK(r.service(2000) == std::optional<u32>(0U));
    CHECK(r.service(5000) == std::optional<u32>(0x1200U)); // B is done first
    // A completes later with a short delay
    r.kernel.reply(0x1100, 77, 10);
    CHECK(r.service(1000) == std::optional<u32>(0x1100U));
    CHECK_EQ(r.mem32(0x1104), 77U);
    CHECK_EQ(r.mem32(0x1108), 6U);
    // a reply for a request that is not pending stops
    r.kernel.reply(0x1100, 1, 0); // not pending any more: explicit stop
    CHECK(r.kernel.stopped());
    CHECK(r.kernel.stop_reason().find("not a pending request") != std::string::npos);
}

TEST_CASE(ios_unknown_paths_and_commands_stop_explicitly) {
    {
        Rig r;
        r.put_string(0x2000, "/dev/es");
        r.block(0x1000, 1, 0, {0x2000, 0});
        r.send(0x1000);
        r.ticks(600);
        CHECK(r.kernel.stopped());
        CHECK(r.kernel.stop_reason().find("/dev/es") != std::string::npos);
    }
    {
        Rig r;
        r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
        r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
        r.put_string(0x2000, "/dev/test");
        r.block(0x1000, 1, 0, {0x2000, 0});
        CHECK_EQ(r.call(0x1000), 0);
        r.block(0x1000, 3, 0, {0x3000, 4});      // read: not implemented by the device
        r.send(0x1000);
        r.ticks(600);
        CHECK(r.kernel.stopped());
        CHECK(r.kernel.stop_reason() == "IOS /dev/test: read is not emulated");
    }
    {
        Rig r;
        std::string longpath(80, 'a');
        r.put_string(0x2000, longpath);
        r.block(0x1000, 1, 0, {0x2000, 0});
        r.send(0x1000);
        r.ticks(600);
        CHECK(r.kernel.stopped());
        CHECK(r.kernel.stop_reason().find("at most 63") != std::string::npos);
    }
}

TEST_CASE(hollywood_gpio_resets_and_unknown_registers) {
    Rig r;
    bool disc = false;
    r.ipc.set_disc_present([&] { return disc; });
    // Broadway owns only some pins: the fan (bit 2) cannot be switched from here
    r.set(GPIOB_OUT, GPIO_SLOT_LED | GPIO_SENSOR_BAR | 0x4U);
    CHECK_EQ(r.reg(GPIOB_OUT), GPIO_SLOT_LED | GPIO_SENSOR_BAR);
    CHECK_EQ(r.ipc.gpio_out() & 0x4U, 0U);
    // A/V encoder data pin as output: the input register shows the slot sensor
    CHECK_EQ(r.reg(GPIOB_IN), 0U);
    disc = true;
    CHECK_EQ(r.reg(GPIOB_IN), GPIO_SLOT_IN);
    // eject pin, A/V encoder data as input, asserting a reset, unknown registers: device errors
    CHECK(r.bus.write32(kBase + GPIOB_OUT, GPIO_DO_EJECT) == mem::BusStatus::DeviceError);
    CHECK(r.ipc.mmio_error().find("eject") != std::string::npos);
    r.set(GPIOB_DIR, kGpioOutputsAfterIos & ~GPIO_AVE_SDA);
    u32 v = 0;
    CHECK(r.bus.read32(kBase + GPIOB_IN, v) == mem::BusStatus::DeviceError);
    CHECK_EQ(r.reg(RESETS), 0xFFFFFFFFU);
    r.set(RESETS, 0xFFFFFFFFU);
    CHECK(r.bus.write32(kBase + RESETS, 0xFFFFFBFFU) == mem::BusStatus::DeviceError);
    CHECK(r.ipc.mmio_error().find("00000400") != std::string::npos);
    CHECK(r.bus.read32(kBase + 0x18, v) == mem::BusStatus::DeviceError);
    CHECK(r.bus.read32(kBase + 0x64, v) == mem::BusStatus::DeviceError);
    CHECK(r.bus.write32(kBase + VISOLID, 1) == mem::BusStatus::DeviceError);
    r.set(VISOLID, 0);
    u16 h = 0;
    CHECK(r.bus.read16(kBase + PPCCTRL, h) == mem::BusStatus::DeviceError);
}

TEST_CASE(system_maps_hollywood_only_on_wii) {
    system::SystemConfig gc;
    system::System g(gc);
    CHECK(g.hollywood() == nullptr);
    CHECK(g.ios() == nullptr);
    u32 v = 0;
    CHECK(g.bus().read32(kBase + PPCCTRL, v) != mem::BusStatus::Ok);

    system::SystemConfig wc;
    wc.console = system::Console::Wii;
    system::System w(wc);
    CHECK(w.hollywood() != nullptr);
    CHECK(w.ios() != nullptr);
    CHECK(w.bus().read32(kBase + PPCCTRL, v) == mem::BusStatus::Ok);
    // the legacy page is mirrored at 0x0D006000 (AI control register at +0xC00)
    u32 a = 0, b = 0;
    CHECK(w.bus().read32(0x0C006C00U, a) == mem::BusStatus::Ok);
    CHECK(w.bus().read32(0x0D006C00U, b) == mem::BusStatus::Ok);
    CHECK_EQ(a, b);
    // an IOS stop ends System::run with the reason
    w.ios()->stop("test stop");
    const auto res = w.run(1000);
    CHECK(res.device_stop);
    CHECK(res.device_error == "test stop");
}

TEST_CASE(stm_event_hook_release_and_buttons) {
    Rig r;
    auto hook = std::make_unique<ios::StmEventHook>();
    ios::StmEventHook* h = hook.get();
    r.kernel.add_device(std::move(hook));
    auto imm = std::make_unique<ios::StmImmediate>(*h);
    ios::StmImmediate* im = imm.get();
    r.kernel.add_device(std::move(imm));
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/stm/eventhook");
    r.put_string(0x2040, "/dev/stm/immediate");
    r.block(0x1000, 1, 0, {0x2000, 0});
    CHECK_EQ(r.call(0x1000), 0);
    r.block(0x1000, 1, 0, {0x2040, 0});
    CHECK_EQ(r.call(0x1000), 1);
    // release with no hook: not found
    r.block(0x1000, 6, 1, {0x3002, 0x3000, 0x20, 0x3100, 0x20});
    CHECK_EQ(r.call(0x1000), ios::err::kNotFound);
    // install the hook (stays pending), then a second one: exists
    r.bus.write32(0x3200, 0xFFFFFFFFU);
    r.block(0x1100, 6, 0, {0x1000, 0, 0, 0x3200, 0x20});
    r.send(0x1100);
    CHECK(r.service(2000) == std::optional<u32>(0U));
    CHECK(h->hook_installed());
    r.block(0x1000, 6, 0, {0x1000, 0, 0, 0x3300, 0x20});
    CHECK_EQ(r.call(0x1000), ios::err::kExists);
    // reset button: the hook answers with the event
    CHECK(h->trigger(r.kernel, ios::kStmEventReset, 0));
    CHECK(r.service(1000) == std::optional<u32>(0x1100U));
    CHECK_EQ(r.mem32(0x3200), ios::kStmEventReset);
    CHECK(!h->hook_installed());
    CHECK(!h->trigger(r.kernel, ios::kStmEventPower, 0));
    // install again, release from immediate: hook answers 0 first, then the release
    r.block(0x1100, 6, 0, {0x1000, 0, 0, 0x3200, 0x20});
    r.send(0x1100);
    CHECK(r.service(2000) == std::optional<u32>(0U));
    r.block(0x1000, 6, 1, {0x3002, 0x3000, 0x20, 0x3100, 0x20});
    r.send(0x1000);
    CHECK(r.service(2000) == std::optional<u32>(0U));
    CHECK(r.service(1000) == std::optional<u32>(0x1100U));
    CHECK_EQ(r.mem32(0x3200), 0U);
    CHECK(r.service(10000) == std::optional<u32>(0x1000U));
    CHECK_EQ(static_cast<s32>(r.mem32(0x1004)), 0);
    // dimming / LED mode are recorded
    r.bus.write32(0x3000, 1);
    r.block(0x1000, 6, 1, {0x5001, 0x3000, 0x14, 0x3100, 0x14});
    CHECK_EQ(r.call(0x1000), 0);
    CHECK(im->dimming_request() == std::optional<u32>(1U));
    r.bus.write32(0x3000, 2);
    r.block(0x1000, 6, 1, {0x6002, 0x3000, 0x14, 0x3100, 0x14});
    CHECK_EQ(r.call(0x1000), 0);
    CHECK(im->led_mode_request() == std::optional<u32>(2U));
    // power off: explicit stop
    r.block(0x1000, 6, 1, {0x2003, 0, 0, 0, 0});
    r.send(0x1000);
    r.ticks(600);
    CHECK(r.kernel.stopped());
    CHECK(r.kernel.stop_reason().find("power off") != std::string::npos);
}

namespace {
void put64(std::vector<u8>& b, std::size_t at, u64 v) { for (int i = 0; i < 8; ++i) b[at + static_cast<std::size_t>(i)] = static_cast<u8>(v >> (56 - 8 * i)); }
std::vector<u8> make_ticket(u64 title) {
    std::vector<u8> t(0x2A4);
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = static_cast<u8>(i * 7 + 1);
    t[0x1BC] = 0;  // ticket version
    put64(t, 0x1DC, title);
    return t;
}
std::vector<u8> make_tmd(u64 title, u16 contents) {
    std::vector<u8> t(0x1E4 + 0x24U * contents);
    for (std::size_t i = 0; i < t.size(); ++i) t[i] = static_cast<u8>(i * 13 + 5);
    put64(t, 0x18C, title);
    t[0x1DE] = static_cast<u8>(contents >> 8);
    t[0x1DF] = static_cast<u8>(contents);
    return t;
}
std::vector<u8> read_mem(Rig& r, u32 a, std::size_t n) {
    std::vector<u8> v(n);
    for (std::size_t i = 0; i < n; ++i) r.bus.read8(a + static_cast<u32>(i), v[i]);
    return v;
}
void write_mem(Rig& r, u32 a, const std::vector<u8>& v) {
    for (std::size_t i = 0; i < v.size(); ++i) r.bus.write8(a + static_cast<u32>(i), v[i]);
}
// ioctlv on fd 0 with vectors {address, size}...; the vector table at 0x2800
s32 ioctlv(Rig& r, u32 n, std::vector<std::pair<u32, u32>> in, std::vector<std::pair<u32, u32>> io) {
    u32 i = 0;
    for (auto& v : in) { r.bus.write32(0x2800 + 8 * i, v.first); r.bus.write32(0x2804 + 8 * i, v.second); ++i; }
    for (auto& v : io) { r.bus.write32(0x2800 + 8 * i, v.first); r.bus.write32(0x2804 + 8 * i, v.second); ++i; }
    r.block(0x1000, 7, 0, {n, static_cast<u32>(in.size()), static_cast<u32>(io.size()), 0x2800});
    return r.call(0x1000);
}
} // namespace

TEST_CASE(es_active_title_ids_views_and_tmd) {
    Rig r;
    auto es_dev = std::make_unique<ios::Es>();
    ios::Es* es = es_dev.get();
    r.kernel.add_device(std::move(es_dev));
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/es");
    r.block(0x1000, 1, 0, {0x2000, 0});
    CHECK_EQ(r.call(0x1000), 0);
    // no active title yet
    CHECK_EQ(ioctlv(r, 0x20, {}, {{0x4000, 8}}), ios::es_err::kInvalid);
    const u64 title = 0x0001000052534245ULL; // 00010000/RSBE
    std::string error;
    CHECK(!es->set_active_title(make_ticket(title + 1), make_tmd(title, 2), error));
    CHECK(error.find("does not match") != std::string::npos);
    CHECK(!es->set_active_title(make_ticket(title), std::vector<u8>(0x100), error));
    const auto ticket = make_ticket(title);
    const auto tmd = make_tmd(title, 2);
    CHECK(es->set_active_title(ticket, tmd, error));
    // title ID
    CHECK_EQ(ioctlv(r, 0x20, {}, {{0x4000, 8}}), 0);
    CHECK_EQ(r.mem32(0x4000), 0x00010000U);
    CHECK_EQ(r.mem32(0x4004), 0x52534245U);
    CHECK_EQ(ioctlv(r, 0x20, {}, {{0x4000, 4}}), ios::es_err::kInvalid);
    // title directory
    r.bus.write32(0x4100, 0x00010000U); r.bus.write32(0x4104, 0x52534245U);
    CHECK_EQ(ioctlv(r, 0x1D, {{0x4100, 8}}, {{0x4200, 30}}), 0);
    const auto dir = read_mem(r, 0x4200, 30);
    CHECK(std::string(reinterpret_cast<const char*>(dir.data())) == "/title/00010000/52534245/data");
    // ticket views
    CHECK_EQ(ioctlv(r, 0x12, {{0x4100, 8}}, {{0x4300, 4}}), 0);
    CHECK_EQ(r.mem32(0x4300), 1U);
    r.bus.write32(0x4310, 1);
    CHECK_EQ(ioctlv(r, 0x13, {{0x4100, 8}, {0x4310, 4}}, {{0x5000, 0xD8}}), 0);
    const auto tv = read_mem(r, 0x5000, 0xD8);
    CHECK_EQ(tv[0], ticket[0x1BC]);
    CHECK(tv[1] == 0 && tv[2] == 0 && tv[3] == 0);
    CHECK(std::equal(tv.begin() + 4, tv.end(), ticket.begin() + 0x1D0));
    // TMD view: 0x5C + 2 * 0x10 bytes
    CHECK_EQ(ioctlv(r, 0x14, {{0x4100, 8}}, {{0x4300, 4}}), 0);
    CHECK_EQ(r.mem32(0x4300), 0x7CU);
    r.bus.write32(0x4310, 0x7C);
    CHECK_EQ(ioctlv(r, 0x15, {{0x4100, 8}, {0x4310, 4}}, {{0x5100, 0x7C}}), 0);
    const auto view = read_mem(r, 0x5100, 0x7C);
    CHECK(std::equal(view.begin(), view.begin() + 0x58, tmd.begin() + 0x180));
    CHECK(std::equal(view.begin() + 0x58, view.begin() + 0x5C, tmd.begin() + 0x1DC));
    CHECK(std::equal(view.begin() + 0x5C, view.begin() + 0x6C, tmd.begin() + 0x1E4));
    CHECK(std::equal(view.begin() + 0x6C, view.begin() + 0x7C, tmd.begin() + 0x1E4 + 0x24));
    r.bus.write32(0x4310, 0x7B);
    CHECK_EQ(ioctlv(r, 0x15, {{0x4100, 8}, {0x4310, 4}}, {{0x5100, 0x7C}}), ios::es_err::kInvalid);
    // DI views: active title, and a TMD passed in
    CHECK_EQ(ioctlv(r, 0x19, {{0, 0}}, {{0x4300, 4}}), 0);
    CHECK_EQ(r.mem32(0x4300), 0x7CU);
    const auto tmd3 = make_tmd(title, 3);
    write_mem(r, 0x6000, tmd3);
    CHECK_EQ(ioctlv(r, 0x19, {{0x6000, static_cast<u32>(tmd3.size())}}, {{0x4300, 4}}), 0);
    CHECK_EQ(r.mem32(0x4300), 0x8CU);
    r.bus.write32(0x4310, 0x8C);
    CHECK_EQ(ioctlv(r, 0x1A, {{0x6000, static_cast<u32>(tmd3.size())}, {0x4310, 4}}, {{0x5200, 0x8C}}), 0);
    CHECK(read_mem(r, 0x5200, 0x8C) == ios::tmd_view(tmd3));
    CHECK_EQ(ioctlv(r, 0x1B, {{0, 0}}, {{0x5300, 0xD8}}), 0);
    CHECK(read_mem(r, 0x5300, 0xD8) == tv);
    CHECK_EQ(ioctlv(r, 0x1B, {{0x6000, 0x100}}, {{0x5300, 0xD8}}), ios::es_err::kInvalid);
    CHECK_EQ(ioctlv(r, 0x39, {}, {{0x4300, 4}}), 0);
    CHECK_EQ(r.mem32(0x4300), static_cast<u32>(tmd.size()));
    r.bus.write32(0x4310, static_cast<u32>(tmd.size()));
    CHECK_EQ(ioctlv(r, 0x3A, {{0x4310, 4}}, {{0x7000, static_cast<u32>(tmd.size())}}), 0);
    CHECK(read_mem(r, 0x7000, tmd.size()) == tmd);
    // consumption
    r.bus.write32(0x4320, 0xFFFFFFFFU);
    CHECK_EQ(ioctlv(r, 0x16, {{0x4100, 8}}, {{0x4400, 0x40}, {0x4320, 4}}), 0);
    CHECK_EQ(r.mem32(0x4320), 0U);
    // vector count mismatch
    CHECK_EQ(ioctlv(r, 0x12, {}, {{0x4300, 4}}), ios::es_err::kInvalid);
    // another title: explicit stop
    r.bus.write32(0x4104, 0x41414141U);
    r.bus.write32(0x2800, 0x4100); r.bus.write32(0x2804, 8);
    r.bus.write32(0x2808, 0x4300); r.bus.write32(0x280C, 4);
    r.block(0x1000, 7, 0, {0x12, 1, 1, 0x2800});
    r.send(0x1000);
    r.ticks(600);
    CHECK(r.kernel.stopped());
    CHECK(r.kernel.stop_reason().find("not the active title") != std::string::npos);
}

TEST_CASE(es_unimplemented_requests_stop) {
    Rig r;
    r.kernel.add_device(std::make_unique<ios::Es>());
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/es");
    r.block(0x1000, 1, 0, {0x2000, 0});
    CHECK_EQ(r.call(0x1000), 0);
    r.block(0x1000, 7, 0, {0x08, 0, 0, 0x2800}); // launch
    r.send(0x1000);
    r.ticks(600);
    CHECK(r.kernel.stopped());
    CHECK(r.kernel.stop_reason() == "IOS /dev/es: ioctlv 0x00000008 is not emulated");
}

namespace {
struct TempDir {
    std::filesystem::path p;
    explicit TempDir(const char* tag) {
        p = std::filesystem::temp_directory_path() / (std::string("orca_nand_") + tag);
        std::filesystem::remove_all(p);
    }
    ~TempDir() { std::filesystem::remove_all(p); }
};
} // namespace

TEST_CASE(nand_paths_permissions_and_files) {
    TempDir t("unit");
    ios::NandFs fs(t.p);
    std::string error;
    CHECK(fs.load(error));
    using ios::NandFs;
    CHECK(NandFs::valid_path("/"));
    CHECK(NandFs::valid_non_root("/title/00010000/52534245/data"));
    CHECK(!NandFs::valid_non_root("/title/"));
    CHECK(!NandFs::valid_non_root("title"));
    CHECK(!NandFs::valid_non_root("/a//b"));
    CHECK(!NandFs::valid_non_root("/abcdefghijklm"));      // 13 characters
    CHECK(!NandFs::valid_non_root("/" + std::string(63, 'a')));
    const ios::FsModes rw_r_none{3, 1, 0};
    // the root: only the kernel (uid 0) may create
    CHECK_EQ(fs.create(0x1000, 1, "/title", 0, {3, 3, 3}, false), ios::fs_err::kAccess);
    CHECK_EQ(fs.create(0, 0, "/title", 0, {3, 3, 1}, false), 0);
    CHECK_EQ(fs.create(0, 0, "/title", 0, {3, 3, 1}, false), ios::fs_err::kExists);
    CHECK_EQ(fs.create(0, 0, "/x/y", 0, {3, 3, 1}, false), ios::fs_err::kNotFound);
    CHECK_EQ(fs.create(0, 0, "/title/a/b", 0, {3, 3, 1}, false), ios::fs_err::kNotFound);
    CHECK_EQ(fs.create(0, 0, "/1/2/3/4/5/6/7/8/9", 0, {3, 3, 1}, false), ios::fs_err::kTooDeep);
    CHECK_EQ(fs.create(0, 0, "/title/game", 0x10, rw_r_none, false), 0);
    CHECK_EQ(fs.set_metadata(0, "/title/game", 0x1001, 0x31, 0x10, rw_r_none), 0);
    // owner 0x1001 (rw), group 0x31 (r), others none
    CHECK_EQ(fs.create(0x1001, 0x31, "/title/game/save.bin", 0, rw_r_none, true), 0);
    CHECK_EQ(fs.create(0x1002, 0x31, "/title/game/x", 0, rw_r_none, true), ios::fs_err::kAccess); // group: read only
    std::vector<std::string> names;
    CHECK_EQ(fs.list(0x1002, 0x31, "/title/game", names), 0);
    CHECK_EQ(fs.list(0x1003, 0x99, "/title/game", names), ios::fs_err::kAccess);
    // file data
    const s32 h = fs.open(0x1001, 0x31, "/title/game/save.bin", 3);
    CHECK(h >= 0);
    const std::vector<u8> data{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    CHECK_EQ(fs.write(h, data), 10);
    CHECK_EQ(fs.seek(h, 2, 0), 2);
    std::vector<u8> back(4);
    CHECK_EQ(fs.read(h, back), 4);
    CHECK(back == std::vector<u8>({3, 4, 5, 6}));
    CHECK_EQ(fs.seek(h, -1, 2), 9);
    CHECK_EQ(fs.read(h, back), 1);
    CHECK_EQ(fs.read(h, back), 0);
    CHECK_EQ(fs.seek(h, 1, 1), ios::fs_err::kInvalid); // past the end
    u32 size = 0, pos = 0;
    CHECK_EQ(fs.status(h, size, pos), 0);
    CHECK_EQ(size, 10U);
    CHECK_EQ(pos, 10U);
    // in use while open
    CHECK_EQ(fs.remove(0x1001, 0x31, "/title/game/save.bin"), ios::fs_err::kInUse);
    CHECK_EQ(fs.remove(0, 0, "/title/game"), ios::fs_err::kInUse);
    CHECK_EQ(fs.set_metadata(0, "/title/game/save.bin", 0x1002, 0x31, 0, rw_r_none), ios::fs_err::kNotEmpty);
    CHECK_EQ(fs.close(h), 0);
    CHECK_EQ(fs.close(h), ios::fs_err::kInvalid);
    // read-only open cannot write; others have no access
    const s32 r = fs.open(0x1002, 0x31, "/title/game/save.bin", 1);
    CHECK(r >= 0);
    CHECK_EQ(fs.write(r, data), ios::fs_err::kAccess);
    CHECK_EQ(fs.close(r), 0);
    CHECK_EQ(fs.open(0x1002, 0x31, "/title/game/save.bin", 2), ios::fs_err::kAccess);
    CHECK_EQ(fs.open(0x1003, 0x99, "/title/game/save.bin", 1), ios::fs_err::kAccess);
    CHECK_EQ(fs.open(0, 0, "/title/game", 1), ios::fs_err::kInvalid);
    // newest first
    CHECK_EQ(fs.create(0x1001, 0x31, "/title/game/b", 0, rw_r_none, false), 0);
    CHECK_EQ(fs.create(0x1001, 0x31, "/title/game/a", 0, rw_r_none, true), 0);
    CHECK_EQ(fs.list(0x1001, 0x31, "/title/game", names), 0);
    CHECK(names == std::vector<std::string>({"a", "b", "save.bin"}));
    // renames: files keep their name; a directory moves with its content
    CHECK_EQ(fs.rename(0x1001, 0x31, "/title/game/a", "/title/game/c"), ios::fs_err::kInvalid);
    CHECK_EQ(fs.rename(0x1001, 0x31, "/title/game/a", "/title/game/b/a"), 0);
    CHECK_EQ(fs.rename(0x1001, 0x31, "/title/game/b", "/title/game/d"), 0);
    ios::FsMeta m;
    CHECK_EQ(fs.metadata(0x1001, 0x31, "/title/game/d/a", m), 0);
    CHECK(m.is_file);
    CHECK_EQ(m.uid, 0x1001U);
    CHECK_EQ(fs.metadata(0x1001, 0x31, "/title/game/b", m), ios::fs_err::kNotFound);
    u32 clusters = 0, inodes = 0;
    CHECK_EQ(fs.usage("/title/game", clusters, inodes), 0);
    CHECK_EQ(clusters, 1U);   // save.bin (10 bytes); the moved file is empty
    CHECK_EQ(inodes, 4U);     // game, save.bin, d, d/a
    // metadata survives a reload
    ios::NandFs again(t.p);
    CHECK(again.load(error));
    CHECK_EQ(again.metadata(0, 0, "/title/game", m), 0);
    CHECK_EQ(m.uid, 0x1001U);
    CHECK_EQ(m.gid, 0x31U);
    CHECK_EQ(m.attribute, 0x10U);
    CHECK(m.modes.owner == 3 && m.modes.group == 1 && m.modes.other == 0);
    CHECK_EQ(again.list(0, 0, "/title/game", names), 0);
    CHECK(names == std::vector<std::string>({"d", "save.bin"}));
    // delete removes contents
    CHECK_EQ(again.remove(0x1001, 0x31, "/title/game/d"), 0);
    CHECK_EQ(again.metadata(0, 0, "/title/game/d/a", m), ios::fs_err::kNotFound);
    // names with characters the host may not accept
    CHECK_EQ(again.create(0, 0, "/sh:are?", 0, {3, 3, 3}, false), 0);
    CHECK_EQ(again.list(0, 0, "/", names), 0);
    CHECK(std::find(names.begin(), names.end(), "sh:are?") != names.end());
    CHECK(std::filesystem::exists(t.p / "sh%3Aare%3F"));
}

TEST_CASE(fs_device_files_and_isfs_requests) {
    TempDir t("device");
    ios::NandFs nand(t.p);
    std::string error;
    CHECK(nand.load(error));
    CHECK_EQ(nand.create(0, 0, "/shared2", 0, {3, 3, 3}, false), 0);
    Rig r;
    r.kernel.add_device(std::make_unique<ios::FsDevice>(nand));
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    // without credentials: explicit stop
    {
        Rig q;
        q.kernel.add_device(std::make_unique<ios::FsDevice>(nand));
        q.put_string(0x2000, "/dev/fs");
        q.block(0x1000, 1, 0, {0x2000, 0});
        q.send(0x1000);
        q.ticks(600);
        CHECK(q.kernel.stopped());
        CHECK(q.kernel.stop_reason().find("owner/group") != std::string::npos);
    }
    r.kernel.set_ppc_credentials(0x1000, 0x30);
    r.put_string(0x2000, "/dev/fs");
    r.block(0x1000, 1, 0, {0x2000, 0});
    CHECK_EQ(r.call(0x1000), 0);
    // create file /shared2/f.bin (ioctl 9)
    std::vector<u8> params(0x4A, 0);
    const std::string path = "/shared2/f.bin";
    std::copy(path.begin(), path.end(), params.begin() + 6);
    params[70] = 3; params[71] = 3; params[72] = 1;
    write_mem(r, 0x3000, params);
    r.block(0x1000, 6, 0, {9, 0x3000, 0x4A, 0, 0});
    CHECK_EQ(r.call(0x1000), 0);
    r.block(0x1000, 6, 0, {9, 0x3000, 0x4A, 0, 0});
    CHECK_EQ(r.call(0x1000), ios::fs_err::kExists);
    // open it (a NAND path goes to /dev/fs), write, seek, read, file status
    r.put_string(0x2040, path);
    r.block(0x1000, 1, 0, {0x2040, 3});
    CHECK_EQ(r.call(0x1000), 1);
    write_mem(r, 0x4000, {0xDE, 0xAD, 0xBE, 0xEF, 0x01});
    r.block(0x1000, 4, 1, {0x4000, 5});
    CHECK_EQ(r.call(0x1000), 5);
    r.block(0x1000, 5, 1, {1, 0});
    CHECK_EQ(r.call(0x1000), 1);
    r.block(0x1000, 3, 1, {0x4100, 16});
    CHECK_EQ(r.call(0x1000), 4);
    CHECK_EQ(r.mem32(0x4100), 0xADBEEF01U);
    r.block(0x1000, 6, 1, {11, 0, 0, 0x4200, 8});
    CHECK_EQ(r.call(0x1000), 0);
    CHECK_EQ(r.mem32(0x4200), 5U);
    CHECK_EQ(r.mem32(0x4204), 5U);
    // the file system descriptor cannot be read
    r.block(0x1000, 3, 0, {0x4100, 4});
    CHECK_EQ(r.call(0x1000), ios::fs_err::kInvalid);
    r.block(0x1000, 2, 1, {});
    CHECK_EQ(r.call(0x1000), 0);
    // get attributes
    r.put_string(0x3100, path);
    r.block(0x1000, 6, 0, {6, 0x3100, 64, 0x3200, 0x4A});
    CHECK_EQ(r.call(0x1000), 0);
    CHECK_EQ(r.mem32(0x3200), 0x1000U);
    const auto attr = read_mem(r, 0x3200, 0x4A);
    CHECK_EQ(attr[5], 0x30U);
    CHECK(attr[70] == 3 && attr[71] == 3 && attr[72] == 1);
    // read directory: count, then names
    r.put_string(0x3300, "/shared2");
    r.bus.write32(0x3400, 0);
    CHECK_EQ(ioctlv(r, 4, {{0x3300, 64}}, {{0x3400, 4}}), 0);
    CHECK_EQ(r.mem32(0x3400), 1U);
    r.bus.write32(0x3410, 4);
    CHECK_EQ(ioctlv(r, 4, {{0x3300, 64}, {0x3410, 4}}, {{0x3500, 52}, {0x3420, 4}}), 0);
    CHECK_EQ(r.mem32(0x3420), 1U);
    const auto listed = read_mem(r, 0x3500, 6);
    CHECK(std::string(reinterpret_cast<const char*>(listed.data())) == "f.bin");
    // usage and statistics
    CHECK_EQ(ioctlv(r, 12, {{0x3300, 64}}, {{0x3600, 4}, {0x3604, 4}}), 0);
    CHECK_EQ(r.mem32(0x3600), 1U);
    CHECK_EQ(r.mem32(0x3604), 2U);
    r.block(0x1000, 6, 0, {2, 0, 0, 0x3700, 0x1C});
    CHECK_EQ(r.call(0x1000), 0);
    CHECK_EQ(r.mem32(0x3700), 0x4000U);
    CHECK_EQ(r.mem32(0x3708), 1U);                // used clusters
    CHECK_EQ(r.mem32(0x3704), 0x7BC0U - 1U);      // free clusters
    CHECK_EQ(r.mem32(0x3718), 3U);                // inodes: root, shared2, f.bin
    // rename (files keep their name), delete
    r.put_string(0x3800, path);
    r.put_string(0x3840, "/shared2/g.bin");
    r.block(0x1000, 6, 0, {8, 0x3800, 128, 0, 0});
    CHECK_EQ(r.call(0x1000), ios::fs_err::kInvalid);
    r.block(0x1000, 6, 0, {7, 0x3800, 64, 0, 0});
    CHECK_EQ(r.call(0x1000), 0);
    r.block(0x1000, 1, 0, {0x2040, 1});
    CHECK_EQ(r.call(0x1000), ios::fs_err::kNotFound);
    // format: explicit stop
    r.block(0x1000, 6, 0, {1, 0, 0, 0, 0});
    r.send(0x1000);
    r.ticks(600);
    CHECK(r.kernel.stopped());
}

namespace {
// A Rig with a drive, ES, a NAND and /dev/di over a synthetic Wii disc.
struct DiRig {
    TempDir dir{"di"};
    ios::NandFs nand{dir.p};
    Rig r;
    hw::DiscInterface drive{r.bus, r.sched, r.pi};
    ios::Es* es = nullptr;
    ios::DiDevice* di = nullptr;
    std::vector<u8> image;
    DiRig() {
        std::string error;
        CHECK(nand.load(error));
        drive.set_wii_mode();
        auto e = std::make_unique<ios::Es>();
        es = e.get();
        r.kernel.add_device(std::move(e));
        auto d = std::make_unique<ios::DiDevice>(drive, *es, &nand, test_wii::test_keys());
        di = d.get();
        r.kernel.add_device(std::move(d));
        image = test_wii::build_disc(test_wii::plain_data(3));
        drive.insert(std::make_unique<disc::MemoryDisc>(image));
        r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
        r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
        r.put_string(0x2000, "/dev/di");
        r.block(0x1000, 1, 0, {0x2000, 0});
        CHECK_EQ(r.call(0x1000), 0);
    }
    // ioctl on fd 0 with the 0x20-byte input at 0x2100
    s32 ioctl(u32 cmd, std::vector<u32> words, u32 out, u32 out_size) {
        for (u32 i = 0; i < 8; ++i) r.bus.write32(0x2100 + 4 * i, i == 0 ? cmd << 24 : (i - 1 < words.size() ? words[i - 1] : 0));
        r.block(0x1000, 6, 0, {cmd, 0x2100, 0x20, out, out_size});
        return r.call(0x1000);
    }
};
} // namespace

TEST_CASE(di_device_drive_commands_partitions_and_reads) {
    DiRig t;
    // a partition read before opening one: refused
    CHECK_EQ(t.ioctl(0x71, {0x20, 0}, 0x8000, 0x20), ios::di_res::kSecurity);
    // open the partition (ioctlv 0x8B): TMD out, ES result 0
    t.r.bus.write32(0x2200, 0x8B000000U);
    t.r.bus.write32(0x2204, 0x50000U >> 2);
    CHECK_EQ(ioctlv(t.r, 0x8B, {{0x2200, 0x20}, {0, 0}, {0, 0}}, {{0x9000, 0x49E4}, {0x2300, 4}}), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x2300), 0U);
    CHECK_EQ(t.r.mem32(0x9000 + 0x18C), 0x00010000U);
    CHECK(t.es->active_title().has_value());
    const auto cred = t.r.kernel.ppc_credentials();
    CHECK(cred.has_value());
    CHECK_EQ(cred->first, 0x1000U);   // first title in uid.sys
    CHECK_EQ(cred->second, 0x3031U);  // group from the TMD
    // the drive has not read the disc ID yet: drive error
    CHECK_EQ(t.ioctl(0x71, {0x20, 0}, 0x8000, 0x20), ios::di_res::kDriveError);
    CHECK_EQ(t.ioctl(0xE0, {}, 0x8100, 4), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8100), 0x05020401U); // state "disc ID not read" (6 - 1), error 0x20401
    // read the disc ID (DMA into MEM2), then the partition
    CHECK_EQ(t.ioctl(0x70, {}, 0x10001000U, 0x20), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x10001000U), 0x52534245U); // "RSBE"
    CHECK_EQ(t.ioctl(0x71, {0x40, 0}, 0x8000, 0x40), ios::di_res::kSuccess);
    const auto plain = test_wii::plain_data(3);
    CHECK(read_mem(t.r, 0x8000, 0x40) == std::vector<u8>(plain.begin(), plain.begin() + 0x40));
    // across a cluster boundary, offset in words
    const u32 off = 0x7C00 - 0x10;
    CHECK_EQ(t.ioctl(0x71, {0x20, off >> 2}, 0x8200, 0x20), ios::di_res::kSuccess);
    CHECK(read_mem(t.r, 0x8200, 0x20) == std::vector<u8>(plain.begin() + off, plain.begin() + off + 0x20));
    CHECK_EQ(t.ioctl(0x83, {}, 0x8100, 4), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8100), off >> 2); // IOS keeps the offset as "length"
    CHECK_EQ(t.ioctl(0x71, {0x40, 0}, 0x8000, 0x20), ios::di_res::kSecurity); // buffer too small
    // unencrypted reads: system area only
    CHECK_EQ(t.ioctl(0x8D, {0x20, 0x40000U >> 2}, 0x8300, 0x20), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8300), 1U);                 // one partition in group 0
    CHECK_EQ(t.ioctl(0x8D, {0x20, 0x50000U >> 2}, 0x8300, 0x20), ios::di_res::kSecurity);
    // out-of-bounds test area past the first layer: the drive reports an error
    CHECK_EQ(t.ioctl(0x8D, {0x20, 0x460A0000U}, 0x8300, 0x20), ios::di_res::kDriveError);
    CHECK_EQ(t.ioctl(0xE0, {}, 0x8100, 4), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8100) & 0xFFFFFFU, 0x052100U); // block out of bounds
    // inquiry, cover status, registers
    CHECK_EQ(t.ioctl(0x12, {}, 0x8400, 0x20), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8400), 0x00000002U);
    CHECK_EQ(t.ioctl(0x88, {}, 0x8100, 4), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8100), 2U);
    CHECK_EQ(t.ioctl(0x7A, {}, 0x8100, 4), ios::di_res::kSuccess);
    CHECK_EQ(t.r.mem32(0x8100), 0U); // cover closed, no interrupt
    CHECK_EQ(t.ioctl(0x12, {}, 0x8410, 0x20), ios::di_res::kBadArgument); // misaligned buffer
    CHECK_EQ(t.ioctl(0x12, {}, 0x8400, 0x10), 0x10);                     // too small: timed out
    // reset: the disc ID must be read again
    CHECK_EQ(t.ioctl(0x8A, {1}, 0, 0), ios::di_res::kSuccess);
    CHECK_EQ(t.ioctl(0x71, {0x20, 0}, 0x8000, 0x20), ios::di_res::kDriveError);
    // close the partition: reads refused again
    CHECK_EQ(t.ioctl(0x70, {}, 0x8000, 0x20), ios::di_res::kSuccess);
    CHECK_EQ(t.ioctl(0x8C, {}, 0, 0), ios::di_res::kSuccess);
    CHECK_EQ(t.ioctl(0x71, {0x20, 0}, 0x8000, 0x20), ios::di_res::kSecurity);
    // a second title gets the next uid; the first keeps its own
    std::string error;
    CHECK(ios::uid_for_title(t.nand, 0x0001000041414141ULL, error) == std::optional<u32>(0x1001U));
    CHECK(ios::uid_for_title(t.nand, 0x0001000052534245ULL, error) == std::optional<u32>(0x1000U));
    // DVD-video commands: explicit stop
    t.r.bus.write32(0x2100, 0xD0000000U);
    t.r.block(0x1000, 6, 0, {0xD0, 0x2100, 0x20, 0x8000, 0x20});
    t.r.send(0x1000);
    t.r.ticks(600);
    CHECK(t.r.kernel.stopped());
    CHECK(t.r.kernel.stop_reason().find("0xD0") != std::string::npos);
}

TEST_CASE(di_device_requests_queue_behind_the_drive) {
    DiRig t;
    t.drive.set_latency_cycles(12U * 100000U); // the drive takes 100000 ticks per command
    // two drive commands sent back to back: the second waits for the first
    for (u32 i = 0; i < 8; ++i) t.r.bus.write32(0x2100 + 4 * i, i == 0 ? 0x70000000U : 0);
    for (u32 i = 0; i < 8; ++i) t.r.bus.write32(0x2180 + 4 * i, i == 0 ? 0xE0000000U : 0);
    t.r.block(0x1100, 6, 0, {0x70, 0x2100, 0x20, 0x8000, 0x20});
    t.r.block(0x1200, 6, 0, {0xE0, 0x2180, 0x20, 0x8100, 4});
    t.r.send(0x1100);
    CHECK(t.r.service(2000) == std::optional<u32>(0U));
    t.r.send(0x1200);
    CHECK(t.r.service(2000) == std::optional<u32>(0U));
    CHECK_EQ(t.r.mem32(0x1100), 6U);                         // not answered yet
    CHECK(t.r.service(200000) == std::optional<u32>(0x1100U));
    CHECK_EQ(t.r.mem32(0x1200), 6U);                         // B starts only now
    CHECK(t.r.service(200000) == std::optional<u32>(0x1200U));
    CHECK_EQ(t.r.mem32(0x1104), 1U);
    CHECK_EQ(t.r.mem32(0x1204), 1U);
    CHECK_EQ(t.r.mem32(0x8000), 0x52534245U);
    CHECK_EQ(t.r.mem32(0x8100), 0x00000000U); // drive ready (disc ID read), no error
}

TEST_CASE(ios_memory_values_for_known_versions) {
    CHECK(!ios::ios_memory_values(5).has_value());
    const auto v36 = ios::ios_memory_values(36);
    CHECK(v36.has_value());
    CHECK_EQ(v36->version >> 16, 36U);
    mem::Bus bus;
    mem::map_wii_ram(bus);
    bus.write32(0x3FFC, 0xFFFFFFFFU);
    CHECK(ios::write_ios_memory_values(bus, *v36));
    u32 x = 0;
    bus.read32(0x3FFC, x); CHECK_EQ(x, 0U);
    bus.read32(0x3140, x); CHECK_EQ(x, v36->version);
    bus.read32(0x3120, x); CHECK_EQ(x, 0x93600000U);
    bus.read32(0x3130, x); CHECK_EQ(x, 0x935E0000U);
    bus.read32(0x3134, x); CHECK_EQ(x, 0x93600000U);
    bus.read32(0x3128, x); CHECK_EQ(x, 0x935E0000U);
    bus.read32(0x3148, x); CHECK_EQ(x, 0x93600000U);
    bus.read32(0x28, x); CHECK_EQ(x, 0x01800000U);
    const auto v9 = ios::ios_memory_values(9);
    CHECK(v9.has_value());
    CHECK_EQ(v9->mem2_end, 0x93400000U);
    CHECK_EQ(v9->reserved_begin, 0xDEADBEEFU);
}

#ifdef ORCA_PROGRAMS_DIR
namespace {
std::vector<u8> read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
void put_be32(std::vector<u8>& d, std::size_t at, u32 v) { for (unsigned k = 0; k < 4; ++k) d[at + k] = static_cast<u8>(v >> (24U - 8U * k)); }
} // namespace

TEST_CASE(wii_disc_boot_runs_the_integer_suite) {
    const auto app = read_file(ORCA_PROGRAMS_DIR "/apploader_wii.bin");
    const auto dol = read_file(ORCA_PROGRAMS_DIR "/integer_suite.dol");
    CHECK(!app.empty() && !dol.empty());
    // partition data: header (DOL at 0x20000, in words at 0x420), apploader at 0x2440, DOL
    const u32 dol_at = 0x20000;
    const u64 bytes = dol_at + dol.size();
    std::vector<u8> plain = test_wii::plain_data((bytes + disc::kWiiClusterData - 1U) / disc::kWiiClusterData);
    std::fill(plain.begin() + 0x20, plain.begin() + 0x2440 + 0x20 + static_cast<std::ptrdiff_t>(app.size()), u8{0});
    put_be32(plain, 0x420, dol_at >> 2);
    const char date[] = "2026/11/23";
    std::copy(date, date + sizeof date, plain.begin() + 0x2440);
    put_be32(plain, 0x2450, 0x81200000U);
    put_be32(plain, 0x2454, static_cast<u32>((app.size() + 31U) & ~std::size_t{31}));
    put_be32(plain, 0x2458, 0);
    std::copy(app.begin(), app.end(), plain.begin() + 0x2460);
    std::copy(dol.begin(), dol.end(), plain.begin() + dol_at);
    const auto image = test_wii::build_disc(plain);
    // NAND with the system menu's setting.txt
    TempDir dir("boot");
    {
        ios::NandFs nand(dir.p);
        std::string error;
        CHECK(nand.load(error));
        for (const char* d : {"/title", "/title/00000001", "/title/00000001/00000002", "/title/00000001/00000002/data"})
            CHECK_EQ(nand.create(0, 0, d, 0, {3, 3, 1}, false), 0);
        CHECK_EQ(nand.create(0, 0, "/title/00000001/00000002/data/setting.txt", 0, {3, 3, 1}, true), 0);
        const s32 h = nand.open(0, 0, "/title/00000001/00000002/data/setting.txt", 3);
        std::vector<u8> st(0x100);
        for (std::size_t i = 0; i < st.size(); ++i) st[i] = static_cast<u8>(i ^ 0x5A);
        CHECK_EQ(nand.write(h, st), 0x100);
        nand.close(h);
    }
    system::SystemConfig cfg;
    cfg.console = system::Console::Wii;
    cfg.nand_root = dir.p.string();
    cfg.wii_keys = test_wii::test_keys();
    system::System sys(cfg);
    CHECK(sys.nand() != nullptr);
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    sys.insert_disc(std::make_unique<disc::MemoryDisc>(image));
    std::string error;
    const bool booted = sys.boot_wii_disc(error);
    if (!booted) std::fprintf(stderr, "    boot_wii_disc: %s\n", error.c_str());
    CHECK(booted);
    if (!booted) return;
    // what the boot left below the program (this test DOL is linked at 0x80003100,
    // so the IOS and system menu values from 0x3100 on are overwritten by it)
    u32 x = 0;
    sys.bus().read32(0x00, x); CHECK_EQ(x, 0x52534245U);  // disc ID
    sys.bus().read32(0x18, x); CHECK_EQ(x, disc::kWiiMagic);
    sys.bus().read32(0x20, x); CHECK_EQ(x, 0x0D15EA5EU);
    sys.bus().read32(0x2C, x); CHECK_EQ(x, 0x23U);
    sys.bus().read32(0xCC, x); CHECK_EQ(x, 0U);           // NTSC region letter 'E'
    sys.bus().read32(0xF8, x); CHECK_EQ(x, 243000000U);
    sys.bus().read32(0x28, x); CHECK_EQ(x, 0x01800000U);
    CHECK((sys.cpu().state().hid4 & ppc::HID4_SBE) != 0U);
    CHECK_EQ(sys.cpu().state().dbat[5].upper, 0xD0001FFFU);
    CHECK(sys.ios()->ppc_credentials().has_value());
    CHECK(sys.bluetooth() != nullptr);
    CHECK_EQ(sys.bluetooth()->remote_count(), 0U); // this NAND has no SYSCONF
    CHECK(sys.bluetooth_error().find("SYSCONF") != std::string::npos);
    CHECK(sys.es()->active_title().has_value());
    ios::FsMeta data_dir;
    CHECK_EQ(sys.nand()->metadata(0, 0, "/title/00010000/52534245/data", data_dir), 0);
    CHECK_EQ(data_dir.uid, sys.ios()->ppc_credentials()->first);
    const auto res = sys.run(4000000000ULL);
    CHECK(res.stop == ppc::StepResult::Halted);
    CHECK(port.exited);
    CHECK_EQ(port.exit_code, 0U);
    std::printf("[ios] integer_suite booted from a Wii disc partition: %zu values, exit %u\n", port.log.size(), port.exit_code);
}

TEST_CASE(wii_disc_boot_failures_are_explicit) {
    system::SystemConfig cfg;
    cfg.console = system::Console::Wii;
    cfg.wii_keys = test_wii::test_keys();
    system::System sys(cfg);
    std::string error;
    CHECK(!sys.boot_wii_disc(error));
    CHECK(error.find("no disc") != std::string::npos);
    sys.insert_disc(std::make_unique<disc::MemoryDisc>(test_wii::build_disc(test_wii::plain_data(2), 0, 5)));
    CHECK(!sys.boot_wii_disc(error));      // no NAND for uid.sys
    CHECK(error.find("NAND") != std::string::npos);
}
#endif

namespace {
// A scripted Bluetooth host: keeps one HCI event buffer and one ACL buffer
// pending, as the Wii's stack does, and records what arrives.
struct BtHost {
    Rig& r;
    std::set<u32> replied;
    std::deque<std::vector<u8>> events, acl;
    static constexpr u32 kEvBlock = 0x6000, kEvBuf = 0x6400, kAclBlock = 0x6100, kAclBuf = 0x6800;
    u32 scratch = 0x7000; // request blocks and their data
    explicit BtHost(Rig& rig) : r(rig) {}

    void on_reply(u32 a) {
        replied.insert(a);
        const auto n = static_cast<std::size_t>(static_cast<s32>(r.mem32(a + 4)));
        if (a == kEvBlock) { events.push_back(read_mem(r, kEvBuf, n)); post(true); }
        if (a == kAclBlock) { acl.push_back(read_mem(r, kAclBuf, n)); post(false); }
    }
    void submit(u32 a) {
        r.send(a);
        for (;;) {
            const auto x = r.service(20000000);
            CHECK(x.has_value());
            if (!x) return;
            if (*x == 0U) return;
            on_reply(*x);
        }
    }
    void pump(u64 ticks = 2000) {
        for (;;) {
            const auto x = r.service(ticks);
            if (!x) return;
            if (*x != 0U) on_reply(*x);
        }
    }
    void wait(u32 a) {
        while (replied.count(a) == 0U) {
            const auto x = r.service(20000000);
            CHECK(x.has_value());
            if (!x) return;
            if (*x != 0U) on_reply(*x);
        }
    }
    // ioctlv on fd 0 with the given vectors, using a fresh block and table
    u32 ioctlv_block(u32 n, std::vector<std::pair<u32, u32>> in, std::vector<std::pair<u32, u32>> io, u32 block) {
        const u32 table = block + 0x40;
        u32 i = 0;
        for (auto& v : in) { r.bus.write32(table + 8 * i, v.first); r.bus.write32(table + 8 * i + 4, v.second); ++i; }
        for (auto& v : io) { r.bus.write32(table + 8 * i, v.first); r.bus.write32(table + 8 * i + 4, v.second); ++i; }
        r.block(block, 7, 0, {n, static_cast<u32>(in.size()), static_cast<u32>(io.size()), table});
        return block;
    }
    void post(bool event) {
        const u32 blk = event ? kEvBlock : kAclBlock;
        const u32 ep = blk + 0x200, len = blk + 0x204;
        r.bus.write8(ep, event ? 0x81 : 0x82);
        r.bus.write16(len, 0x400);
        ioctlv_block(event ? 2 : 1, {{ep, 1}, {len, 2}}, {{event ? kEvBuf : kAclBuf, 0x400}}, blk);
        submit(blk);
    }
    // HCI command; returns the control transfer's result
    s32 hci(u16 op, std::vector<u8> params) {
        const u32 blk = next(), data = blk + 0x100;
        std::vector<u8> cmd{static_cast<u8>(op), static_cast<u8>(op >> 8), static_cast<u8>(params.size())};
        cmd.insert(cmd.end(), params.begin(), params.end());
        write_mem(r, data, cmd);
        const u32 f = blk + 0x200;
        r.bus.write8(f, 0x20);
        r.bus.write8(f + 1, 0);
        r.bus.write16(f + 2, 0);
        r.bus.write16(f + 4, 0);
        r.bus.write8(f + 6, static_cast<u8>(cmd.size()));
        r.bus.write8(f + 7, 0);
        ioctlv_block(0, {{f, 1}, {f + 1, 1}, {f + 2, 2}, {f + 4, 2}, {f + 6, 2}, {f + 8, 1}}, {{data, static_cast<u32>(cmd.size())}}, blk);
        submit(blk);
        wait(blk);
        pump();
        return static_cast<s32>(r.mem32(blk + 4));
    }
    // L2CAP packet on a connection
    s32 send_acl(u16 handle, u16 cid, std::vector<u8> payload) {
        const u32 blk = next(), data = blk + 0x100;
        std::vector<u8> pkt{static_cast<u8>(handle), static_cast<u8>(0x20 | handle >> 8)};
        const u16 l2 = static_cast<u16>(payload.size() + 4);
        pkt.insert(pkt.end(), {static_cast<u8>(l2), static_cast<u8>(l2 >> 8), static_cast<u8>(payload.size()),
                               static_cast<u8>(payload.size() >> 8), static_cast<u8>(cid), static_cast<u8>(cid >> 8)});
        pkt.insert(pkt.end(), payload.begin(), payload.end());
        write_mem(r, data, pkt);
        const u32 f = blk + 0x200;
        r.bus.write8(f, 0x02);
        r.bus.write16(f + 2, static_cast<u16>(pkt.size()));
        ioctlv_block(1, {{f, 1}, {f + 2, 2}}, {{data, static_cast<u32>(pkt.size())}}, blk);
        submit(blk);
        wait(blk);
        pump();
        return static_cast<s32>(r.mem32(blk + 4));
    }
    u32 next() { const u32 b = scratch; scratch += 0x400; if (scratch >= 0xF000) scratch = 0x7000; return b; }
    std::optional<std::vector<u8>> event_with(u8 code) {
        for (auto it = events.begin(); it != events.end(); ++it)
            if ((*it)[0] == code) { auto e = *it; events.erase(it); return e; }
        return std::nullopt;
    }
    // next ACL payload (L2CAP payload after the 4-byte L2CAP header), with its CID
    std::optional<std::pair<u16, std::vector<u8>>> next_l2cap() {
        if (acl.empty()) return std::nullopt;
        auto a = acl.front();
        acl.pop_front();
        const u16 cid = static_cast<u16>(a[6] | a[7] << 8);
        return std::pair<u16, std::vector<u8>>{cid, std::vector<u8>(a.begin() + 8, a.end())};
    }
};

std::vector<u8> sysconf_with_remotes(std::vector<ios::BdAddr> remotes) {
    // SCv0, 1 entry at 0x0C: BT.DINF big array of 0x461 bytes; end marker
    std::vector<u8> f(0x4000, 0);
    f[0] = 'S'; f[1] = 'C'; f[2] = 'v'; f[3] = '0';
    f[5] = 1;
    f[6] = 0; f[7] = 0x0C;
    f[0x0C] = static_cast<u8>(1 << 5 | (7 - 1));
    std::copy_n("BT.DINF", 7, f.begin() + 0x0D);
    f[0x14] = 0x04; f[0x15] = 0x60; // 0x461 - 1
    const std::size_t data = 0x16;
    f[data] = static_cast<u8>(remotes.size());
    for (std::size_t i = 0; i < remotes.size(); ++i) {
        std::reverse_copy(remotes[i].begin(), remotes[i].end(), f.begin() + static_cast<std::ptrdiff_t>(data + 1 + 10 * 70 + i * 70));
    }
    std::copy_n("SCed", 4, f.end() - 4);
    return f;
}
} // namespace

TEST_CASE(sysconf_paired_remotes) {
    const ios::BdAddr a{1, 2, 3, 4, 5, 6}, b{0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    std::string error;
    const auto got = ios::paired_remotes_from_sysconf(sysconf_with_remotes({a, b}), error);
    CHECK(got == std::vector<ios::BdAddr>({a, b}));
    CHECK(ios::paired_remotes_from_sysconf(sysconf_with_remotes({}), error).empty());
    CHECK(error.find("no active remote") != std::string::npos);
    std::vector<u8> bad(16, 0);
    CHECK(ios::paired_remotes_from_sysconf(bad, error).empty());
    CHECK(error.find("SCv0") != std::string::npos);
}

TEST_CASE(bluetooth_hci_connection_l2cap_and_hid) {
    Rig r;
    const ios::BdAddr ctrl{0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const ios::BdAddr remote0{0x10, 0x20, 0x30, 0x40, 0x50, 0x60}, remote1{0x11, 0x21, 0x31, 0x41, 0x51, 0x61};
    auto dev = std::make_unique<ios::BluetoothDevice>(ctrl, std::vector<ios::BdAddr>{remote0, remote1});
    ios::BluetoothDevice* bt = dev.get();
    r.kernel.add_device(std::move(dev));
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/usb/oh1/57e/305");
    r.block(0x1000, 1, 0, {0x2000, 0});
    CHECK_EQ(r.call(0x1000), 0);
    BtHost h(r);
    h.post(true);
    h.post(false);
    // controller setup
    CHECK_EQ(h.hci(0x0C03, {}), 3);
    auto e = h.event_with(0x0E);
    CHECK(e && *e == std::vector<u8>({0x0E, 4, 1, 0x03, 0x0C, 0}));
    CHECK_EQ(h.hci(0x1005, {}), 3);
    e = h.event_with(0x0E);
    CHECK(e && *e == std::vector<u8>({0x0E, 11, 1, 0x05, 0x10, 0, 0x53, 0x01, 64, 10, 0, 0, 0}));
    h.hci(0x1009, {});
    e = h.event_with(0x0E);
    CHECK(e && std::vector<u8>(e->begin() + 6, e->end()) == std::vector<u8>(ctrl.begin(), ctrl.end()));
    h.hci(0x0C0D, {0, 0, 0, 0, 0, 0, 1});
    auto keys = h.event_with(0x15);
    CHECK(keys.has_value() && (*keys)[2] == 2U && keys->size() == 3U + 2U * 22U);
    CHECK(keys && std::equal(remote1.begin(), remote1.end(), keys->begin() + 3 + 22));
    e = h.event_with(0x0E);
    CHECK(e && (*e)[8] == 2U); // two keys read
    // no page scan yet: a remote cannot connect
    CHECK(!bt->connect(0));
    h.hci(0x0C1A, {0x02});
    h.event_with(0x0E);
    CHECK(bt->connect(0));
    h.pump();
    e = h.event_with(0x04);
    CHECK(e && std::equal(remote0.begin(), remote0.end(), e->begin() + 2) && (*e)[8] == 0x04 && (*e)[9] == 0x25);
    // accept (become master): status, role change, connection complete
    std::vector<u8> acc(remote0.begin(), remote0.end());
    acc.push_back(0);
    h.hci(0x0409, acc);
    CHECK(h.event_with(0x0F).has_value());
    CHECK(h.event_with(0x12).has_value());
    e = h.event_with(0x03);
    CHECK(e && (*e)[2] == 0 && (*e)[3] == 0x00 && (*e)[4] == 0x01);
    CHECK(bt->remote_connected(0));
    // authentication and remote information
    h.hci(0x0411, {0x00, 0x01});
    e = h.event_with(0x06);
    CHECK(e && (*e)[2] == 0);
    h.hci(0x041D, {0x00, 0x01});
    e = h.event_with(0x0C);
    CHECK(e && (*e)[5] == 0x02);
    // the remote opens HID control: L2CAP connection request
    bt->tick();
    h.pump();
    auto l = h.next_l2cap();
    CHECK(l && l->first == 1U && l->second[0] == 0x02);
    CHECK(l && l->second[4] == 0x11 && l->second[6] == 0x40);
    const u8 ident = l ? l->second[1] : 0;
    // host: connection response (our cid 0x70), then its configuration request
    CHECK_EQ(h.send_acl(0x100, 1, {0x03, ident, 8, 0, 0x70, 0, 0x40, 0, 0, 0, 0, 0}), 20); // ACL + L2CAP headers + 12
    CHECK(h.event_with(0x13).has_value()); // completed packets
    l = h.next_l2cap();                    // remote's configuration request (MTU 185)
    CHECK(l && l->second[0] == 0x04 && l->second[4] == 0x70 && l->second[8] == 0x01 && l->second[10] == 185);
    const u8 cfg_ident = l ? l->second[1] : 0;
    h.send_acl(0x100, 1, {0x05, cfg_ident, 6, 0, 0x40, 0, 0, 0, 0, 0});
    h.send_acl(0x100, 1, {0x04, 9, 8, 0, 0x40, 0, 0, 0, 0x01, 0x02, 0x80, 0x02});
    l = h.next_l2cap();
    CHECK(l && l->second[0] == 0x05 && l->second[1] == 9 && l->second[4] == 0x70 && l->second[8] == 0 && l->second[10] == 0x01);
    // then HID interrupt (our cid 0x71)
    bt->tick();
    h.pump();
    l = h.next_l2cap();
    CHECK(l && l->second[0] == 0x02 && l->second[4] == 0x13 && l->second[6] == 0x41);
    h.send_acl(0x100, 1, {0x03, l ? l->second[1] : u8{0}, 8, 0, 0x71, 0, 0x41, 0, 0, 0, 0, 0});
    l = h.next_l2cap();
    h.send_acl(0x100, 1, {0x05, l ? l->second[1] : u8{0}, 6, 0, 0x41, 0, 0, 0, 0, 0});
    h.send_acl(0x100, 1, {0x04, 10, 4, 0, 0x41, 0, 0, 0});
    l = h.next_l2cap();
    CHECK(l && l->second[0] == 0x05);
    CHECK(bt->remote_ready(0));
    // HID: first report period sends the core buttons on the interrupt channel
    wiimote::InputState in;
    in.buttons = wiimote::button::kA;
    bt->set_input(0, in);
    bt->tick();
    h.pump();
    l = h.next_l2cap();
    CHECK(l && l->first == 0x71 && l->second == std::vector<u8>({0xA1, 0x30, 0x00, 0x08}));
    // SET_REPORT on the control channel: LEDs with acknowledge -> handshake, then the ack report
    h.send_acl(0x100, 0x40, {0x52, 0x11, 0x12});
    l = h.next_l2cap();
    CHECK(l && l->first == 0x70 && l->second == std::vector<u8>({0x00}));
    CHECK_EQ(bt->remote(0).leds(), 1U);
    bt->tick();
    h.pump();
    l = h.next_l2cap();
    CHECK(l && l->first == 0x71 && l->second == std::vector<u8>({0xA1, 0x22, 0x00, 0x08, 0x11, 0x00}));
    // DATA on the interrupt channel: continuous core reports
    h.send_acl(0x100, 0x41, {0xA2, 0x12, 0x04, 0x30});
    bt->tick();
    bt->tick();
    h.pump();
    CHECK_EQ(h.acl.size(), 2U);
    h.acl.clear();
    // the 200 Hz tick runs by itself
    r.ticks(ios::BluetoothDevice::kUpdateTicks * 3 + 10);
    h.pump();
    CHECK(h.acl.size() >= 3U);
    // disconnect
    h.hci(0x0406, {0x00, 0x01, 0x13});
    e = h.event_with(0x05);
    CHECK(e && (*e)[2] == 0 && (*e)[5] == 0x16);
    CHECK(!bt->remote_connected(0));
    // SDP is not emulated: explicit stop
    CHECK(bt->connect(1));
    h.pump();
    std::vector<u8> acc1(remote1.begin(), remote1.end());
    acc1.push_back(1);
    h.hci(0x0409, acc1);
    h.events.clear();
    h.acl.clear();
    const u32 blk = h.next(), data = blk + 0x100;
    write_mem(r, data, {0x01, 0x21, 12, 0, 8, 0, 0x01, 0, 0x02, 0x50, 4, 0, 0x01, 0, 0x40, 0}); // connect to PSM 1
    r.bus.write8(blk + 0x200, 0x02);
    r.bus.write16(blk + 0x202, 16);
    h.ioctlv_block(1, {{blk + 0x200, 1}, {blk + 0x202, 2}}, {{data, 16}}, blk);
    r.send(blk);
    r.ticks(600);
    CHECK(r.kernel.stopped());
    CHECK(r.kernel.stop_reason().find("SDP") != std::string::npos);
}

TEST_CASE(bluetooth_unknown_hci_command_stops) {
    Rig r;
    r.kernel.add_device(std::make_unique<ios::BluetoothDevice>(ios::BdAddr{}, std::vector<ios::BdAddr>{}));
    r.set(PPC_IRQMASK, IRQ_IPC_BROADWAY);
    r.set(PPCCTRL, CTRL_IY1 | CTRL_IY2);
    r.put_string(0x2000, "/dev/usb/oh1/57e/305");
    r.block(0x1000, 1, 0, {0x2000, 0});
    CHECK_EQ(r.call(0x1000), 0);
    BtHost h(r);
    const u32 blk = h.next(), data = blk + 0x100;
    write_mem(r, data, {0x34, 0x12, 0});
    const u32 f = blk + 0x200;
    r.bus.write8(f + 6, 3);
    h.ioctlv_block(0, {{f, 1}, {f + 1, 1}, {f + 2, 2}, {f + 4, 2}, {f + 6, 2}, {f + 8, 1}}, {{data, 3}}, blk);
    r.send(blk);
    r.ticks(600);
    CHECK(r.kernel.stopped());
    CHECK(r.kernel.stop_reason().find("0x1234") != std::string::npos);
}

TEST_CASE(nand_setup_sysconf_setting_and_system_remotes) {
    ios::NandSetup cfg;
    cfg.region = ios::WiiRegion::Europe;
    cfg.serial = "123456789";
    cfg.remotes = 2;
    const auto sc = ios::make_sysconf(cfg);
    CHECK_EQ(sc.size(), 0x4000U);
    CHECK(sc[0] == 'S' && sc[3] == '0' && sc[0x3FFC] == 'S' && sc[0x3FFF] == 'd');
    std::string error;
    const auto remotes = ios::paired_remotes_from_sysconf(sc, error);
    CHECK(remotes == std::vector<ios::BdAddr>({ios::emulated_remote_address(0), ios::emulated_remote_address(1)}));
    const auto st = ios::make_setting_txt(cfg);
    CHECK_EQ(st.size(), 0x100U);
    const std::string text = ios::decrypt_setting_txt(st);
    CHECK(text.find("AREA=EUR\r\n") == 0);
    CHECK(text.find("VIDEO=PAL\r\n") != std::string::npos);
    CHECK(text.find("SERNO=123456789\r\n") != std::string::npos);
    // a NAND from nothing, then a Wii System sees two remotes
    TempDir dir("setup");
    {
        ios::NandFs nand(dir.p);
        CHECK(nand.load(error));
        CHECK(ios::create_nand(nand, cfg, error));
        CHECK(ios::create_nand(nand, cfg, error)); // a second run keeps the files
        ios::FsMeta m;
        CHECK_EQ(nand.metadata(0, 0, "/shared2/sys/SYSCONF", m), 0);
        CHECK_EQ(m.uid, 0x1000U);
        CHECK_EQ(m.size, 0x4000U);
        CHECK_EQ(nand.metadata(0, 0, "/title/00000001/00000002/data/setting.txt", m), 0);
        CHECK_EQ(m.size, 0x100U);
    }
    system::SystemConfig sys_cfg;
    sys_cfg.console = system::Console::Wii;
    sys_cfg.nand_root = dir.p.string();
    system::System sys(sys_cfg);
    CHECK(sys.bluetooth() != nullptr);
    CHECK_EQ(sys.bluetooth()->remote_count(), 2U);
}

int main() { return orca::test::run_all("ios"); }
