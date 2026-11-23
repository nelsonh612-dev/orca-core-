// Wii: Hollywood IPC registers and the IOS kernel (request decoding, the
// acknowledge/reply handshake, descriptors, ordering, explicit stops).
#include "orca/hw/hollywood_ipc.hpp"
#include "orca/ios/kernel.hpp"
#include "orca/memory/console_maps.hpp"
#include "orca/system/system.hpp"
#include "test_framework.hpp"

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
    ios::Result open(ios::Kernel&, const ios::Request&, u32 mode) override { return ios::Result::reply(mode == 7 ? -1 : 0, 300); }
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
        for (u64 t = 0; t < limit && !line; ++t) ticks(1);
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
        CHECK(service(100000) == std::optional<u32>(a)); // reply
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

int main() { return orca::test::run_all("ios"); }
