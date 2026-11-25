// DSP HLE: the AX ucode (command lists, voices through the shared accelerator,
// resampling, envelopes, mixing, updates, loops, output) and ucode switching.
// Expected values are computed here from the documented formulas; the real AX
// ucode is not available to compare with.
#include "orca/dsp/hle.hpp"
#include "orca/hw/dsp_interface.hpp"
#include "test_framework.hpp"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace orca;
using namespace orca::dsp;

namespace {

constexpr u32 kAx = 0x07F88145U; // a GameCube AX version (Dolphin's list)

struct FakeHost final : hle::Host, DspHost {
    std::vector<u8> ram = std::vector<u8>(0x400000, 0), aram_ = std::vector<u8>(0x100000, 0);
    DspCore core_{*this};
    unsigned irqs = 0;
    std::vector<u64> irq_delays;
    bool ram_read(u32 a, std::span<u8> out) override {
        if (a + out.size() > ram.size()) return false;
        std::copy_n(ram.begin() + a, out.size(), out.begin());
        return true;
    }
    bool ram_write(u32 a, std::span<const u8> in) override {
        if (a + in.size() > ram.size()) return false;
        std::copy(in.begin(), in.end(), ram.begin() + a);
        return true;
    }
    void interrupt_cpu(u64 c) override { ++irqs; irq_delays.push_back(c); }
    std::vector<u8>& aram() override { return aram_; }
    DspCore& core() override { return core_; }
    // DspHost (accelerator)
    bool dsp_dma_read(u32, std::span<u8>) override { return false; }
    bool dsp_dma_write(u32, std::span<const u8>) override { return false; }
    void dsp_interrupt_cpu() override {}
    void dsp_halted() override {}
    bool dsp_aram_read8(u32 a, u8& v) override { if (a >= aram_.size()) return false; v = aram_[a]; return true; }
    bool dsp_aram_write16(u32, u16) override { return false; }
    void w16(u32 a, u16 v) { ram[a] = static_cast<u8>(v >> 8); ram[a + 1] = static_cast<u8>(v); }
    void w32(u32 a, u32 v) { w16(a, static_cast<u16>(v >> 16)); w16(a + 2, static_cast<u16>(v)); }
    u16 r16(u32 a) const { return static_cast<u16>(ram[a] << 8 | ram[a + 1]); }
    s32 r32s(u32 a) const { return static_cast<s32>(u32{r16(a)} << 16 | r16(a + 2)); }
};

struct Rig {
    FakeHost host;
    hle::Dsp dsp{host, false};
    Rig() {
        dsp.reset(false);
        dsp.set_halted(false);
        (void)read_mail();             // 0x8071FEED
        dsp.switch_to(kAx);
    }
    u32 read_mail() { const u32 hi = dsp.cpu_mailbox_high(); return hi << 16 | dsp.cpu_mailbox_low(); }
    void send(u32 m) { dsp.cpu_write_high(static_cast<u16>(m >> 16)); dsp.cpu_write_low(static_cast<u16>(m)); }
    // writes the list at 0x10000 and sends it
    void run(const std::vector<u16>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) host.w16(0x10000U + 2U * static_cast<u32>(i), list[i]);
        send(0xBABE0000U | static_cast<u32>(list.size()));
        send(0x00010000U);
    }
};

std::vector<u16> addr(u32 a) { return {static_cast<u16>(a >> 16), static_cast<u16>(a)}; }
template <class... L> std::vector<u16> cat(L... l) { std::vector<u16> r; (r.insert(r.end(), l.begin(), l.end()), ...); return r; }

// PB word offsets (newer GameCube layout)
enum : u32 { NEXT = 0, SRC_TYPE = 4, MIXER_CONTROL = 6, RUNNING = 7, IS_STREAM = 8, MIX = 9, NUM_UPD = 34, UPD = 39,
             DPOP = 41, VOL = 50, VOL_DELTA = 51, LOOPING = 55, FORMAT = 56, LOOP = 57, END = 59, CUR = 61, GAIN = 79,
             RATIO = 83, FRAC = 85, LAST = 86, LPF_ON = 93, LPF_YN1 = 94, LPF_A0 = 95, LPF_B0 = 96, LOOP_COUNTER = 97 };
void pbw(FakeHost& h, u32 pb, u32 word, u16 v) { h.w16(pb + 2U * word, v); }
u16 pbr(const FakeHost& h, u32 pb, u32 word) { return h.r16(pb + 2U * word); }

// A PCM16 voice of `n` samples at ARAM byte `ab`, playing at "nearest" (no conversion)
void pcm_voice(FakeHost& h, u32 pb, u32 ab, u32 n, u16 mixer_control) {
    for (u32 w = 0; w < 122; ++w) pbw(h, pb, w, 0);
    pbw(h, pb, RUNNING, 1);
    pbw(h, pb, SRC_TYPE, 2);
    pbw(h, pb, MIXER_CONTROL, mixer_control);
    pbw(h, pb, FORMAT, 0x000A);              // PCM16
    pbw(h, pb, GAIN, 0x0800);                // gain scale 0: x * 0x800 >> 11 = x
    const u32 start = ab / 2U, end = start + n - 1U;
    pbw(h, pb, LOOP, static_cast<u16>(start >> 16)); pbw(h, pb, LOOP + 1, static_cast<u16>(start));
    pbw(h, pb, END, static_cast<u16>(end >> 16)); pbw(h, pb, END + 1, static_cast<u16>(end));
    pbw(h, pb, CUR, static_cast<u16>(start >> 16)); pbw(h, pb, CUR + 1, static_cast<u16>(start));
    pbw(h, pb, VOL, 0x7FFF);
    pbw(h, pb, MIX + 0, 0x7FFF);             // main left volume
    pbw(h, pb, MIX + 2, 0x4000);             // main right volume
}

s16 sat(s64 v) { return static_cast<s16>(v > 32767 ? 32767 : (v < -32768 ? -32768 : v)); }

} // namespace

TEST_CASE(ax_protocol_and_task_mails) {
    Rig r;
    CHECK_EQ(r.read_mail(), hle::kMailInit);
    // an empty list (END): the CPU gets DSP_YIELD with an interrupt 2500 cycles later
    const unsigned irq0 = r.host.irqs;
    r.run({0x000F});
    CHECK(!r.dsp.failed());
    CHECK_EQ(r.host.irqs, irq0 + 1U);
    CHECK_EQ(r.host.irq_delays.back(), 2500U);
    CHECK_EQ(r.read_mail(), hle::kMailYield);
    r.send(hle::kCpuResume);
    CHECK_EQ(r.read_mail(), hle::kMailResume);
    r.run({0x000F});
    CHECK_EQ(r.read_mail(), hle::kMailYield);
    r.send(0x00000003U);                       // "continue" without the CDD1 prefix: accepted
    r.run({0x000F});
    CHECK(!r.dsp.failed());
    CHECK_EQ(r.read_mail(), hle::kMailYield);
    r.send(hle::kCpuReset);                    // back to the boot ROM
    CHECK_EQ(r.read_mail(), 0x8071FEEDU);
    // unknown command and wrong mail: clear stops
    Rig u;
    (void)u.read_mail();
    u.run({0x0008, 0, 0});
    CHECK(u.dsp.failed());
    CHECK(u.dsp.error().find("0008") != std::string::npos);
    Rig v;
    (void)v.read_mail();
    v.send(0x12345678U);
    CHECK(v.dsp.failed());
}

TEST_CASE(ax_setup_and_output) {
    Rig r;
    (void)r.read_mail();
    // SETUP: main L = 1000 + k, main R = -500, the rest 0; OUTPUT to 0x20000 (surround 0x30000)
    const u32 init = 0x18000;
    for (u32 w = 0; w < 27; ++w) r.host.w16(init + 2U * w, 0);
    r.host.w32(init, 1000); r.host.w16(init + 4, 1);
    r.host.w32(init + 6, static_cast<u32>(-500));
    r.host.w32(init + 12, 40000); r.host.w16(init + 16, 0); // main S
    r.run(cat(std::vector<u16>{0x0000}, addr(init), std::vector<u16>{0x000E}, addr(0x30000), addr(0x20000), std::vector<u16>{0x000F}));
    CHECK(!r.dsp.failed());
    bool ok = true;
    for (u32 k = 0; k < 160; ++k) {
        ok = ok && static_cast<s16>(r.host.r16(0x20000 + 4 * k)) == -500;                       // right first
        ok = ok && static_cast<s16>(r.host.r16(0x20000 + 4 * k + 2)) == static_cast<s16>(1000 + k); // then left
        ok = ok && r.host.r32s(0x30000 + 4 * k) == 40000;                                        // surround, 32-bit
    }
    CHECK(ok);
    // UPLOAD_LRS writes the three main buffers as 32-bit words; SET_LR (0x07) replaces them
    r.host.w32(0x40000, 0x12345678U);
    (void)r.read_mail();                       // DSP_YIELD
    r.send(hle::kCpuContinue);                 // the CPU continues with the next list
    r.run(cat(std::vector<u16>{0x0006}, addr(0x50000), std::vector<u16>{0x0007}, addr(0x40000), std::vector<u16>{0x0006}, addr(0x60000), std::vector<u16>{0x000F}));
    CHECK_EQ(r.host.r32s(0x50000 + 4), 1001);
    CHECK_EQ(r.host.r32s(0x60000), 0x12345678);
    CHECK_EQ(r.host.r32s(0x60000 + 640), 0x12345678);           // right = left
    CHECK_EQ(r.host.r32s(0x60000 + 1280), 0);                   // surround cleared
}

TEST_CASE(ax_pcm_voice_through_the_accelerator) {
    Rig r;
    (void)r.read_mail();
    // 100 samples of PCM16 in ARAM at 0x1000: x(k) = 100 * k - 3000
    for (u32 k = 0; k < 100; ++k) {
        const auto v = static_cast<u16>(static_cast<s16>(100 * static_cast<s32>(k) - 3000));
        r.host.aram_[0x1000 + 2 * k] = static_cast<u8>(v >> 8); r.host.aram_[0x1000 + 2 * k + 1] = static_cast<u8>(v);
    }
    const u32 pb = 0x70000;
    pcm_voice(r.host, pb, 0x1000, 100, 0x0003);  // main L and R, no ramps
    r.host.w16(0x18000, 0);                       // SETUP from zeros
    for (u32 w = 0; w < 27; ++w) r.host.w16(0x18000 + 2 * w, 0);
    r.run(cat(std::vector<u16>{0x0000}, addr(0x18000), std::vector<u16>{0x0002}, addr(pb), std::vector<u16>{0x0003, 0x000E},
              addr(0x30000), addr(0x20000), std::vector<u16>{0x000F}));
    CHECK(!r.dsp.failed());
    if (r.dsp.failed()) std::printf("    %s\n", r.dsp.error().c_str());
    bool ok = true;
    for (u32 k = 0; k < 160; ++k) {
        const s32 x = k < 100 ? 100 * static_cast<s32>(k) - 3000 : 0;        // the voice stops after its end
        const s16 env = sat((x * 0x7FFF) >> 15);
        const s16 l = sat((s64{env} * 0x7FFF) >> 15), rr = sat((s64{env} * 0x4000) >> 15);
        ok = ok && static_cast<s16>(r.host.r16(0x20000 + 4 * k + 2)) == l && static_cast<s16>(r.host.r16(0x20000 + 4 * k)) == rr;
        if (!ok) { std::printf("    sample %u: L %d/%d R %d/%d\n", k, static_cast<s16>(r.host.r16(0x20000 + 4 * k + 2)), l,
                               static_cast<s16>(r.host.r16(0x20000 + 4 * k)), rr); break; }
    }
    CHECK(ok);
    CHECK_EQ(pbr(r.host, pb, RUNNING), 0U);       // not looping: stopped at the end
    CHECK_EQ(static_cast<s16>(pbr(r.host, pb, DPOP + 0)), 0);  // last mixed main-left sample
}

TEST_CASE(ax_linear_resampling_loops_ramps_and_updates) {
    Rig r;
    (void)r.read_mail();
    // 16-sample looping ramp, played at half speed (ratio 0x8000) with linear interpolation
    for (u32 k = 0; k < 16; ++k) {
        const auto v = static_cast<u16>(static_cast<s16>(1000 * static_cast<s32>(k)));
        r.host.aram_[0x2000 + 2 * k] = static_cast<u8>(v >> 8); r.host.aram_[0x2000 + 2 * k + 1] = static_cast<u8>(v);
    }
    const u32 pb = 0x70000;
    pcm_voice(r.host, pb, 0x2000, 16, 0x0001);    // main left only
    pbw(r.host, pb, SRC_TYPE, 1);
    pbw(r.host, pb, RATIO, 0);
    pbw(r.host, pb, RATIO + 1, 0x8000);
    pbw(r.host, pb, LOOPING, 1);
    pbw(r.host, pb, MIX + 0, 0x8000);             // volume 1.0 exactly (u16 0x8000 >> 15)
    pbw(r.host, pb, VOL, 0x7FFF);
    // update at ms 3: main left volume (word MIX) = 0x4000
    pbw(r.host, pb, NUM_UPD + 3, 1);
    pbw(r.host, pb, UPD, 0x0008); pbw(r.host, pb, UPD + 1, 0x0000);
    r.host.w16(0x80000, MIX); r.host.w16(0x80002, 0x4000);
    for (u32 w = 0; w < 27; ++w) r.host.w16(0x18000 + 2 * w, 0);
    r.run(cat(std::vector<u16>{0x0000}, addr(0x18000), std::vector<u16>{0x0002}, addr(pb), std::vector<u16>{0x0003, 0x000E},
              addr(0x30000), addr(0x20000), std::vector<u16>{0x000F}));
    CHECK(!r.dsp.failed());
    // model of the same resampler: four-sample history (initially 0), position += 0.5,
    // linear between the two oldest
    std::vector<s16> in;
    for (u32 k = 0; k < 200; ++k) in.push_back(static_cast<s16>(1000 * static_cast<s32>(k % 16)));
    s16 t[4] = {0, 0, 0, 0};
    u32 idx = 0, pos = 0, next = 0;
    bool ok = true;
    for (u32 k = 0; k < 160; ++k) {
        pos += 0x8000;
        while (pos >= 0x10000) { t[idx++ & 3] = in[next++]; pos -= 0x10000; }
        const u32 f = pos & 0xFFFF;
        const s32 a = t[idx & 3], b = t[(idx + 1) & 3];
        const s16 s = f != 0 ? static_cast<s16>((a * static_cast<s32>(0x10000 - f) + b * static_cast<s32>(f)) >> 16) : t[idx & 3];
        const s16 env = sat((s32{s} * 0x7FFF) >> 15);
        const u16 vol = k < 96 ? 0x8000 : 0x4000;
        const s16 expect = sat((s64{env} * vol) >> 15);
        const auto got = static_cast<s16>(r.host.r16(0x20000 + 4 * k + 2));
        if (got != expect) { std::printf("    sample %u: %d vs %d\n", k, got, expect); ok = false; break; }
    }
    CHECK(ok);
    CHECK_EQ(pbr(r.host, pb, RUNNING), 1U);       // looping: still playing
    CHECK_EQ(pbr(r.host, pb, FRAC), 0U);          // 160 * 0.5 = 80 whole input samples
}

TEST_CASE(ax_through_the_dsp_interface_with_a_matching_ucode_image) {
    // Build an IMEM image whose Ector hash is the AX hash (the hash is linear over
    // GF(2): hash = XOR of rotl(byte_i, 3 * (n - i))), boot it through the ROM.
    const u32 target = kAx;
    constexpr unsigned n = 16;
    // basis: bit j of byte i -> rotl(1 << j, 3 * (n - i)) (mod 32)
    std::vector<std::pair<u32, std::pair<unsigned, unsigned>>> rows;
    for (unsigned i = 0; i < n; ++i)
        for (unsigned j = 0; j < 8; ++j) {
            const unsigned r = (3U * (n - i)) % 32U;
            const u32 v = (1U << j);
            rows.push_back({r == 0U ? v : ((v << r) | (v >> (32U - r))), {i, j}});
        }
    // Gaussian elimination to express `target`
    std::vector<u8> image(n, 0);
    std::vector<std::pair<u32, std::vector<std::pair<unsigned, unsigned>>>> basis;
    for (auto& [v, ij] : rows) {
        u32 x = v;
        std::vector<std::pair<unsigned, unsigned>> used{ij};
        for (auto& [b, u] : basis)
            if ((x ^ b) < x) { x ^= b; used.insert(used.end(), u.begin(), u.end()); }
        if (x != 0U) {
            for (auto& [b, u] : basis) if ((b ^ x) < b) { b ^= x; u.insert(u.end(), used.begin(), used.end()); }
            basis.push_back({x, used});
            std::sort(basis.begin(), basis.end(), [](auto& p, auto& q) { return p.first > q.first; });
        }
    }
    u32 x = target;
    std::vector<unsigned> flips(n * 8, 0);
    for (auto& [b, u] : basis)
        if ((x ^ b) < x) { x ^= b; for (auto& [i, j] : u) flips[i * 8 + j] ^= 1U; }
    CHECK_EQ(x, 0U);
    for (unsigned i = 0; i < n; ++i)
        for (unsigned j = 0; j < 8; ++j) if (flips[i * 8 + j] != 0U) image[i] = static_cast<u8>(image[i] ^ (1U << j));
    CHECK_EQ(hle::ucode_hash(image), target);

    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi(bus, nullptr);
    hw::DspInterface di(bus, sched, pi, true);
    CHECK(bus.map_ram("MEM1", 0, 0x01800000U));
    CHECK(bus.dma_write(0x00200000U, image) == mem::BusStatus::Ok);
    di.use_hle(true);
    auto w = [&](u32 o, u16 v) { CHECK(di.mmio_write(o, 2, v)); };
    auto rd = [&](u32 o) { u64 v = 0; CHECK(di.mmio_read(o, 2, v)); return static_cast<u16>(v); };
    w(0x0A, hw::dsp::CSR_RES | hw::dsp::CSR_BIT11 | hw::dsp::CSR_DSPINTMASK);
    CHECK_EQ(rd(0x04), 0x8071U); CHECK_EQ(rd(0x06), 0xFEEDU);
    auto mail = [&](u32 m) { w(0x00, static_cast<u16>(m >> 16)); w(0x02, static_cast<u16>(m)); };
    mail(0x80F3A001U); mail(0x80200000U);
    mail(0x80F3A002U); mail(n);
    mail(0x80F3D001U); mail(0x10U);
    CHECK(!di.dsp_failed());
    CHECK(di.hle_dsp()->ucode() != nullptr && std::string(di.hle_dsp()->ucode()->name()) == "AX (GameCube)");
    CHECK_EQ(rd(0x04), 0xDCD1U); CHECK_EQ(rd(0x06), 0x0000U); // DSP_INIT
    CHECK_EQ(di.csr() & hw::dsp::CSR_DSPINT, hw::dsp::CSR_DSPINT);
}

// ---------------------------------------------------------------- AXWii
namespace {
// largest Wii PB layout (word offsets)
enum : u32 { W_RUNNING = 8, W_SRC = 4, W_MIX_HI = 6, W_MIX_LO = 7, W_MIXER = 10, W_UPD = 53, W_VOL = 58, W_FORMAT = 61, W_LOOP = 62,
             W_END = 64, W_CUR = 66, W_GAIN = 84, W_BIQUAD = 102, W_HPF_END = 106, W_BIQ_END = 112, W_REMOTE = 112,
             W_RMC = 113, W_RMIX = 114, W_SIZE = 155 };
// writes a PB given in the largest layout, in the memory layout of a version
void write_wii_pb(FakeHost& h, u32 addr, const std::vector<u16>& pb, bool updates, bool biquad) {
    std::vector<u16> mem;
    for (u32 w = 0; w < W_SIZE; ++w) {
        if (!updates && w >= W_UPD && w < W_UPD + 5) continue;
        if (!biquad && w >= W_HPF_END && w < W_BIQ_END) continue;
        mem.push_back(pb[w]);
    }
    for (std::size_t i = 0; i < mem.size(); ++i) h.w16(addr + 2U * static_cast<u32>(i), mem[i]);
}
std::vector<u16> wii_pcm_pb(u32 ab, u32 n) {
    std::vector<u16> pb(W_SIZE, 0);
    pb[W_RUNNING] = 1; pb[W_SRC] = 2; pb[W_MIX_LO] = 0x0001; // main left
    pb[W_FORMAT] = 0x000A; pb[W_GAIN] = 0x0800;
    const u32 start = ab / 2, end = start + n - 1;
    pb[W_LOOP] = static_cast<u16>(start >> 16); pb[W_LOOP + 1] = static_cast<u16>(start);
    pb[W_END] = static_cast<u16>(end >> 16); pb[W_END + 1] = static_cast<u16>(end);
    pb[W_CUR] = static_cast<u16>(start >> 16); pb[W_CUR + 1] = static_cast<u16>(start);
    pb[W_VOL] = 0x8000; pb[W_MIXER] = 0x8000;
    return pb;
}
struct WiiRig {
    FakeHost host;
    hle::Dsp dsp{host, true};
    explicit WiiRig(u32 hash) {
        dsp.reset(false); dsp.set_halted(false);
        (void)read_mail();
        dsp.switch_to(hash);
        (void)read_mail(); // DSP_INIT
    }
    u32 read_mail() { const u32 hi = dsp.cpu_mailbox_high(); return hi << 16 | dsp.cpu_mailbox_low(); }
    void send(u32 m) { dsp.cpu_write_high(static_cast<u16>(m >> 16)); dsp.cpu_write_low(static_cast<u16>(m)); }
    void run(const std::vector<u16>& list) {
        for (std::size_t i = 0; i < list.size(); ++i) host.w16(0x10000U + 2U * static_cast<u32>(i), list[i]);
        send(0xBABE0000U | static_cast<u32>(list.size()));
        send(0x00010000U);
    }
};
void ramp_aram(FakeHost& h, u32 ab, u32 n) {
    for (u32 k = 0; k < n; ++k) {
        const auto v = static_cast<u16>(static_cast<s16>(50 * static_cast<s32>(k) - 2000));
        h.aram_[ab + 2 * k] = static_cast<u8>(v >> 8); h.aram_[ab + 2 * k + 1] = static_cast<u8>(v);
    }
}
} // namespace

TEST_CASE(axwii_voice_in_each_pb_layout) {
    struct V { u32 hash; bool updates, biquad, old; };
    const V versions[] = {{0x347112BAU, false, true, false}, {0xD9C4BF34U, false, false, false}, {0xFA450138U, true, false, true}};
    for (const V& v : versions) {
        WiiRig r(v.hash);
        ramp_aram(r.host, 0x1000, 200);
        write_wii_pb(r.host, 0x70000, wii_pcm_pb(0x1000, 200), v.updates, v.biquad);
        for (u32 w = 0; w < 60; ++w) r.host.w16(0x18000 + 2 * w, 0);
        std::vector<u16> list = cat(std::vector<u16>{0x0000}, addr(0x18000));
        if (v.old) list = cat(list, std::vector<u16>{0x0004}, addr(0x70000), std::vector<u16>{0x0005, 0x000C}, addr(0x30000), addr(0x20000),
                              std::vector<u16>{0x000F});
        else if (v.hash == 0xD9C4BF34U) list = cat(list, std::vector<u16>{0x0004}, addr(0x70000), std::vector<u16>{0x000B}, addr(0x30000),
                                                   addr(0x20000), std::vector<u16>{0x000E});
        else list = cat(list, std::vector<u16>{0x0004}, addr(0x70000), std::vector<u16>{0x000B, 0x8000}, addr(0x30000), addr(0x20000),
                        std::vector<u16>{0x000E});
        r.run(list);
        CHECK(!r.dsp.failed());
        if (r.dsp.failed()) { std::printf("    %08X: %s\n", v.hash, r.dsp.error().c_str()); continue; }
        CHECK_EQ(r.read_mail(), hle::kMailSync);    // after the output
        CHECK_EQ(r.read_mail(), hle::kMailYield);
        bool ok = true;
        for (u32 k = 0; k < 96; ++k) {
            const auto expect = static_cast<s16>(50 * static_cast<s32>(k) - 2000); // unsigned 0x8000 = 1.0 everywhere
            ok = ok && static_cast<s16>(r.host.r16(0x20000 + 4 * k + 2)) == expect && r.host.r16(0x20000 + 4 * k) == 0;
        }
        CHECK(ok);
        if (!ok) std::printf("    version %08X: first sample %d\n", v.hash, static_cast<s16>(r.host.r16(0x20002)));
        CHECK(r.dsp.warnings().empty());              // no ramp (0x8000 -> 0x8000), no polyphase
    }
}

TEST_CASE(axwii_wii_remote_speaker_and_unknown_layout) {
    WiiRig r(0x347112BAU);
    for (u32 k = 0; k < 200; ++k) { r.host.aram_[0x1000 + 2 * k] = 0x03; r.host.aram_[0x1000 + 2 * k + 1] = 0xE8; } // 1000
    auto pb = wii_pcm_pb(0x1000, 200);
    pb[W_REMOTE] = 1;
    pb[W_RMC] = 0x0001;                // channel main0 on, no ramp
    pb[W_RMIX] = 0x8000;
    write_wii_pb(r.host, 0x70000, pb, false, true);
    for (u32 w = 0; w < 60; ++w) r.host.w16(0x18000 + 2 * w, 0);
    r.run(cat(std::vector<u16>{0x0000}, addr(0x18000), std::vector<u16>{0x0004}, addr(0x70000), std::vector<u16>{0x000D}, addr(0x40000),
              addr(0x40100), addr(0x40200), addr(0x40300), std::vector<u16>{0x000E}));
    CHECK(!r.dsp.failed());
    // 18 speaker samples, resampled by ~96/18 from the voice (linear without the COEF dump)
    CHECK_EQ(static_cast<s16>(r.host.r16(0x40000 + 2 * 17)), 1000);
    CHECK_EQ(r.host.r16(0x40100), 0U);  // speaker 1 got nothing
    CHECK(!r.dsp.warnings().empty());   // polyphase without the COEF ROM: reported
    WiiRig u(0x2EA36CE6U);              // a version whose PB layout Orca does not know
    u.run(cat(std::vector<u16>{0x0004}, addr(0x70000), std::vector<u16>{0x000E}));
    CHECK(u.dsp.failed());
    CHECK(u.dsp.error().find("layout") != std::string::npos);
}

TEST_MAIN("dsp_hle")
