// GPU command stream parser (M013). Expected values worked out by hand from
// the byte format documented in command_stream.hpp.
#include "orca/gpu/command_stream.hpp"
#include "orca/gpu/xf_memory.hpp"
#include "orca/gpu/bp_memory.hpp"
#include "orca/gpu/cp_state.hpp"
#include "orca/gpu/texture_decoder.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/system/system.hpp"
#include "orca/video/xfb.hpp"
#include "orca/memory/bus.hpp"
#include "test_framework.hpp"
#include "test_port.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>

#include <string>
#include <vector>

using namespace orca;
using namespace orca::gpu;

namespace {

struct Ev {
    char kind;
    u32 a, b, c, d;
    std::vector<u8> data;
};

struct Sink : CommandSink {
    std::vector<Ev> ev;
    u32 vsize[8] = {4, 8, 12, 1, 2, 3, 5, 0};
    char fail_on = 0;
    bool on_cp(u8 reg, u32 value, std::string& e) override { return rec({'C', reg, value, 0, 0, {}}, e); }
    bool on_xf(u16 addr, std::span<const u8> w, std::string& e) override {
        return rec({'X', addr, static_cast<u32>(w.size()), 0, 0, {w.begin(), w.end()}}, e);
    }
    bool on_indexed_xf(u8 array, u16 index, u16 addr, u8 size, std::string& e) override {
        return rec({'I', array, index, addr, size, {}}, e);
    }
    bool on_bp(u8 reg, u32 value, std::string& e) override { return rec({'B', reg, value, 0, 0, {}}, e); }
    bool on_primitive(Primitive p, u8 vat, u16 count, u32 vs, std::span<const u8> d, std::string& e) override {
        return rec({'P', static_cast<u32>(p), vat, count, vs, {d.begin(), d.end()}}, e);
    }
    bool on_display_list(u32 addr, u32 size, std::string& e) override { return rec({'D', addr, size, 0, 0, {}}, e); }
    bool vertex_size(u8 vat, u32& size, std::string& e) override {
        if (fail_on == 'S') { e = "no size"; return false; }
        size = vsize[vat];
        return true;
    }
    bool rec(Ev x, std::string& e) {
        if (x.kind == fail_on) {
            e = "sink refused";
            return false;
        }
        ev.push_back(std::move(x));
        return true;
    }
};

ParseResult run(Sink& s, const std::vector<u8>& b, bool dl = false) { return parse_commands(b, s, dl); }

} // namespace

TEST_CASE(gpu_cmd_register_loads) {
    Sink s;
    const std::vector<u8> b = {0x08, 0x50, 0x12, 0x34, 0x56, 0x78,       // CP 0x50 = 0x12345678
                               0x61, 0x49, 0xAB, 0xCD, 0xEF,             // BP 0x49 = 0xABCDEF
                               0x10, 0x00, 0x01, 0x10, 0x20,             // XF 2 words at 0x1020
                               1, 2, 3, 4, 5, 6, 7, 8,
                               0x30, 0x01, 0x23, 0x5A, 0xBC};            // indexed C: index 0x123, size 6, addr 0xABC
    const ParseResult r = run(s, b);
    CHECK(!r.error);
    CHECK_EQ(r.consumed, static_cast<u32>(b.size()));
    CHECK_EQ(s.ev.size(), 4U);
    if (s.ev.size() != 4U) return;
    CHECK_EQ(s.ev[0].kind, 'C'); CHECK_EQ(s.ev[0].a, 0x50U); CHECK_EQ(s.ev[0].b, 0x12345678U);
    CHECK_EQ(s.ev[1].kind, 'B'); CHECK_EQ(s.ev[1].a, 0x49U); CHECK_EQ(s.ev[1].b, 0xABCDEFU);
    CHECK_EQ(s.ev[2].kind, 'X'); CHECK_EQ(s.ev[2].a, 0x1020U); CHECK_EQ(s.ev[2].b, 8U);
    CHECK_EQ(s.ev[2].data[7], 8U);
    CHECK_EQ(s.ev[3].kind, 'I'); CHECK_EQ(s.ev[3].a, 14U); CHECK_EQ(s.ev[3].b, 0x0123U);
    CHECK_EQ(s.ev[3].c, 0xABCU); CHECK_EQ(s.ev[3].d, 6U);
    // the four indexed opcodes map to arrays 12-15
    Sink t;
    CHECK(!run(t, {0x20, 0, 0, 0, 0, 0x28, 0, 0, 0, 0, 0x38, 0xFF, 0xFF, 0xFF, 0xFF}).error);
    CHECK_EQ(t.ev.size(), 3U);
    if (t.ev.size() == 3U) {
        CHECK_EQ(t.ev[0].a, 12U); CHECK_EQ(t.ev[0].d, 1U);
        CHECK_EQ(t.ev[1].a, 13U);
        CHECK_EQ(t.ev[2].a, 15U); CHECK_EQ(t.ev[2].b, 0xFFFFU); CHECK_EQ(t.ev[2].c, 0xFFFU); CHECK_EQ(t.ev[2].d, 16U);
    }
}

TEST_CASE(gpu_cmd_xf_count_limits) {
    Sink s;
    std::vector<u8> b = {0x10, 0x00, 0x0F, 0x00, 0x00};
    b.resize(5 + 64, 0xAA);
    ParseResult r = run(s, b);
    CHECK(!r.error);
    CHECK_EQ(r.consumed, 69U);
    if (!s.ev.empty()) CHECK_EQ(s.ev[0].b, 64U); // 16 words
    Sink t;
    r = run(t, {0x10, 0x00, 0x10, 0x00, 0x00, 0, 0, 0, 0});
    CHECK(r.error);
    CHECK_EQ(r.consumed, 0U);
    CHECK(r.message.find("above 15") != std::string::npos);
}

TEST_CASE(gpu_cmd_primitives) {
    Sink s;
    // every primitive opcode decodes type = bits 3-6, VAT = bits 0-2
    unsigned bad = 0;
    for (u32 opc = 0x80; opc <= 0xBF; ++opc) {
        Sink t;
        t.vsize[opc & 7U] = 2;
        const ParseResult r = run(t, {static_cast<u8>(opc), 0x00, 0x02, 1, 2, 3, 4});
        if (r.error || r.consumed != 7U || t.ev.size() != 1U || t.ev[0].a != ((opc >> 3) & 7U) || t.ev[0].b != (opc & 7U) ||
            t.ev[0].c != 2U || t.ev[0].data.size() != 4U)
            ++bad;
    }
    CHECK_EQ(bad, 0U);
    // 0x92: triangles (0x90), VAT 2 (12 bytes per vertex), 2 vertices
    std::vector<u8> b = {0x92, 0x00, 0x02};
    for (u8 i = 0; i < 24; ++i) b.push_back(i);
    b.push_back(0x00);
    const ParseResult r = run(s, b);
    CHECK(!r.error);
    CHECK_EQ(r.consumed, 28U);
    CHECK_EQ(s.ev.size(), 1U);
    if (!s.ev.empty()) {
        CHECK_EQ(s.ev[0].a, static_cast<u32>(Primitive::Triangles));
        CHECK_EQ(s.ev[0].d, 12U);
        CHECK_EQ(s.ev[0].data[23], 23U);
    }
    // zero vertices: 3 bytes
    Sink z;
    CHECK_EQ(run(z, {0xB8, 0x00, 0x00}).consumed, 3U);
    CHECK_EQ(z.ev.size(), 1U);
}

TEST_CASE(gpu_cmd_incomplete_commands_are_left) {
    // a command cut at any point is not consumed; what precedes it is
    const std::vector<std::vector<u8>> cmds = {
        {0x08, 0x50, 1, 2, 3, 4},
        {0x61, 0x49, 1, 2, 3},
        {0x10, 0x00, 0x01, 0x10, 0x20, 1, 2, 3, 4, 5, 6, 7, 8},
        {0x20, 0, 1, 2, 3},
        {0x40, 0, 0, 0x10, 0x00, 0, 0, 0, 0x40},
        {0x99, 0x00, 0x01, 1, 2, 3, 4, 5, 6, 7, 8}, // VAT 1: 8 bytes
    };
    unsigned bad = 0;
    for (const auto& c : cmds) {
        for (std::size_t cut = 1; cut < c.size(); ++cut) {
            Sink s;
            std::vector<u8> b = {0x00, 0x00};
            b.insert(b.end(), c.begin(), c.begin() + static_cast<std::ptrdiff_t>(cut));
            const ParseResult r = run(s, b);
            if (r.error || r.consumed != 2U || !s.ev.empty()) ++bad;
        }
        Sink s;
        if (run(s, c).consumed != c.size()) ++bad;
    }
    CHECK_EQ(bad, 0U);
}

TEST_CASE(gpu_cmd_display_lists_and_noops) {
    Sink s;
    ParseResult r = run(s, {0x40, 0x80, 0x12, 0x34, 0x5F, 0x00, 0x00, 0x01, 0x3F, // address & ~31, size & ~31
                            0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x3F, 0x44, 0x48, 0x00});
    CHECK(!r.error);
    CHECK_EQ(r.consumed, 20U);
    CHECK_EQ(s.ev.size(), 1U);
    if (!s.ev.empty()) {
        CHECK_EQ(s.ev[0].kind, 'D');
        CHECK_EQ(s.ev[0].a, 0x80123440U);
        CHECK_EQ(s.ev[0].b, 0x00000120U);
    }
    Sink t;
    r = run(t, {0x00, 0x40, 0, 0, 0, 0, 0, 0, 0, 0}, true);
    CHECK(r.error);
    CHECK_EQ(r.consumed, 1U);
    CHECK(r.message.find("inside a display list") != std::string::npos);
}

TEST_CASE(gpu_cmd_errors) {
    // unknown opcodes stop at their position
    const u8 unknown[] = {0x09, 0x18, 0x41, 0x50, 0x60, 0x62, 0x7F};
    unsigned bad = 0;
    for (u8 u : unknown) {
        Sink s;
        const ParseResult r = run(s, {0x00, 0x00, 0x00, u, 0x00});
        if (!r.error || r.consumed != 3U || r.message.find("opcode") == std::string::npos) ++bad;
    }
    CHECK_EQ(bad, 0U);
    Sink s;
    ParseResult r = run(s, {0x00, 0x18});
    CHECK(r.message.find("18") != std::string::npos);
    // a sink refusal stops parsing at that command with the sink's message
    Sink t;
    t.fail_on = 'B';
    r = run(t, {0x08, 0x50, 0, 0, 0, 0, 0x61, 0x00, 0, 0, 0, 0x08, 0x60, 0, 0, 0, 0});
    CHECK(r.error);
    CHECK_EQ(r.consumed, 6U);
    CHECK(r.message == "sink refused");
    CHECK_EQ(t.ev.size(), 1U);
}

TEST_CASE(gpu_cmd_vertex_size_error_stops) {
    Sink s;
    s.fail_on = 'S';
    const ParseResult r = run(s, {0x00, 0x90, 0x00, 0x01, 0, 0, 0, 0});
    CHECK(r.error);
    CHECK_EQ(r.consumed, 1U);
    CHECK(r.message == "no size");
}

TEST_CASE(gpu_cp_loads) {
    CpState cp;
    std::string err;
    CHECK(cp.load(0x30, 0x12345678U, err)); CHECK_EQ(cp.matrix_index_a, 0x12345678U);
    CHECK(cp.load(0x4F, 0x00ABCDEFU, err)); CHECK_EQ(cp.matrix_index_b, 0x00ABCDEFU); // low nibble ignored
    CHECK(cp.load(0x57, 0x0001FFFFU, err)); CHECK_EQ(cp.vcd_lo, 0x0001FFFFU);
    CHECK(cp.load(0x60, 0x0000FFFFU, err)); CHECK_EQ(cp.vcd_hi, 0x0000FFFFU);
    CHECK(cp.load(0x73, 0xAAAAAAAAU, err)); CHECK_EQ(cp.vat[3][0], 0xAAAAAAAAU);
    CHECK(cp.load(0x87, 0xBBBBBBBBU, err)); CHECK_EQ(cp.vat[7][1], 0xBBBBBBBBU);
    CHECK(cp.load(0x90, 0xCCCCCCCCU, err)); CHECK_EQ(cp.vat[0][2], 0xCCCCCCCCU);
    CHECK(cp.load(0xAF, 0x80123456U, err)); CHECK_EQ(cp.array_base[15], 0x80123456U);
    CHECK(cp.load(0xB1, 0x00000123U, err)); CHECK_EQ(cp.array_stride[1], 0x23U);
    CHECK(cp.load(0x00, 1, err)); CHECK(cp.load(0x10, 1, err)); CHECK(cp.load(0x2F, 1, err));
    CHECK(!cp.load(0x78, 0, err)); CHECK(err.find("format 8") != std::string::npos);
    CHECK(!cp.load(0x9F, 0, err));
    CHECK(!cp.load(0xC0, 0, err)); CHECK(err.find("not handled") != std::string::npos);
    CHECK(!cp.load(0xF5, 0, err));
}

TEST_CASE(gpu_cp_vertex_sizes) {
    std::string err;
    u32 sz = 0;
    // position direct, 3 x float = 12
    CpState a;
    a.vcd_lo = vcf::DIRECT << 9;
    a.vat[0][0] = 1U | (4U << 1);
    CHECK(a.vertex_size(0, sz, err)); CHECK_EQ(sz, 12U);
    // 2 x s16 = 4
    a.vat[0][0] = 0U | (3U << 1);
    CHECK(a.vertex_size(0, sz, err)); CHECK_EQ(sz, 4U);
    // invalid float formats 5-7 also count as 4 bytes
    a.vat[0][0] = 1U | (7U << 1);
    CHECK(a.vertex_size(0, sz, err)); CHECK_EQ(sz, 12U);
    // matrix indices: PosMatIdx + Tex0MatIdx + Tex7MatIdx = 3 bytes, position index16 = 2
    CpState b;
    b.vcd_lo = 0x1U | 0x2U | 0x100U | (vcf::INDEX16 << 9);
    CHECK(b.vertex_size(0, sz, err)); CHECK_EQ(sz, 5U);
    // normal: direct NBT s8 = 9; index8 NBT with NormalIndex3 = 3; without = 1
    CpState c;
    c.vcd_lo = vcf::DIRECT << 11;
    c.vat[1][0] = (1U << 9) | (1U << 10);
    CHECK(c.vertex_size(1, sz, err)); CHECK_EQ(sz, 9U);
    c.vcd_lo = vcf::INDEX8 << 11;
    CHECK(c.vertex_size(1, sz, err)); CHECK_EQ(sz, 1U);
    c.vat[1][0] |= 1U << 31;
    CHECK(c.vertex_size(1, sz, err)); CHECK_EQ(sz, 3U);
    c.vcd_lo = vcf::INDEX16 << 11;
    CHECK(c.vertex_size(1, sz, err)); CHECK_EQ(sz, 6U);
    c.vat[1][0] &= ~(1U << 9); // single normal: index3 has no effect
    CHECK(c.vertex_size(1, sz, err)); CHECK_EQ(sz, 2U);
    c.vcd_lo = vcf::DIRECT << 11; // 3 x float
    c.vat[1][0] = 4U << 10;
    CHECK(c.vertex_size(1, sz, err)); CHECK_EQ(sz, 12U);
    // colours: every direct format, and the two invalid ones
    const u32 csize[6] = {2, 3, 4, 2, 3, 4};
    unsigned bad = 0;
    for (u32 f = 0; f < 6; ++f) {
        CpState d;
        d.vcd_lo = (vcf::DIRECT << 13) | (vcf::DIRECT << 15);
        d.vat[2][0] = (f << 14) | (f << 18);
        if (!d.vertex_size(2, sz, err) || sz != 2U * csize[f]) ++bad;
    }
    CHECK_EQ(bad, 0U);
    CpState e;
    e.vcd_lo = vcf::INDEX8 << 15;
    e.vat[2][0] = 6U << 18;
    CHECK(!e.vertex_size(2, sz, err));
    CHECK(err.find("invalid format 6") != std::string::npos);
    e.vcd_lo = vcf::NONE << 15; // not present: format ignored
    CHECK(e.vertex_size(2, sz, err)); CHECK_EQ(sz, 0U);
    // texture coordinates: tex0 ST u16 = 4 (VAT g0), tex4 S float = 4 (fmt in g1), tex7 index16 = 2
    CpState t;
    t.vcd_hi = (vcf::DIRECT << 0) | (vcf::DIRECT << 8) | (vcf::INDEX16 << 14);
    t.vat[5][0] = (1U << 21) | (2U << 22);
    t.vat[5][1] = 4U << 28;
    CHECK(t.vertex_size(5, sz, err)); CHECK_EQ(sz, 4U + 4U + 2U);
    t.vat[5][1] |= 1U << 27; // tex4 ST
    CHECK(t.vertex_size(5, sz, err)); CHECK_EQ(sz, 4U + 8U + 2U);
    // VAT field decoding of the third group
    const VatFields vf = decode_vat({0U, 0U, (31U) | (1U << 23) | (3U << 24) | (5U << 27)});
    CHECK_EQ(vf.tex_frac[4], 31U);
    CHECK_EQ(vf.tex_elems[7], 1U);
    CHECK_EQ(vf.tex_fmt[7], 3U);
    CHECK_EQ(vf.tex_frac[7], 5U);
}

TEST_CASE(gpu_xf_memory_loads) {
    XfMemory xf;
    std::string err;
    const std::vector<u8> w = {0x3F, 0x80, 0x00, 0x00, 0x12, 0x34, 0x56, 0x78, 0xDE, 0xAD, 0xBE, 0xEF};
    CHECK(xf.load(0x0000, w, err));
    CHECK_EQ(xf.word(0x0000), 0x3F800000U);
    CHECK(xf.as_float(0x0000) == 1.0f);
    CHECK_EQ(xf.word(0x0002), 0xDEADBEEFU);
    CHECK(xf.load(0x101A, w, err)); // viewport registers
    CHECK_EQ(xf.word(0x101B), 0x12345678U);
    CHECK(xf.load(0x0FFF, w, err)); // crosses from memory into registers
    CHECK_EQ(xf.word(0x0FFF), 0x3F800000U);
    CHECK_EQ(xf.word(0x1001), 0xDEADBEEFU);
    CHECK(xf.load(0x0800, w, err)); // undocumented area is stored
    CHECK_EQ(xf.word(0x0801), 0x12345678U);
    // runs past the end: words up to 0x1057 kept, error reported
    CHECK(!xf.load(0x1056, w, err));
    CHECK_EQ(xf.word(0x1056), 0x3F800000U);
    CHECK_EQ(xf.word(0x1057), 0x12345678U);
    CHECK(err.find("runs past") != std::string::npos);
    CHECK(!xf.load(0x1058, w, err));
    CHECK(err.find("past the XF address space") != std::string::npos);
    CHECK_EQ(xf.word(0x2000), 0U);
    CHECK(xf.load(0x1057, std::vector<u8>{1, 2, 3, 4}, err)); // last word exactly
    CHECK_EQ(xf.word(0x1057), 0x01020304U);
}

TEST_CASE(gpu_bp_mask_register) {
    BpMemory b;
    CHECK_EQ(b.reg(bp::BP_MASK), 0xFFFFFFU);
    CHECK_EQ(b.load(bp::GENMODE, 0x12345678U), 0x345678U); // 24-bit registers
    CHECK_EQ(b.load(bp::BP_MASK, 0x00FF00U), 0x00FF00U);
    CHECK_EQ(b.load(bp::GENMODE, 0xAAAAAAU), 0x34AA78U);   // only bits 8-15 change
    CHECK_EQ(b.reg(bp::BP_MASK), 0xFFFFFFU);               // mask reset after use
    CHECK_EQ(b.load(bp::GENMODE, 0x000000U), 0x000000U);
    // a mask write is itself masked by the mask in force
    CHECK_EQ(b.load(bp::BP_MASK, 0x0000FFU), 0x0000FFU);
    CHECK_EQ(b.load(bp::BP_MASK, 0xFFFF00U), 0x000000U);  // (0xFF & ~0xFF) | (0xFFFF00 & 0xFF) = 0
    CHECK_EQ(b.load(bp::ZMODE, 0x123456U), 0x000000U);    // mask 0: nothing changes
    CHECK_EQ(b.reg(bp::BP_MASK), 0xFFFFFFU);
    b.load(bp::BP_MASK, 0);
    b.reset();
    CHECK_EQ(b.reg(bp::BP_MASK), 0xFFFFFFU);
    CHECK_EQ(b.reg(bp::ZMODE), 0U);
}

namespace {
struct PeRig {
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::PixelEngine pe{sched, pi};
    u16 r(u32 off) { u64 v = 0; CHECK(pe.mmio_read(off, 2, v)); return static_cast<u16>(v); }
    void w(u32 off, u16 v) { CHECK(pe.mmio_write(off, 2, v)); }
    void to(u64 t) { sched.advance_to(t); }
};
} // namespace

TEST_CASE(gpu_pe_registers) {
    PeRig t;
    for (u32 off = 0; off <= 8; off += 2) { t.w(off, static_cast<u16>(0x1100 + off)); }
    for (u32 off = 0; off <= 8; off += 2) CHECK_EQ(t.r(off), 0x1100U + off);
    t.w(hw::pe::CTRL, 0xFFFF);
    CHECK_EQ(t.r(hw::pe::CTRL), 0x0003U); // ack bits read 0, upper bits not stored
    u64 v = 0;
    CHECK(t.pe.mmio_read(0x00, 4, v));
    CHECK_EQ(v, 0x11001102ULL);
    CHECK(!t.pe.mmio_write(hw::pe::TOKEN, 2, 5));
    CHECK(t.pe.mmio_error().find("read-only") != std::string::npos);
    // Bounding box and quad counters (M018): readable, read-only
    t.pe.stats().bbox = {1, 2, 3, 4};
    t.pe.stats().quads[gpu::PixelStats::BlendInput] = 0x00012345U;
    CHECK_EQ(t.r(0x10), 1U); CHECK_EQ(t.r(0x12), 2U); CHECK_EQ(t.r(0x14), 3U); CHECK_EQ(t.r(0x16), 4U);
    CHECK_EQ(t.r(0x28), 0x2345U); CHECK_EQ(t.r(0x2A), 0x0001U);
    CHECK(t.pe.mmio_read(0x10, 4, v));
    CHECK_EQ(v, 0x00010002ULL);
    CHECK(!t.pe.mmio_write(0x10, 2, 5));
    CHECK(!t.pe.mmio_read(0x2C, 2, v)); // EFB copy clocks: no timing model
    CHECK(t.pe.mmio_error().find("copy clock") != std::string::npos);
    t.pe.stats().tracked = false;       // a GPU backend that does not track them
    CHECK(!t.pe.mmio_read(0x10, 2, v));
    CHECK(t.pe.mmio_error().find("not tracked") != std::string::npos);
    CHECK(!t.pe.mmio_read(0x18, 2, v));
    CHECK(!t.pe.mmio_read(0x0C, 2, v));
    CHECK(!t.pe.mmio_read(0x00, 1, v));
}

TEST_CASE(gpu_pe_token_and_finish_timing) {
    PeRig t;
    t.w(hw::pe::CTRL, hw::pe::CTRL_TOKEN_ENABLE | hw::pe::CTRL_FINISH_ENABLE);
    t.pe.set_token(0x1234, true, 100); // below the 500-cycle minimum
    t.to(499);
    CHECK_EQ(t.r(hw::pe::TOKEN), 0U);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, 0U);
    t.to(500);
    CHECK_EQ(t.r(hw::pe::TOKEN), 0x1234U);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, hw::pi::CAUSE_PE_TOKEN);
    t.w(hw::pe::CTRL, hw::pe::CTRL_TOKEN_ENABLE | hw::pe::CTRL_FINISH_ENABLE | hw::pe::CTRL_TOKEN_ACK);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, 0U);
    // token without interrupt, then finish before the event: one event, latest token, finish signalled
    t.pe.set_token(0x0001, false, 2000);
    t.pe.set_finish(10);
    t.pe.set_token(0x0002, false, 10);
    t.to(500 + 1999);
    CHECK_EQ(t.r(hw::pe::TOKEN), 0x1234U);
    t.to(500 + 2000);
    CHECK_EQ(t.r(hw::pe::TOKEN), 0x0002U);
    CHECK_EQ(t.pi.causes() & (hw::pi::CAUSE_PE_TOKEN | hw::pi::CAUSE_PE_FINISH), hw::pi::CAUSE_PE_FINISH);
    // disabling the enable lowers the line; re-enabling raises it again (still signalled)
    t.w(hw::pe::CTRL, hw::pe::CTRL_TOKEN_ENABLE);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_FINISH, 0U);
    t.w(hw::pe::CTRL, hw::pe::CTRL_FINISH_ENABLE);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_FINISH, hw::pi::CAUSE_PE_FINISH);
    t.w(hw::pe::CTRL, hw::pe::CTRL_FINISH_ACK);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_FINISH, 0U);
    t.w(hw::pe::CTRL, hw::pe::CTRL_FINISH_ENABLE);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_FINISH, 0U); // acknowledged: stays low
    // an interrupt request stays pending even if a later token in the same window has none
    t.pe.set_token(0x0010, true, 0);
    t.pe.set_token(0x0011, false, 0);
    t.to(t.sched.now() + 500);
    CHECK_EQ(t.r(hw::pe::TOKEN), 0x0011U);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, 0U); // enable is off at this point (finish only)
    t.w(hw::pe::CTRL, hw::pe::CTRL_TOKEN_ENABLE);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, hw::pi::CAUSE_PE_TOKEN);
    t.w(hw::pe::CTRL, hw::pe::CTRL_TOKEN_ACK);
    // a token interrupt requested while disabled shows up once enabled
    t.w(hw::pe::CTRL, 0);
    t.pe.set_token(0x0003, true, 0);
    t.to(t.sched.now() + 500);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, 0U);
    t.w(hw::pe::CTRL, hw::pe::CTRL_TOKEN_ENABLE);
    CHECK_EQ(t.pi.causes() & hw::pi::CAUSE_PE_TOKEN, hw::pi::CAUSE_PE_TOKEN);
}

namespace {
// A System with the CPU-GPU FIFO set up the way GX does: PI and CP share base/end,
// linked, reads enabled.
struct FifoRig {
    system::System sys{{}};
    mem::Bus& bus() { return sys.bus(); }
    void cpw(u32 off, u16 v) { CHECK(bus().write16(0x0C000000U + off, v) == mem::BusStatus::Ok); }
    u16 cpr(u32 off) { u16 v = 0; CHECK(bus().read16(0x0C000000U + off, v) == mem::BusStatus::Ok); return v; }
    void cp32(u32 off, u32 v) { cpw(off, static_cast<u16>(v)); cpw(off + 2U, static_cast<u16>(v >> 16)); }
    u32 cpr32(u32 off) { return cpr(off) | (static_cast<u32>(cpr(off + 2U)) << 16); }
    void pi32(u32 off, u32 v) { CHECK(bus().write32(0x0C003000U + off, v) == mem::BusStatus::Ok); }
    u32 pir32(u32 off) { u32 v = 0; CHECK(bus().read32(0x0C003000U + off, v) == mem::BusStatus::Ok); return v; }
    void setup(u32 base, u32 end, u16 ctrl = hw::cp::CTRL_LINK | hw::cp::CTRL_READ) {
        pi32(0x0C, base); pi32(0x10, end); pi32(0x14, base);
        cp32(hw::cp::FIFO_BASE, base); cp32(hw::cp::FIFO_END, end);
        cp32(hw::cp::FIFO_WP, base); cp32(hw::cp::FIFO_RP, base);
        cp32(hw::cp::FIFO_HI_WM, 0x3FFE0U);
        cpw(hw::cp::CTRL, ctrl);
    }
    // writes bytes through the gather pipe (byte stores)
    void gp(const std::vector<u8>& b) { for (u8 x : b) CHECK(bus().write8(0x0C008000U, x) == mem::BusStatus::Ok); }
    void gp_pad() { while (sys.gather_pipe().pending_bytes() != 0U) gp({0x00}); }
    void wait(u64 cycles) { sys.scheduler().advance_to(sys.scheduler().now() + cycles); }
    u16 pe_token() { u16 v = 0; CHECK(bus().read16(0x0C00100EU, v) == mem::BusStatus::Ok); return v; }
};
std::vector<u8> bp_cmd(u8 reg, u32 v) { return {0x61, reg, static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)}; }
} // namespace

TEST_CASE(gpu_cp_register_masks) {
    FifoRig t;
    t.cp32(hw::cp::FIFO_BASE, 0xFFFFFFFFU);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_BASE), 0x03FFFFE0U); // LO 0xFFE0, HI 0x03FF on GameCube
    t.cp32(hw::cp::FIFO_BP, 0x12345678U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_BP), 0x02345660U);
    t.cpw(hw::cp::CTRL, 0xFFFF);
    CHECK_EQ(t.cpr(hw::cp::CTRL), 0x3FU);
    t.cpw(hw::cp::CTRL, 0);
    t.cpw(hw::cp::PERF_SELECT, 0xFFFF); CHECK_EQ(t.cpr(hw::cp::PERF_SELECT), 7U);
    t.cpw(hw::cp::UNK_0A, 0xFFFF); CHECK_EQ(t.cpr(hw::cp::UNK_0A), 0xFFU);
    CHECK_EQ(t.cpr(hw::cp::CLEAR), 0U);
    CHECK_EQ(t.cpr(0x40), 0U); CHECK_EQ(t.cpr(0x5A), 0U); CHECK_EQ(t.cpr(0x64), 4U);
    u16 v = 0;
    CHECK(t.bus().read16(0x0C000062U, v) != mem::BusStatus::Ok);
    CHECK(t.bus().write16(0x0C000000U, 1) != mem::BusStatus::Ok); // status is read only
    CHECK(t.bus().write16(0x0C000040U, 1) != mem::BusStatus::Ok);
    // reset status: idle, below watermark (lo wm 0, so not below)
    CHECK_EQ(t.cpr(hw::cp::STATUS), hw::cp::ST_READ_IDLE | hw::cp::ST_CMD_IDLE);
    // Wii keeps 13 high bits
    system::SystemConfig wc{};
    wc.console = system::Console::Wii;
    system::System wii{wc};
    CHECK(wii.bus().write16(0x0C000022U, 0xFFFF) == mem::BusStatus::Ok);
    CHECK(wii.bus().read16(0x0C000022U, v) == mem::BusStatus::Ok);
    CHECK_EQ(v, 0x1FFFU);
}

TEST_CASE(gpu_fifo_token_through_gather_pipe) {
    FifoRig t;
    t.setup(0x00100000U, 0x00100040U);
    CHECK(t.bus().write16(0x0C00100AU, hw::pe::CTRL_TOKEN_ENABLE) == mem::BusStatus::Ok);
    t.gp(bp_cmd(0x48, 0x001234)); // token with interrupt
    CHECK_EQ(t.cpr32(hw::cp::FIFO_WP), 0x00100000U); // not yet a full burst
    t.gp_pad();
    CHECK_EQ(t.cpr32(hw::cp::FIFO_WP), 0x00100020U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RP), 0x00100020U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0U);
    CHECK_EQ(t.pir32(0x14) & 0x03FFFFE0U, 0x00100020U);
    CHECK_EQ(t.sys.gpu().bp().reg(0x48), 0x001234U);
    CHECK_EQ(t.pe_token(), 0U);
    t.wait(500);
    CHECK_EQ(t.pe_token(), 0x1234U);
    CHECK_EQ(t.sys.pi().causes() & hw::pi::CAUSE_PE_TOKEN, hw::pi::CAUSE_PE_TOKEN);
    CHECK(!t.sys.command_processor().stopped());
}

TEST_CASE(gpu_fifo_wraps_and_commands_span_bursts) {
    FifoRig t;
    t.setup(0x00200000U, 0x00200040U); // three 32-byte blocks: end is the last one
    // XF load of 16 words (69 bytes) crosses three bursts
    std::vector<u8> xf = {0x10, 0x00, 0x0F, 0x00, 0x40};
    for (u32 i = 0; i < 64; ++i) xf.push_back(static_cast<u8>(i));
    t.gp(xf);
    t.gp_pad(); // 69 bytes + 27 NOPs = 3 bursts
    CHECK_EQ(t.cpr32(hw::cp::FIFO_WP), 0x00200000U); // 0x..20, 0x..40, wrap to base
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RP), 0x00200000U);
    CHECK_EQ(t.sys.gpu().xf().word(0x0040), 0x00010203U);
    CHECK_EQ(t.sys.gpu().xf().word(0x004F), 0x3C3D3E3FU);
    // data really went around the ring in RAM
    t.gp(bp_cmd(0x47, 0x00ABCD));
    t.gp_pad();
    u8 b = 0;
    CHECK(t.bus().read8(0x00200001U, b) == mem::BusStatus::Ok);
    CHECK_EQ(b, 0x47U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_WP), 0x00200020U);
    t.wait(500);
    CHECK_EQ(t.pe_token(), 0xABCDU);
}

TEST_CASE(gpu_fifo_breakpoint_and_watermarks) {
    FifoRig t;
    t.setup(0x00300000U, 0x003000E0U, hw::cp::CTRL_LINK | hw::cp::CTRL_READ | hw::cp::CTRL_BP_ENABLE | hw::cp::CTRL_BP_INT);
    t.cp32(hw::cp::FIFO_BP, 0x00300040U);
    t.cp32(hw::cp::FIFO_HI_WM, 0x40U);
    for (u32 i = 0; i < 4; ++i) { t.gp(bp_cmd(0x47, 0x100U + i)); t.gp_pad(); }
    // the GPU stopped at the breakpoint after two blocks; two remain queued
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RP), 0x00300040U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0x40U);
    CHECK_EQ(t.sys.gpu().bp().reg(0x47), 0x101U);
    u16 st = t.cpr(hw::cp::STATUS);
    CHECK_EQ(st & (hw::cp::ST_BREAKPOINT | hw::cp::ST_CMD_IDLE | hw::cp::ST_READ_IDLE), hw::cp::ST_BREAKPOINT | hw::cp::ST_CMD_IDLE);
    CHECK_EQ(t.sys.pi().causes() & hw::pi::CAUSE_CP, hw::pi::CAUSE_CP);
    // one more block: distance 0x60 > high watermark 0x40; overflow interrupt only once enabled
    t.gp(bp_cmd(0x47, 0x104U)); t.gp_pad();
    st = t.cpr(hw::cp::STATUS);
    CHECK_EQ(st & hw::cp::ST_OVERFLOW, hw::cp::ST_OVERFLOW);
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK | hw::cp::CTRL_READ | hw::cp::CTRL_BP_ENABLE | hw::cp::CTRL_OVF_INT);
    // still at the breakpoint (no BP interrupt enabled now) but overflow raises it
    CHECK_EQ(t.sys.pi().causes() & hw::pi::CAUSE_CP, hw::pi::CAUSE_CP);
    // releasing the breakpoint drains the FIFO
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK | hw::cp::CTRL_READ | hw::cp::CTRL_OVF_INT);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0U);
    CHECK_EQ(t.sys.gpu().bp().reg(0x47), 0x104U);
    CHECK_EQ(t.sys.pi().causes() & hw::pi::CAUSE_CP, 0U);
    // underflow: low watermark above the (zero) distance, interrupt enabled
    t.cp32(hw::cp::FIFO_LO_WM, 0x20U);
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK | hw::cp::CTRL_READ | hw::cp::CTRL_UNDF_INT);
    CHECK_EQ(t.cpr(hw::cp::STATUS) & hw::cp::ST_UNDERFLOW, hw::cp::ST_UNDERFLOW);
    CHECK_EQ(t.sys.pi().causes() & hw::pi::CAUSE_CP, hw::pi::CAUSE_CP);
    // reads disabled gate the interrupt
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK | hw::cp::CTRL_UNDF_INT);
    CHECK_EQ(t.sys.pi().causes() & hw::pi::CAUSE_CP, 0U);
}

TEST_CASE(gpu_fifo_unlinked_and_reads_disabled) {
    FifoRig t;
    t.setup(0x00400000U, 0x00400100U, hw::cp::CTRL_READ); // not linked
    t.gp(bp_cmd(0x47, 7)); t.gp_pad();
    CHECK_EQ(t.cpr32(hw::cp::FIFO_WP), 0x00400000U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0U);
    CHECK_EQ(t.sys.gpu().bp().reg(0x47), 0U);
    // linked but reads disabled: data queues, then runs when reads are enabled
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK);
    t.gp(bp_cmd(0x47, 8)); t.gp_pad();
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0x20U);
    CHECK_EQ(t.sys.gpu().bp().reg(0x47), 0U);
    // the unlinked burst advanced only the PI pointer, so the linked one was
    // written at base + 0x20 while the CP read pointer is still at the base:
    // the GPU reads the older block first (token 7), then the new one (token 8)
    CHECK_EQ(t.cpr32(hw::cp::FIFO_WP), 0x00400020U);
    t.cp32(hw::cp::FIFO_RW_DIST, 0x40U);
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK | hw::cp::CTRL_READ);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RP), 0x00400040U);
    CHECK_EQ(t.sys.gpu().bp().reg(0x47), 8U);
}

TEST_CASE(gpu_fifo_read_pointer_wraps_only_at_end) {
    FifoRig t;
    t.setup(0x00800000U, 0x00800020U, hw::cp::CTRL_LINK);
    t.cp32(hw::cp::FIFO_RP, 0x00800040U); // beyond the end
    t.cp32(hw::cp::FIFO_RW_DIST, 0x20U);
    t.cpw(hw::cp::CTRL, hw::cp::CTRL_LINK | hw::cp::CTRL_READ);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RP), 0x00800060U);
    CHECK_EQ(t.cpr32(hw::cp::FIFO_RW_DIST), 0U);
}

TEST_CASE(gpu_display_list_and_indexed_load) {
    FifoRig t;
    t.setup(0x00500000U, 0x00500100U);
    // display list at 0x00600000: CP load into array base 12, BP token, padded to 32
    std::vector<u8> dl = {0x08, 0xAC, 0x00, 0x61, 0x00, 0x00};
    auto tok = bp_cmd(0x47, 0x00BEEF);
    dl.insert(dl.end(), tok.begin(), tok.end());
    dl.resize(32, 0x00);
    CHECK(t.bus().dma_write(0x00600000U, dl) == mem::BusStatus::Ok);
    // XF words for the indexed load: array 12 base 0x00610000 (set by the DL), stride 0x10, index 2
    std::vector<u8> words = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04};
    CHECK(t.bus().dma_write(0x00610020U, words) == mem::BusStatus::Ok);
    t.gp({0x40, 0x80, 0x60, 0x00, 0x1F, 0x00, 0x00, 0x00, 0x3F}); // virtual-looking address & size masked
    t.gp({0x08, 0xBC, 0x00, 0x00, 0x00, 0x10});                      // stride of array 12
    t.gp({0x20, 0x00, 0x02, 0x10, 0x12});                            // index 2, 2 words, XF 0x012
    t.gp_pad();
    CHECK(!t.sys.command_processor().stopped());
    CHECK_EQ(t.sys.gpu().cp().array_base[12], 0x00610000U);
    CHECK_EQ(t.sys.gpu().xf().word(0x012), 0xDEADBEEFU);
    CHECK_EQ(t.sys.gpu().xf().word(0x013), 0x01020304U);
    t.wait(500);
    CHECK_EQ(t.pe_token(), 0xBEEFU);
}

TEST_CASE(gpu_tmem_preload) {
    mem::Bus bus;
    CHECK(bus.map_ram("MEM1", 0, 0x100000U));
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::PixelEngine pe{sched, pi};
    std::vector<u8> ram(256);
    for (u32 i = 0; i < 256; ++i) ram[i] = static_cast<u8>(i);
    CHECK(bus.dma_write(0x4000, ram) == mem::BusStatus::Ok);
    auto cat = [](std::initializer_list<std::vector<u8>> parts) {
        std::vector<u8> r;
        for (const auto& p : parts) r.insert(r.end(), p.begin(), p.end());
        return r;
    };
    {
        // type 0, 3 lines from 0x4000 to TMEM line 2 (byte 64); 0 = GX_TexModeSync, nothing copied
        gpu::Gpu g{bus, pe};
        CHECK(g.feed(cat({bp_cmd(0x60, 0x4000 >> 5), bp_cmd(0x61, 2), bp_cmd(0x63, 0)})));
        CHECK_EQ(g.tmem()[64], 0U);
        CHECK(g.feed(bp_cmd(0x63, 3)));
        CHECK_EQ(g.tmem()[64], 0U); CHECK_EQ(g.tmem()[65], 1U); CHECK_EQ(g.tmem()[64 + 95], 95U);
        CHECK_EQ(g.tmem()[64 + 96], 0U); CHECK_EQ(g.tmem()[63], 0U);
        // cut at the end of TMEM: line 0x7FFF, 4 lines -> only 32 bytes
        CHECK(g.feed(cat({bp_cmd(0x61, 0x7FFF), bp_cmd(0x63, 4)})));
        CHECK_EQ(g.tmem()[(1U << 20) - 1U], 31U);
        // starting outside TMEM: nothing, no error
        CHECK(g.feed(cat({bp_cmd(0x61, 0x8000), bp_cmd(0x63, 4)})));
    }
    {
        // type 3 (RGBA8): lines alternate even (line 1) / odd (line 0x4000) banks
        gpu::Gpu g{bus, pe};
        CHECK(g.feed(cat({bp_cmd(0x60, 0x4000 >> 5), bp_cmd(0x61, 1), bp_cmd(0x62, 0x4000), bp_cmd(0x63, (3U << 15) | 2U)})));
        const u32 even = 32, odd = 0x4000U * 32U;
        CHECK_EQ(g.tmem()[even], 0U); CHECK_EQ(g.tmem()[even + 31], 31U);
        CHECK_EQ(g.tmem()[odd], 32U); CHECK_EQ(g.tmem()[odd + 31], 63U);
        CHECK_EQ(g.tmem()[even + 32], 64U); CHECK_EQ(g.tmem()[odd + 32], 96U);
        CHECK_EQ(g.tmem()[even + 64], 0U); CHECK_EQ(g.tmem()[odd + 64], 0U);
        // a bank reaching the end of TMEM stops the loop
        CHECK(g.feed(cat({bp_cmd(0x60, 0x4080 >> 5), bp_cmd(0x61, 0x7FFF), bp_cmd(0x62, 0x10), bp_cmd(0x63, (3U << 15) | 2U)})));
        CHECK_EQ(g.tmem()[(1U << 20) - 32U], 128U); CHECK_EQ(g.tmem()[0x10U * 32U], 160U);
        CHECK_EQ(g.tmem()[0x11U * 32U], 0U);
        // likewise the odd bank: odd line 0x7FFF, even line 0x20 -> one pair only
        CHECK(g.feed(cat({bp_cmd(0x60, 0x4000 >> 5), bp_cmd(0x61, 0x20), bp_cmd(0x62, 0x7FFF), bp_cmd(0x63, (3U << 15) | 2U)})));
        CHECK_EQ(g.tmem()[0x20U * 32U], 0U); CHECK_EQ(g.tmem()[(1U << 20) - 32U], 32U);
        CHECK_EQ(g.tmem()[0x21U * 32U], 0U);
        // source outside RAM stops the GPU
        CHECK(!g.feed(cat({bp_cmd(0x60, 0x100000U >> 5), bp_cmd(0x61, 0), bp_cmd(0x63, (3U << 15) | 1U)})));
        CHECK(g.error().find("outside RAM") != std::string::npos);
    }
}

TEST_CASE(gpu_stops_are_reported) {
    // an unhandled draw-done value: the system run stops with the reason
    FifoRig t;
    t.setup(0x00700000U, 0x00700100U);
    t.gp(bp_cmd(0x45, 0x000001));
    t.gp_pad();
    CHECK(t.sys.command_processor().stopped());
    CHECK(t.sys.command_processor().stop_reason().find("draw done") != std::string::npos);
    const auto r = t.sys.run(10);
    CHECK(r.device_stop);
    CHECK(r.device_error.find("GPU:") != std::string::npos);
    // other stops, through Gpu::feed directly
    mem::Bus bus;
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::PixelEngine pe{sched, pi};
    const std::vector<std::pair<std::vector<u8>, const char*>> cases = {
        {[] { auto a = bp_cmd(0x43, 5); const auto c = bp_cmd(0x52, 0); a.insert(a.end(), c.begin(), c.end()); return a; }(), "pixel format 5"}, {bp_cmd(0x65, 1U << 10), "TLUT load"}, {bp_cmd(0x63, 1), "TMEM preload reads 32 bytes at 00000000 outside RAM"},
        {bp_cmd(0x45, 0x000001), "draw done"}, {{0x40, 0, 0, 0, 0, 0, 0, 0, 0x20}, "outside RAM"},
        {{0x08, 0xC0, 0, 0, 0, 0}, "not handled"}, {{0x18}, "opcode 18"},
    };
    unsigned bad = 0;
    for (const auto& [bytes, what] : cases) {
        gpu::Gpu g{bus, pe};
        if (g.feed(bytes) || !g.failed() || g.error().find(what) == std::string::npos) ++bad;
        if (g.feed(std::vector<u8>{0x00})) ++bad; // stays stopped
    }
    CHECK_EQ(bad, 0U);
    // draw done looks only at the low byte: 0x000102 is accepted
    {
        gpu::Gpu g{bus, pe};
        CHECK(g.feed(bp_cmd(0x45, 0x000102)));
    }
    // a display list whose last command is cut is an error
    CHECK(bus.map_ram("MEM1", 0, 0x100000U));
    std::vector<u8> dl(32, 0x00);
    dl[31] = 0x61;
    CHECK(bus.dma_write(0x1000, dl) == mem::BusStatus::Ok);
    gpu::Gpu g{bus, pe};
    CHECK(!g.feed(std::vector<u8>{0x40, 0, 0, 0x10, 0, 0, 0, 0, 0x20}));
    CHECK(g.error().find("ends inside a command") != std::string::npos);
    // GPU time: NOP 6, CP 12, BP 12, XF 18+6n, 0x01 1
    gpu::Gpu c{bus, pe};
    CHECK(c.feed(std::vector<u8>{0x00, 0x01, 0x08, 0x50, 0, 0, 0, 0, 0x61, 0x00, 0, 0, 0, 0x10, 0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0, 0, 0, 0}));
    CHECK_EQ(c.cycles(), 6U + 1U + 12U + 12U + 18U + 12U);
}

namespace {
std::array<u8, 4> texel(TexFormat f, const std::vector<u8>& d, u32 s, u32 t, u32 w, const std::vector<u8>& tl = {},
                        TlutFormat tf = TlutFormat::IA8) {
    std::array<u8, 4> c{};
    std::string e;
    CHECK(decode_texel(f, d, s, t, w, tl, tf, c.data(), e));
    return c;
}
u32 pack(const std::array<u8, 4>& c) { return (u32{c[0]} << 24) | (u32{c[1]} << 16) | (u32{c[2]} << 8) | c[3]; }
} // namespace

TEST_CASE(gpu_texture_formats_hand_values) {
    std::vector<u8> d(64, 0);
    // RGB565 tile: texels 0-3 of row 0
    d[0] = 0xF8; d[1] = 0x00; d[2] = 0x07; d[3] = 0xE0; d[4] = 0x00; d[5] = 0x1F; d[6] = 0x84; d[7] = 0x10;
    CHECK_EQ(pack(texel(TexFormat::RGB565, d, 0, 0, 4)), 0xFF0000FFU);
    CHECK_EQ(pack(texel(TexFormat::RGB565, d, 1, 0, 4)), 0x00FF00FFU);
    CHECK_EQ(pack(texel(TexFormat::RGB565, d, 2, 0, 4)), 0x0000FFFFU);
    CHECK_EQ(pack(texel(TexFormat::RGB565, d, 3, 0, 4)), 0x848284FFU);
    // RGB5A3
    d[0] = 0xFC; d[1] = 0x00; d[2] = 0x7F; d[3] = 0x00; d[4] = 0x3A; d[5] = 0xBC;
    CHECK_EQ(pack(texel(TexFormat::RGB5A3, d, 0, 0, 4)), 0xFF0000FFU);
    CHECK_EQ(pack(texel(TexFormat::RGB5A3, d, 1, 0, 4)), 0xFF0000FFU); // A3 = 7 -> 255
    CHECK_EQ(pack(texel(TexFormat::RGB5A3, d, 2, 0, 4)), 0xAABBCC6DU); // A3 = 3 -> 0x6D
    // IA8: first byte alpha; IA4: high nibble alpha
    d[0] = 0x80; d[1] = 0x40;
    CHECK_EQ(pack(texel(TexFormat::IA8, d, 0, 0, 4)), 0x40404080U);
    d[0] = 0x5A;
    CHECK_EQ(pack(texel(TexFormat::IA4, d, 0, 0, 8)), 0xAAAAAA55U);
    // I4: high nibble first; I8
    d[0] = 0x1F;
    CHECK_EQ(pack(texel(TexFormat::I4, d, 0, 0, 8)), 0x11111111U);
    CHECK_EQ(pack(texel(TexFormat::I4, d, 1, 0, 8)), 0xFFFFFFFFU);
    d[9] = 0x77; // I8 tile row 1, x 1
    CHECK_EQ(pack(texel(TexFormat::I8, d, 1, 1, 8)), 0x77777777U);
    // RGBA8: texel 5 (x 1, y 1): A,R at 10,11; G,B at 42,43
    d[10] = 0x11; d[11] = 0x22; d[42] = 0x33; d[43] = 0x44;
    CHECK_EQ(pack(texel(TexFormat::RGBA8, d, 1, 1, 4)), 0x22334411U);
    // second tile of a two-tile-wide RGB565 image starts at byte 32
    std::vector<u8> two(64, 0);
    two[32] = 0x00; two[33] = 0x1F;
    CHECK_EQ(pack(texel(TexFormat::RGB565, two, 4, 0, 8)), 0x0000FFFFU);
}

TEST_CASE(gpu_texture_cmpr_and_palettes) {
    std::vector<u8> d(32, 0);
    // top-left DXT1 block: red > blue, selectors 0,1,2,3 on row 0
    d[0] = 0xF8; d[1] = 0x00; d[2] = 0x00; d[3] = 0x1F; d[4] = 0x1B;
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 0, 0, 8)), 0xFF0000FFU);
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 1, 0, 8)), 0x0000FFFFU);
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 2, 0, 8)), 0x9F005FFFU); // (0*3+255*5)>>3 = 159, (255*3)>>3 = 95
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 3, 0, 8)), 0x5F009FFFU);
    // bottom-right block (bytes 24-31): blue <= red -> midpoint and transparent
    d[24] = 0x00; d[25] = 0x1F; d[26] = 0xF8; d[27] = 0x00; d[28 + 1] = 0xB0; // row 1: sel 2, 3, 0, 0
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 4, 5, 8)), 0x7F0080FFU);
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 5, 5, 8)), 0x7F008000U);
    CHECK_EQ(pack(texel(TexFormat::CMPR, d, 6, 5, 8)), 0x0000FFFFU);
    // equal endpoints take the "c1 <= c2" branch: colour 3 is transparent
    std::vector<u8> eq(32, 0);
    eq[0] = 0x12; eq[1] = 0x34; eq[2] = 0x12; eq[3] = 0x34; eq[4] = 0x03; // x3 selects colour 3
    CHECK_EQ(texel(TexFormat::CMPR, eq, 3, 0, 8)[3], 0U);
    // palettes
    const std::vector<u8> tl = {0xFF, 0x80, 0xF8, 0x00, 0x00, 0x1F, 0x3A, 0xBC};
    std::vector<u8> c(32, 0);
    c[0] = 0x03;
    CHECK_EQ(pack(texel(TexFormat::C8, c, 0, 0, 8, tl, TlutFormat::RGB5A3)), 0xAABBCC6DU);
    CHECK_EQ(pack(texel(TexFormat::C8, c, 0, 0, 8, tl, TlutFormat::IA8)), 0xBCBCBC3AU);
    CHECK_EQ(pack(texel(TexFormat::C4, c, 1, 0, 8, tl, TlutFormat::RGB565)), 0x3955E7FFU); // low nibble 3: 0x3ABC
    c[0] = 0x40; c[1] = 0x02;                                                              // 0x4002 & 0x3FFF = 2
    CHECK_EQ(pack(texel(TexFormat::C14X2, c, 0, 0, 4, tl, TlutFormat::RGB565)), 0x0000FFFFU);
    std::array<u8, 4> o{};
    std::string e;
    c[0] = 0x09;
    CHECK(!decode_texel(TexFormat::C8, c, 0, 0, 8, tl, TlutFormat::IA8, o.data(), e));
    CHECK(e.find("outside") != std::string::npos);
    std::vector<u8> out;
    CHECK(!decode_texture(TexFormat::RGBA8, c, 4, 4, {}, TlutFormat::IA8, out, e));
    CHECK(e.find("shorter") != std::string::npos);
    CHECK(!tex_format_valid(7)); CHECK(!tex_format_valid(11)); CHECK(!tex_format_valid(15)); CHECK(tex_format_valid(14));
    CHECK_EQ(tex_size_bytes(TexFormat::CMPR, 9, 9), 4U * 32U);
    CHECK_EQ(tex_size_bytes(TexFormat::RGBA8, 4, 4), 64U);
    CHECK_EQ(tex_size_bytes(TexFormat::I8, 16, 4), 64U);
}

TEST_CASE(gpu_texture_tile_walk_matches_texel_addressing) {
    // every format, odd sizes, pseudo-random data: the sequential tile decoder and
    // the per-texel decoder are independent code and must agree on every texel
    const TexFormat fmts[] = {TexFormat::I4, TexFormat::I8, TexFormat::IA4, TexFormat::IA8, TexFormat::RGB565,
                              TexFormat::RGB5A3, TexFormat::RGBA8, TexFormat::C4, TexFormat::C8, TexFormat::C14X2,
                              TexFormat::CMPR};
    std::vector<u8> tl(2 * 16384);
    for (std::size_t i = 0; i < tl.size(); ++i) tl[i] = static_cast<u8>(i * 7U + 3U);
    unsigned bad = 0, checked = 0;
    u32 seed = 12345;
    for (TexFormat f : fmts) {
        for (const auto& wh : {std::pair<u32, u32>{13, 11}, {8, 8}, {1, 1}, {17, 4}}) {
            const u32 w = wh.first, h = wh.second;
            std::vector<u8> d(tex_size_bytes(f, w, h));
            for (auto& x : d) { seed = seed * 1103515245U + 12345U; x = static_cast<u8>(seed >> 16); }
            for (TlutFormat tf : {TlutFormat::IA8, TlutFormat::RGB565, TlutFormat::RGB5A3}) {
                std::vector<u8> img;
                std::string e;
                if (!decode_texture(f, d, w, h, tl, tf, img, e)) { ++bad; continue; }
                for (u32 y = 0; y < h; ++y)
                    for (u32 x = 0; x < w; ++x) {
                        u8 c[4];
                        if (!decode_texel(f, d, x, y, w, tl, tf, c, e)) { ++bad; continue; }
                        const u8* o = img.data() + (y * w + x) * 4U;
                        if (o[0] != c[0] || o[1] != c[1] || o[2] != c[2] || o[3] != c[3]) ++bad;
                        ++checked;
                    }
                if (!tex_format_paletted(f)) break;
            }
        }
    }
    CHECK_EQ(bad, 0U);
    CHECK(checked > 3000U);
}

#ifdef ORCA_PROGRAMS_DIR
TEST_CASE(gpu_suite_program_fifo_tokens) {
    std::ifstream in(ORCA_PROGRAMS_DIR "/gpu_suite.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    std::string err;
    CHECK(sys.load_executable(file, err));
    const auto r = sys.run(50000000ULL);
    if (r.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    gpu_suite stopped: %s (%s) pc=%08X dev=%s\n", ppc::to_string(r.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc, r.device_error.c_str());
    CHECK(r.stop == ppc::StepResult::Halted);
    CHECK_EQ(port.exit_code, 0U);
    const auto& L = port.log;
    for (u32 v : L) std::printf("    gpu_suite value %08X (%u)\n", v, v);
    CHECK_EQ(L.size(), 9U);
    if (L.size() != 9U) return;
    CHECK_EQ(L[0], 0x1234U);  // token reached the PE
    CHECK_EQ(L[1], 0x20U);    // CP write pointer one burst further
    CHECK_EQ(L[2], 0x20U);    // read pointer followed
    CHECK_EQ(L[3], 0U);       // distance 0
    CHECK_EQ(L[4], 0U);       // token without interrupt: no cause
    CHECK_EQ(L[5], 0x5678U);  // second token
    CHECK_EQ(L[6], 0x600U);   // PE token + PE finish causes
    CHECK_EQ(L[7], 0U);       // acknowledged
    CHECK_EQ(L[8], 0x0CU);    // read idle + command idle
}

TEST_CASE(gpu_draw_program_to_xfb) {
    // a PowerPC program draws through the FIFO, copies the EFB to an XFB and sets up the VI;
    // the frame read back through the VI registers is decoded to RGB
    std::ifstream in(ORCA_PROGRAMS_DIR "/gpu_draw.dol", std::ios::binary);
    const std::vector<u8> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(!file.empty());
    system::System sys({});
    test::TestPort port;
    port.cpu = &sys.cpu();
    sys.bus().map_mmio("test-port", test::TestPort::kPhysical, 0x1000, port);
    std::string err;
    CHECK(sys.load_executable(file, err));
    const auto r = sys.run(200000000ULL);
    if (r.stop != ppc::StepResult::Halted)
        std::fprintf(stderr, "    gpu_draw stopped: %s (%s) pc=%08X dev=%s\n", ppc::to_string(r.stop),
                     sys.cpu().last_stop().reason.c_str(), sys.cpu().last_stop().pc, r.device_error.c_str());
    CHECK(r.stop == ppc::StepResult::Halted);
    CHECK_EQ(port.exit_code, 0U);
    CHECK_EQ(port.log.size(), 2U);
    if (port.log.size() != 2U) return;
    CHECK_EQ(port.log[0], 0x400U); // draw done reached the CPU
    CHECK_EQ(port.log[1], 0x600DU);
    CHECK_EQ(sys.gpu().copies(), 1U);
    const hw::XfbField f = sys.vi().xfb_field(false);
    CHECK_EQ(f.address, 0x00400000U); CHECK_EQ(f.width, 640U); CHECK_EQ(f.stride, 1280U); CHECK_EQ(f.height, 480U);
    std::vector<u8> raw(static_cast<std::size_t>(f.stride) * f.height), rgba;
    CHECK(sys.bus().dma_read(f.address, raw) == mem::BusStatus::Ok);
    CHECK(video::decode_xfb(raw, f.width, f.height, f.stride, rgba));
    auto px = [&](u32 x, u32 y) { const std::size_t o = (static_cast<std::size_t>(y) * 640U + x) * 4U; return std::array<u8, 3>{rgba[o], rgba[o + 1], rgba[o + 2]}; };
    // inside the rectangle: red (255, 0, 0 after YUV and back); the triangle: green; elsewhere black
    // (the copy happens before the clear)
    const auto red = px(200, 100), green = px(410, 210), bg = px(10, 10), edge = px(99, 100);
    CHECK(red[0] >= 250U && red[1] <= 3U && red[2] <= 3U);
    CHECK(green[1] >= 250U && green[0] <= 3U && green[2] <= 3U);
    CHECK(bg[0] == 0U && bg[1] == 0U && bg[2] == 0U);
    CHECK(edge[1] == 0U && edge[2] <= 3U);
    // the EFB was cleared to dark blue after the copy
    CHECK_EQ(sys.gpu().renderer().efb().color24(200, 100), 0x000040U);
}
#endif

TEST_MAIN("gpu")
