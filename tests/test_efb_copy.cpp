// EFB copies and clears (BP 0x52). Expected values worked out by hand from
// the rules listed in sw/efb_copy.hpp (Dolphin's encoder, XFB encoder and
// ClearScreen, read as information only).
#include "orca/gpu/sw/efb_copy.hpp"
#include "orca/gpu/gpu.hpp"
#include "orca/hw/pixel_engine.hpp"
#include "orca/video/xfb.hpp"
#include "test_framework.hpp"

#include <array>
#include <string>
#include <vector>

using namespace orca;
using namespace orca::gpu;
using namespace orca::gpu::sw;

namespace {

constexpr u32 kDest = 0x10000U;

// Trigger value: tf = BP target format field (bits 3-6).
u32 trig(u32 tf, u32 extra = 0) { return (tf << 3) | extra; }

struct Rig {
    mem::Bus bus;
    BpMemory bp;
    Efb efb;
    CopyResult res;
    std::string err;
    Rig() {
        CHECK(bus.map_ram("MEM1", 0, 0x100000U));
        bp.load(bp::EFB_ADDR, kDest >> 5);
        bp.load(bp::EFB_STRIDE, 2);                          // 64 bytes between rows of blocks
        bp.load(bp::COPY_FILTER0, (21U << 12) | (22U << 18)); // GX default: identity
        bp.load(bp::COPY_FILTER1, 21U);
        bp.load(bp::COPY_YSCALE, 0x100);
    }
    void rect(u32 x, u32 y, u32 w, u32 h) {
        bp.load(bp::EFB_TL, x | (y << 10));
        bp.load(bp::EFB_WH, (w - 1U) | ((h - 1U) << 10));
    }
    void format(u32 f) { bp.load(bp::ZCOMPARE, f); }
    bool copy(u32 t) { err.clear(); return efb_copy(t, bp, efb, bus, 0x03FFFFFFU, res, err); }
    u8 ram(u32 off) {
        std::array<u8, 1> b{};
        CHECK(bus.dma_read(kDest + off, b) == mem::BusStatus::Ok);
        return b[0];
    }
    void zero_ram() { std::vector<u8> z(4096, 0xEE); CHECK(bus.dma_write(kDest, z) == mem::BusStatus::Ok); }
};

} // namespace

TEST_CASE(efb_copy_rgba8_and_565_from_rgb8) {
    Rig t;
    t.format(0);
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) t.efb.set_color24(x, y, ((10 + x) << 16) | ((20 + y) << 8) | (30 + x + y));
    t.rect(0, 0, 4, 4);
    CHECK(t.copy(trig(12))); // RGBA8
    CHECK(t.res.copied); CHECK(!t.res.xfb); CHECK(!t.res.cleared);
    CHECK_EQ(t.res.address, kDest); CHECK_EQ(t.res.bytes, 64U);
    CHECK_EQ(t.res.width, 4U); CHECK_EQ(t.res.height, 4U);
    // texel (3,2) = index 11: AR at 22/23, GB at 54/55
    CHECK_EQ(t.ram(22), 0xFFU); CHECK_EQ(t.ram(23), 13U);
    CHECK_EQ(t.ram(54), 22U); CHECK_EQ(t.ram(55), 35U);
    CHECK_EQ(t.ram(0), 0xFFU); CHECK_EQ(t.ram(1), 10U); CHECK_EQ(t.ram(32), 20U); CHECK_EQ(t.ram(33), 30U);
    // RGB565: (FF, 80, 08) -> F800 | 0400 | 0001
    t.efb.set_color24(0, 0, 0xFF8008U);
    CHECK(t.copy(trig(8)));
    CHECK_EQ(t.ram(0), 0xFCU); CHECK_EQ(t.ram(1), 0x01U);
    CHECK_EQ(t.res.bytes, 32U);
    // RGB5A3 from RGB8 is always 1:5:5:5
    CHECK(t.copy(trig(10)));
    CHECK_EQ(t.ram(0), 0xFEU); CHECK_EQ(t.ram(1), 0x01U); // 8000 | 7C00 | 0200 | 0001
}

TEST_CASE(efb_copy_rgba6_formats) {
    Rig t;
    t.format(1);
    // r6 3F g6 20 b6 10 a6 10 -> RGBA8 FF 82 41 41
    t.efb.set_color24(0, 0, (0x3FU << 18) | (0x20U << 12) | (0x10U << 6) | 0x10U);
    t.rect(0, 0, 4, 4);
    CHECK(t.copy(trig(10))); // RGB5A3, alpha < 224: 4443
    CHECK_EQ(t.ram(0), 0x2FU); CHECK_EQ(t.ram(1), 0x84U);
    CHECK(t.copy(trig(12))); // RGBA8
    CHECK_EQ(t.ram(0), 0x41U); CHECK_EQ(t.ram(1), 0xFFU); CHECK_EQ(t.ram(32), 0x82U); CHECK_EQ(t.ram(33), 0x41U);
    CHECK(t.copy(trig(14))); // A8
    CHECK_EQ(t.ram(0), 0x41U);
    CHECK(t.copy(trig(4)));  // RA4: (a & F0) | (r >> 4)
    CHECK_EQ(t.ram(0), 0x4FU);
    CHECK(t.copy(trig(6)));  // RA8
    CHECK_EQ(t.ram(0), 0x41U); CHECK_EQ(t.ram(1), 0xFFU);
    CHECK(t.copy(trig(7)));  // RG8: G then R
    CHECK_EQ(t.ram(0), 0x82U); CHECK_EQ(t.ram(1), 0xFFU);
    CHECK(t.copy(trig(9)));  // GB8: B then G
    CHECK_EQ(t.ram(0), 0x41U); CHECK_EQ(t.ram(1), 0x82U);
    // alpha 0x38 (a8 = E3 >= 224) -> 1:5:5:5
    t.efb.set_color24(0, 0, (0x3FU << 18) | (0x20U << 12) | (0x10U << 6) | 0x38U);
    CHECK(t.copy(trig(10)));
    CHECK_EQ(t.ram(0), 0xFEU); CHECK_EQ(t.ram(1), 0x08U); // 8000 | 7C00 | 0200 | 0008
}

TEST_CASE(efb_copy_intensity_r4_and_layout) {
    Rig t;
    t.format(0);
    t.rect(0, 0, 8, 4);
    t.efb.set_color24(0, 0, (100U << 16) | (150U << 8) | 200U);
    CHECK(t.copy(trig(1)));                       // R8
    CHECK_EQ(t.ram(0), 100U);
    CHECK(t.copy(trig(1, 0x8000U)));              // intensity without auto conversion: still R
    CHECK_EQ(t.ram(0), 100U);
    CHECK(t.copy(trig(1, 0x18000U)));             // I8: 4096 + 6600 + 19350 + 5000 = 35046 -> 136 + round bit 1
    CHECK_EQ(t.ram(0), 137U);
    CHECK(t.copy(trig(3, 0x18000U)));             // the conversion replaces G with U: 40268 -> 157
    CHECK_EQ(t.ram(0), 157U);
    // R8 block is 8x4: texel (1,1) is byte 9, (7,3) byte 31
    t.efb.set_color24(1, 1, 0x110000U);
    t.efb.set_color24(7, 3, 0x770000U);
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(9), 0x11U); CHECK_EQ(t.ram(31), 0x77U);
    CHECK_EQ(t.res.width, 8U); CHECK_EQ(t.res.height, 4U);
    // R4: 8x8 block, two texels per byte, high nibble first
    t.rect(0, 0, 8, 8);
    t.efb.set_color24(0, 0, 0xAB0000U);
    t.efb.set_color24(1, 0, 0x5C0000U);
    CHECK(t.copy(trig(0)));
    CHECK_EQ(t.ram(0), 0xA5U);
    CHECK_EQ(t.res.bytes, 32U);
    // two blocks across, two rows of blocks: second row at the stride (64)
    t.zero_ram();
    t.rect(0, 0, 16, 8);
    t.efb.set_color24(8, 0, 0x990000U);
    t.efb.set_color24(0, 4, 0x880000U);
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(32), 0x99U);  // block 1 of row 0
    CHECK_EQ(t.ram(64), 0x88U);  // block 0 of row 1
    CHECK_EQ(t.ram(128), 0xEEU); // nothing past the last row
    CHECK_EQ(t.res.bytes, 128U);
}

TEST_CASE(efb_copy_half_scale) {
    Rig t;
    t.format(1);
    t.rect(0, 0, 8, 8);
    const u32 r6[4] = {10, 20, 30, 40};
    const u32 xy[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (unsigned i = 0; i < 4; ++i) t.efb.set_color24(xy[i][0], xy[i][1], (r6[i] << 18) | 0x3FU);
    CHECK(t.copy(trig(1, 0x200U))); // R8, half: 6-bit sum 100 -> 100 + 1
    CHECK_EQ(t.ram(0), 101U);
    CHECK_EQ(t.res.width, 8U); CHECK_EQ(t.res.height, 4U);
    CHECK(t.copy(trig(14, 0x200U))); // A8: 4 * 63 = 252 -> 255
    CHECK_EQ(t.ram(0), 255U);
    // 8-bit EFB: plain average
    t.format(0);
    t.efb.set_color24(0, 0, 0x000003U); t.efb.set_color24(1, 0, 0x000004U);
    t.efb.set_color24(0, 1, 0x000004U); t.efb.set_color24(1, 1, 0x000004U);
    CHECK(t.copy(trig(5, 0x200U))); // B8: 15 >> 2
    CHECK_EQ(t.ram(0), 3U);
    // texel (1,0) uses pixels (2..3, 0..1)
    t.efb.set_color24(2, 0, 0x000008U); t.efb.set_color24(3, 0, 0x000008U);
    t.efb.set_color24(2, 1, 0x000008U); t.efb.set_color24(3, 1, 0x000010U);
    CHECK(t.copy(trig(5, 0x200U)));
    CHECK_EQ(t.ram(1), 10U);
    // texel (0,1) uses rows 2..3; B8 blocks are 8 texels wide (texel (5,0) is byte 5)
    t.efb.set_color24(0, 2, 0x000020U); t.efb.set_color24(1, 2, 0x000020U);
    t.efb.set_color24(0, 3, 0x000020U); t.efb.set_color24(1, 3, 0x000020U);
    t.efb.set_color24(10, 0, 0x000040U); t.efb.set_color24(11, 0, 0x000040U);
    t.efb.set_color24(10, 1, 0x000040U); t.efb.set_color24(11, 1, 0x000040U);
    t.rect(0, 0, 16, 8);
    CHECK(t.copy(trig(5, 0x200U)));
    CHECK_EQ(t.ram(8), 0x20U);
    CHECK_EQ(t.ram(5), 0x40U);
    // block count from the halved size: 17 wide -> 8 texels -> one R8 block (not two)
    t.rect(0, 0, 17, 8);
    CHECK(t.copy(trig(1, 0x200U)));
    CHECK_EQ(t.res.width, 8U); CHECK_EQ(t.res.bytes, 32U);
    t.rect(0, 0, 16, 8);
    // RGB5A3 boundary: 6-bit alphas 55, 55, 55, 56 -> 221 + 3 = 224 -> 1:5:5:5
    t.format(1);
    t.rect(0, 0, 8, 8);
    const u32 al[4] = {55, 55, 55, 56};
    for (unsigned i = 0; i < 4; ++i) t.efb.set_color24(xy[i][0], xy[i][1], al[i]);
    CHECK(t.copy(trig(10, 0x200U)));
    CHECK_EQ(t.ram(0), 0x80U); CHECK_EQ(t.ram(1), 0x00U);
    t.efb.set_color24(1, 1, 55U);                // 220 + 3 = 223 -> 4:4:4:3
    CHECK(t.copy(trig(10, 0x200U)));
    CHECK_EQ(t.ram(0), 0x60U); CHECK_EQ(t.ram(1), 0x00U);
    // half scale with the copy filter active on RGBA6: colours are filtered 8-bit values,
    // averaged with >> 2 (40 * 4 >> 2 = 40), alpha keeps the 6-bit box filter
    for (u32 y = 0; y < 4; ++y)
        for (u32 x = 0; x < 4; ++x) t.efb.set_color24(x, y, (10U << 18) | 63U);
    t.bp.load(bp::COPY_FILTER0, 8U | (8U << 6) | (10U << 12) | (12U << 18));
    t.bp.load(bp::COPY_FILTER1, 10U | (8U << 6) | (8U << 12));
    CHECK(t.copy(trig(6, 0x200U)));              // RA8: A, R
    CHECK_EQ(t.ram(0), 255U); CHECK_EQ(t.ram(1), 40U);
}

TEST_CASE(efb_copy_depth) {
    Rig t;
    t.format(3);
    t.efb.set_color24(0, 0, 0xABCDEFU); // ignored: depth copies read Z
    t.efb.set_depth24(0, 0, 0x123456U);
    t.rect(0, 0, 4, 4);
    CHECK(t.copy(trig(12)));
    CHECK_EQ(t.ram(0), 0xFFU); CHECK_EQ(t.ram(1), 0x12U); CHECK_EQ(t.ram(32), 0x34U); CHECK_EQ(t.ram(33), 0x56U);
    CHECK(t.copy(trig(1, 0x18000U))); CHECK_EQ(t.ram(0), 55U); // intensity of the Z bytes: 14142 -> 55
    CHECK(t.copy(trig(3))); CHECK_EQ(t.ram(0), 0x34U);
    CHECK(t.copy(trig(5))); CHECK_EQ(t.ram(0), 0x56U);
    CHECK(t.copy(trig(7))); CHECK_EQ(t.ram(0), 0x34U); CHECK_EQ(t.ram(1), 0x12U);
    CHECK(t.copy(trig(9))); CHECK_EQ(t.ram(0), 0x56U); CHECK_EQ(t.ram(1), 0x34U);
    CHECK(t.copy(trig(0))); CHECK_EQ(t.ram(0), 0x10U); // R4: high nibble of 0x12, texel 1 is Z 0
    CHECK(t.copy(trig(8)));   // RGB565 of the Z bytes: 1000 | 01A0 | 000A
    CHECK_EQ(t.ram(0), 0x11U); CHECK_EQ(t.ram(1), 0xAAU);
    CHECK(t.copy(trig(14))); CHECK_EQ(t.ram(0), 0xFFU); // A8: depth alpha is 255
    t.format(0);
    CHECK(!t.copy(trig(11)));  // real format 13
    CHECK(t.err.find("format 13") != std::string::npos);
    t.format(5);
    CHECK(!t.copy(trig(12)));
    CHECK(t.err.find("pixel format 5") != std::string::npos);
}

TEST_CASE(efb_copy_rectangles_and_padding) {
    Rig t;
    t.format(0);
    // width 3: the 4th texel of the block is the EFB pixel next to the rectangle
    t.efb.set_color24(3, 0, 0x420000U);
    t.rect(0, 0, 3, 1);
    CHECK(t.copy(trig(12)));
    CHECK_EQ(t.ram(7), 0x42U);
    // past column 639 the last column repeats (clamp-to-edge sampling)
    t.efb.set_color24(639, 0, 0x330000U);
    t.efb.set_color24(638, 0, 0x220000U);
    t.rect(638, 0, 2, 1);
    CHECK(t.copy(trig(12)));
    CHECK_EQ(t.ram(1), 0x22U); CHECK_EQ(t.ram(3), 0x33U); CHECK_EQ(t.ram(5), 0x33U); CHECK_EQ(t.ram(7), 0x33U);
    // oversized rectangle is clamped to the EFB
    t.rect(636, 0, 8, 4);
    CHECK(t.copy(trig(12)));
    CHECK_EQ(t.res.width, 4U);
    CHECK_EQ(t.res.bytes, 64U);
    // past the last row the last row repeats
    t.rect(636, 527, 4, 1);
    t.efb.set_color24(636, 527, 0x010203U);
    CHECK(t.copy(trig(12)));
    CHECK_EQ(t.ram(1), 0x01U);
    CHECK_EQ(t.ram(9), 0x01U); CHECK_EQ(t.ram(41), 0x03U); // texel (0,1): R, B
    // rectangle starting outside: nothing at all, not even the clear
    t.zero_ram();
    t.bp.load(bp::BLENDMODE, 8);
    t.rect(640, 0, 4, 4);
    CHECK(t.copy(trig(12, 0x800U)));
    CHECK(t.res.ignored); CHECK(!t.res.copied); CHECK(!t.res.cleared);
    CHECK_EQ(t.ram(0), 0xEEU);
    // stride 0: no copy, but the clear still happens
    t.bp.load(bp::EFB_STRIDE, 0);
    t.rect(0, 0, 2, 2);
    t.bp.load(bp::CLEAR_AR, 0x0011U);
    t.bp.load(bp::CLEAR_GB, 0x2233U);
    CHECK(t.copy(trig(12, 0x800U)));
    CHECK(!t.res.copied); CHECK(t.res.cleared);
    CHECK_EQ(t.ram(0), 0xEEU);
    CHECK_EQ(t.efb.color24(1, 1), 0x112233U);
    // copy outside RAM stops
    t.bp.load(bp::EFB_STRIDE, 2);
    t.bp.load(bp::EFB_ADDR, 0x200000U >> 5);
    CHECK(!t.copy(trig(12)));
    CHECK(t.err.find("outside RAM") != std::string::npos);
}

TEST_CASE(efb_clear_formats) {
    Rig t;
    t.bp.load(bp::EFB_STRIDE, 0);
    t.bp.load(bp::CLEAR_AR, 0xAA11U);
    t.bp.load(bp::CLEAR_GB, 0x2233U);
    t.bp.load(bp::CLEAR_Z, 0xABCDEFU);
    t.rect(2, 2, 2, 2);
    for (u32 y = 0; y < 6; ++y)
        for (u32 x = 0; x < 6; ++x) { t.efb.set_color24(x, y, 0x777777U); t.efb.set_depth24(x, y, 0x555555U); }
    // RGB8: colour only (z update off, alpha meaningless)
    t.format(0);
    t.bp.load(bp::BLENDMODE, 0x18);
    CHECK(t.copy(0x800U));
    CHECK_EQ(t.efb.color24(2, 2), 0x112233U); CHECK_EQ(t.efb.color24(3, 3), 0x112233U);
    CHECK_EQ(t.efb.color24(1, 2), 0x777777U); CHECK_EQ(t.efb.color24(4, 3), 0x777777U);
    CHECK_EQ(t.efb.depth24(2, 2), 0x555555U);
    // RGBA6 with colour and alpha, and z
    t.format(1);
    t.bp.load(bp::ZMODE, 0x10);
    CHECK(t.copy(0x800U));
    CHECK_EQ(t.efb.color24(2, 2), 0x10832AU);
    CHECK_EQ(t.efb.depth24(3, 2), 0xABCDEFU);
    // RGBA6 alpha only
    t.efb.set_color24(2, 2, 0xFFFFFFU);
    t.bp.load(bp::BLENDMODE, 0x10);
    CHECK(t.copy(0x800U));
    CHECK_EQ(t.efb.color24(2, 2), 0xFFFFEAU);
    // RGB565_Z16: colour and depth quantized
    t.format(2);
    t.bp.load(bp::BLENDMODE, 0x08);
    CHECK(t.copy(0x800U));
    CHECK_EQ(t.efb.color24(2, 2), 0x102031U);
    CHECK_EQ(t.efb.depth24(2, 2), 0xABCDABU);
    // nothing enabled: nothing changes
    t.efb.set_color24(2, 2, 0x777777U);
    t.bp.load(bp::BLENDMODE, 0);
    t.bp.load(bp::ZMODE, 0);
    CHECK(t.copy(0x800U));
    CHECK_EQ(t.efb.color24(2, 2), 0x777777U);
    CHECK(t.res.cleared);
}

TEST_CASE(efb_copy_filter_and_gamma) {
    Rig t;
    t.format(0);
    t.efb.set_color24(0, 0, 0x000000U);
    t.efb.set_color24(0, 1, 101U << 16);
    t.efb.set_color24(0, 2, 200U << 16);
    t.rect(0, 1, 1, 1);
    // identity filter: plain copy
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 101U);
    // deflicker 8,8,10,12,10,8,8: prev 16, current 32, next 16
    t.bp.load(bp::COPY_FILTER0, 8U | (8U << 6) | (10U << 12) | (12U << 18));
    t.bp.load(bp::COPY_FILTER1, 10U | (8U << 6) | (8U << 12));
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 100U);                    // (0 + 3232 + 3200) >> 6
    CHECK(t.copy(trig(1, 1U)));                  // clamp top: previous row = row 1
    CHECK_EQ(t.ram(0), 125U);                    // (1616 + 3232 + 3200) >> 6
    CHECK(t.copy(trig(1, 2U)));                  // clamp bottom: next row = row 1
    CHECK_EQ(t.ram(0), 75U);                     // (0 + 3232 + 1616) >> 6
    // brightness: current row weight 128 -> doubled, clamped at 255
    t.bp.load(bp::COPY_FILTER0, (42U << 12) | (43U << 18));
    t.bp.load(bp::COPY_FILTER1, 43U);
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 202U);
    t.rect(0, 2, 1, 1);
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 255U);
    // gamma 2.2 with the identity filter: 128 -> 186
    t.bp.load(bp::COPY_FILTER0, (21U << 12) | (22U << 18));
    t.bp.load(bp::COPY_FILTER1, 21U);
    t.efb.set_color24(0, 2, 128U << 16);
    CHECK(t.copy(trig(1, 2U << 7)));
    CHECK_EQ(t.ram(0), 186U);
    CHECK(t.copy(trig(1, 3U << 7)));             // 3 behaves as 2.2
    CHECK_EQ(t.ram(0), 186U);
    // gamma rounds to nearest: 3 -> 33.85 -> 34
    t.efb.set_color24(0, 2, 3U << 16);
    CHECK(t.copy(trig(1, 2U << 7)));
    CHECK_EQ(t.ram(0), 34U);
    // weights summing to 128 or more wrap at 512 before the clamp: 189 * 200 >> 6 = 590 -> 78
    t.bp.load(bp::COPY_FILTER0, (63U << 12) | (63U << 18));
    t.bp.load(bp::COPY_FILTER1, 63U);
    t.efb.set_color24(0, 2, 200U << 16);
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 78U);
    t.efb.set_color24(0, 2, 100U << 16);   // 295 -> clamped to 255
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 255U);
    t.bp.load(bp::COPY_FILTER0, (21U << 12) | (22U << 18));
    t.bp.load(bp::COPY_FILTER1, 21U);
    // rows of the block below the rectangle: without clamp they come from the EFB, with
    // clamp bottom they repeat the last row of the rectangle (rect row 5, R8 block of 4 rows)
    t.efb.set_color24(0, 5, 0x500000U); t.efb.set_color24(0, 6, 0x600000U);
    t.rect(0, 5, 8, 1);
    CHECK(t.copy(trig(1)));
    CHECK_EQ(t.ram(0), 0x50U); CHECK_EQ(t.ram(8), 0x60U);
    CHECK(t.copy(trig(1, 2U)));
    CHECK_EQ(t.ram(0), 0x50U); CHECK_EQ(t.ram(8), 0x50U);
    t.rect(0, 2, 1, 1);
    // depth copies go through the filter and the gamma too
    t.format(3);
    t.efb.set_depth24(0, 2, 0x800000U);
    CHECK(t.copy(trig(1, 2U << 7)));
    CHECK_EQ(t.ram(0), 186U);
}

TEST_CASE(efb_copy_xfb) {
    Rig t;
    t.format(0);
    t.bp.load(bp::EFB_STRIDE, 640U * 2U >> 5);
    for (u32 x = 0; x < 4; ++x) t.efb.set_color24(x, 0, 0xFF0000U); // red row
    t.efb.set_color24(0, 1, 0xFFFFFFU);
    t.efb.set_color24(1, 1, 0x000000U);
    t.rect(0, 0, 4, 2);
    CHECK(t.copy(0x4000U));
    CHECK(t.res.xfb); CHECK(t.res.copied);
    CHECK_EQ(t.res.width, 4U); CHECK_EQ(t.res.height, 2U);
    CHECK_EQ(t.res.stride, 1280U); CHECK_EQ(t.res.bytes, 16U);
    // BT.601 red: Y 82, Cb 90, Cr 240
    CHECK_EQ(t.ram(0), 82U); CHECK_EQ(t.ram(1), 90U); CHECK_EQ(t.ram(2), 82U); CHECK_EQ(t.ram(3), 240U);
    // row 1: white then black: Y 235 and 16, chroma 128
    CHECK_EQ(t.ram(1280), 235U); CHECK_EQ(t.ram(1281), 128U); CHECK_EQ(t.ram(1282), 16U); CHECK_EQ(t.ram(1283), 128U);
    // chroma filter across pixels: pixels 2,3 of row 1 red, pixel 1 black, pixel 4 (padding: border repeat)
    t.efb.set_color24(2, 1, 0xFF0000U);
    t.efb.set_color24(3, 1, 0xFF0000U);
    CHECK(t.copy(0x4000U));
    // U for pair (2,3): (U1 + 2*U2 + U3) >> 2 = (0 - 76 - 38) >> 2 = -29 (floor(-28.5))
    CHECK_EQ(t.ram(1284), 82U); CHECK_EQ(t.ram(1285), 99U);
    // V: (0 + 224 + 112) >> 2 = 84
    CHECK_EQ(t.ram(1287), 212U);
    // gamma 2.2 on grey 128 -> 186 -> Y = 160 + 16
    t.efb.set_color24(0, 0, 0x808080U); t.efb.set_color24(1, 0, 0x808080U);
    t.rect(0, 0, 2, 1);
    CHECK(t.copy(0x4000U | (2U << 7)));
    CHECK_EQ(t.ram(0), 176U);
    // clamp top / bottom choose the rows used by the vertical filter (weights 32 above, 32 below)
    t.bp.load(bp::COPY_FILTER0, 16U | (16U << 6));
    t.bp.load(bp::COPY_FILTER1, (16U << 6) | (16U << 12));
    t.efb.set_color24(0, 4, 0xFFFFFFU); t.efb.set_color24(1, 4, 0xFFFFFFU); // row above: white
    t.efb.set_color24(0, 5, 0x000000U); t.efb.set_color24(1, 5, 0x000000U); // copied row: black
    t.efb.set_color24(0, 6, 0x000000U); t.efb.set_color24(1, 6, 0x000000U); // row below: black
    t.rect(0, 5, 2, 1);
    CHECK(t.copy(0x4000U));            // (255*32 + 0*32) >> 6 = 127 grey
    CHECK_EQ(t.ram(0), 125U);          // Y = (220*127 = 27940) >> 8 = 109 (no rounding bit) + 16
    CHECK(t.copy(0x4000U | 1U));       // clamp top: the row above is the copied row -> black
    CHECK_EQ(t.ram(0), 16U);
    t.bp.load(bp::COPY_FILTER0, (21U << 12) | (22U << 18));
    t.bp.load(bp::COPY_FILTER1, 21U);
    // limits
    t.rect(1, 0, 2, 1);
    CHECK(!t.copy(0x4000U));
    CHECK(t.err.find("odd column") != std::string::npos);
    // vertical scale (CopyRegion.h, nearest row = round(row * src_h / dst_h)); rows white,
    // black, red, white
    const u32 rowc[4] = {0xFFFFFFU, 0x000000U, 0xFF0000U, 0xFFFFFFU};
    for (u32 y = 0; y < 4; ++y) { t.efb.set_color24(0, y, rowc[y]); t.efb.set_color24(1, y, rowc[y]); }
    t.zero_ram();
    t.rect(0, 0, 2, 4);
    t.bp.load(bp::COPY_YSCALE, 0x80);  // 0.5: lines 1 + 3 * 0.5 = 2; rows 0 and round(1 * 2) = 2
    CHECK(t.copy(0x4000U));
    CHECK_EQ(t.res.height, 2U);
    CHECK_EQ(t.ram(0), 235U); CHECK_EQ(t.ram(1280), 82U); CHECK_EQ(t.ram(2560), 0xEEU);
    t.zero_ram();
    t.rect(0, 0, 2, 2);
    CHECK(t.copy(0x4000U | 0x400U));   // 256/128 = 2: lines 1 + 1 * 2 = 3; dst 4: rows 0, round(0.5) = 1, 1
    CHECK_EQ(t.res.height, 3U);
    CHECK_EQ(t.ram(0), 235U); CHECK_EQ(t.ram(1280), 16U); CHECK_EQ(t.ram(2560), 16U); CHECK_EQ(t.ram(3840), 0xEEU);
    t.rect(0, 0, 2, 1);
    t.bp.load(bp::COPY_YSCALE, 0x80);
    CHECK(!t.copy(0x4000U));           // one row at 0.5 -> 0 rows
    CHECK(t.err.find("scaled to 0 lines") != std::string::npos);
    t.bp.load(bp::COPY_YSCALE, 0);
    CHECK(!t.copy(0x4000U));
    CHECK(t.err.find("y scale of 0") != std::string::npos);
    CHECK(!t.copy(0x4000U | 0x400U));
    CHECK(t.err.find("y scale of 0") != std::string::npos);
    t.bp.load(bp::COPY_YSCALE, 0x100);
    CHECK(t.copy(0x4000U | 0x400U)); // 256/256 = 1
}

TEST_CASE(xfb_to_rgba) {
    // white (235,128,235,128), red (82,90,82,240), black (16,128,16,128) on one line, then a
    // second line 12 bytes later
    const std::vector<u8> x = {235, 128, 235, 128, 82, 90, 82, 240, 16, 128, 16, 128, 82, 90, 16, 240};
    std::vector<u8> rgba;
    CHECK(orca::video::decode_xfb(x, 6, 1, 12, rgba));
    CHECK_EQ(rgba.size(), 24U);
    // white: 1.164 * 219 = 254.9 -> 254
    CHECK_EQ(rgba[0], 254U); CHECK_EQ(rgba[1], 254U); CHECK_EQ(rgba[2], 254U); CHECK_EQ(rgba[3], 255U);
    // red: R 76.8 + 178.8 = 255.6 -> 255; G 76.8 + 14.9 - 91.1 = 0.7 -> 0; B 76.8 - 76.6 = 0.2 -> 0
    CHECK_EQ(rgba[8], 255U); CHECK_EQ(rgba[9], 0U); CHECK_EQ(rgba[10], 0U);
    CHECK_EQ(rgba[12], 255U); CHECK_EQ(rgba[16], 0U); CHECK_EQ(rgba[17], 0U);
    // two lines: the second starts at byte 12 (Y 82, U 90, Y 16, V 240): second pixel Y 16 with
    // red chroma: R = 178.75 -> 178, G = 14.9 - 91.1 < 0 -> 0, B = -76.6 -> 0
    CHECK(orca::video::decode_xfb(x, 2, 2, 12, rgba));
    CHECK_EQ(rgba[8], 255U); CHECK_EQ(rgba[12], 178U); CHECK_EQ(rgba[13], 0U); CHECK_EQ(rgba[14], 0U);
    CHECK(!orca::video::decode_xfb(x, 6, 2, 12, rgba)); // too short
    CHECK(!orca::video::decode_xfb(x, 3, 1, 12, rgba)); // odd width
    // round trip through an XFB copy: a grey EFB comes back within 1 of the original
    Rig t;
    t.format(0);
    t.bp.load(bp::EFB_STRIDE, 640U * 2U >> 5);
    for (u32 x0 = 0; x0 < 4; ++x0) t.efb.set_color24(x0, 0, 0x808080U);
    t.rect(0, 0, 4, 1);
    CHECK(t.copy(0x4000U));
    std::array<u8, 8> mem{};
    CHECK(t.bus.dma_read(kDest, mem) == mem::BusStatus::Ok);
    CHECK(orca::video::decode_xfb(mem, 4, 1, 8, rgba));
    for (unsigned c = 0; c < 3; ++c) CHECK(rgba[c] >= 127U && rgba[c] <= 129U);
}

TEST_CASE(efb_copy_through_gpu_commands) {
    mem::Bus bus;
    CHECK(bus.map_ram("MEM1", 0, 0x100000U));
    core::Scheduler sched;
    hw::ProcessorInterface pi{bus, nullptr};
    hw::PixelEngine pe{sched, pi};
    gpu::Gpu g{bus, pe};
    auto bpc = [](u8 reg, u32 v) { return std::vector<u8>{0x61, reg, static_cast<u8>(v >> 16), static_cast<u8>(v >> 8), static_cast<u8>(v)}; };
    std::vector<u8> s;
    for (const auto& c : {bpc(0x43, 0), bpc(0x41, 0x18), bpc(0x40, 0x10), bpc(0x49, 0), bpc(0x4A, 3U | (3U << 10)),
                          bpc(0x4B, kDest >> 5), bpc(0x4D, 2), bpc(0x4F, 0x0012), bpc(0x50, 0x3456), bpc(0x51, 0x00ABCD),
                          bpc(0x53, (21U << 12) | (22U << 18)), bpc(0x54, 21U),
                          bpc(0x52, trig(12, 0x800U)), bpc(0x52, trig(12))})
        s.insert(s.end(), c.begin(), c.end());
    CHECK(g.feed(s));
    CHECK_EQ(g.copies(), 2U);
    // first copy saw the old (zero) EFB, then cleared; the second sees the clear colour
    CHECK_EQ(g.renderer().efb().color24(3, 3), 0x123456U);
    CHECK_EQ(g.renderer().efb().depth24(0, 0), 0x00ABCDU);
    std::array<u8, 4> b{};
    CHECK(bus.dma_read(kDest, b) == mem::BusStatus::Ok);
    CHECK_EQ(b[0], 0xFFU); CHECK_EQ(b[1], 0x12U);
    CHECK(g.last_copy().copied); CHECK(!g.last_copy().cleared);
    // with all copy filter weights 0 (BP reset state) the colour copies as black
    CHECK(g.feed(bpc(0x53, 0)));
    CHECK(g.feed(bpc(0x54, 0)));
    CHECK(g.feed(bpc(0x52, trig(12))));
    CHECK(bus.dma_read(kDest, b) == mem::BusStatus::Ok);
    CHECK_EQ(b[0], 0xFFU); CHECK_EQ(b[1], 0x00U);
    // the mask register applies to BP 52 like any other register
    CHECK(g.feed(bpc(0xFE, 0x000800)));
    CHECK(g.feed(bpc(0x52, 0xFFFFFF)));          // only bit 11 (clear) gets through
    CHECK(g.last_copy().cleared);
}

TEST_MAIN("efb_copy")
