/* Draws a red rectangle and a green triangle through the real FIFO, copies the EFB to an XFB
 * in RAM with a clear, and points the VI at it. The host test reads the XFB back through the
 * VI registers and converts it to RGB. Command values follow the GX register layout
 * (see tests/test_gpu_sw.cpp "Scene" for the same setup written from the host). */
#include "common.h"

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define GP8 (*(volatile uint8_t *)0xCC008000u)
#define GP16 (*(volatile uint16_t *)0xCC008000u)
#define GP32 (*(volatile uint32_t *)0xCC008000u)
#define PI_INTSR REG32(0xCC003000u)
#define CP_CTRL REG16(0xCC000002u)
#define PE_CTRL REG16(0xCC00100Au)
#define CAUSE_PE_FINISH 0x400u
#define XFB_ADDR 0x00400000u

static uint8_t fifo[8192] __attribute__((aligned(32)));
static uint32_t gp_count;

static void cp_reg32(uint32_t off, uint32_t v) {
    REG16(0xCC000000u + off) = (uint16_t)v;
    REG16(0xCC000002u + off) = (uint16_t)(v >> 16);
}
static void w8(uint32_t v) { GP8 = (uint8_t)v; gp_count += 1; }
static void w16(uint32_t v) { GP16 = (uint16_t)v; gp_count += 2; }
static void w32(uint32_t v) { GP32 = v; gp_count += 4; }
static void wf(float f) { uint32_t b; memcpy(&b, &f, 4); w32(b); }
static void bp(uint32_t reg, uint32_t v) { w8(0x61); w32((reg << 24) | (v & 0xFFFFFFu)); }
static void cp(uint32_t reg, uint32_t v) { w8(0x08); w8(reg); w32(v); }
static void xf(uint32_t addr, uint32_t v) { w8(0x10); w32(addr); w32(v); }
static void xff(uint32_t addr, const float *v, uint32_t n) {
    w8(0x10); w32(((n - 1u) << 16) | addr);
    for (uint32_t i = 0; i < n; ++i) wf(v[i]);
}
static void vtx(float x, float y, uint32_t rgba) { wf(x); wf(y); wf(-0.5f); w32(rgba); }
static void flush_gp(void) { while (gp_count & 31u) w8(0); }

int program_main(void) {
    const uint32_t base = (uint32_t)fifo & 0x03FFFFFFu;
    const uint32_t end = base + sizeof fifo - 32u;
    for (uint32_t a = (uint32_t)fifo; a < (uint32_t)fifo + sizeof fifo; a += 32u) __asm__ volatile("dcbi 0,%0" ::"r"(a));
    CP_CTRL = 0;
    REG32(0xCC00300Cu) = base;
    REG32(0xCC003010u) = end;
    REG32(0xCC003014u) = base;
    cp_reg32(0x20, base);
    cp_reg32(0x24, end);
    cp_reg32(0x28, 0x1FE0u);
    cp_reg32(0x2C, 0);
    cp_reg32(0x30, 0);
    cp_reg32(0x34, base);
    cp_reg32(0x38, base);
    CP_CTRL = 0x0011;
    PE_CTRL = 0x0002;          /* finish interrupt enable */

    /* transform: matrix 0 identity, orthographic projection mapping x 0..640, y 0..528 */
    static const float ident[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    static const float proj[6] = {1.0f / 320.0f, -1.0f, 1.0f / 264.0f, -1.0f, 1.0f, 0.0f};
    static const float vp[6] = {320.0f, 264.0f, 16777215.0f, 662.0f, 606.0f, 16777215.0f};
    xff(0x0000, ident, 12);
    xff(0x1020, proj, 6);
    xf(0x1026, 1);             /* orthographic */
    xff(0x101A, vp, 6);
    xf(0x100E, 1); xf(0x1010, 1); /* channel 0 colour/alpha from the vertex */
    /* raster state */
    bp(0x00, 1u << 4);         /* GENMODE: 1 colour, 1 stage */
    bp(0x20, (342u << 12) | 342u);
    bp(0x21, ((342u + 639u) << 12) | (342u + 527u));
    bp(0x59, 171u | (171u << 10));
    bp(0x41, 0x18);            /* colour + alpha update */
    bp(0x43, 0);               /* RGB8_Z24 */
    bp(0xC0, 10u | (15u << 4) | (15u << 8) | (15u << 12) | (1u << 19));
    bp(0xC1, (5u << 4) | (7u << 7) | (7u << 10) | (7u << 13) | (1u << 19));
    bp(0xF3, (7u << 16) | (7u << 19));
    bp(0xF6, 1u << 2);
    bp(0xF7, 2u | (3u << 2));
    bp(0x53, (21u << 12) | (22u << 18)); /* copy filter: GX default (no vertical filter) */
    bp(0x54, 21u);
    /* vertex format 0: position xyz float, colour 0 RGBA8, both direct */
    cp(0x50, (1u << 9) | (1u << 13));
    cp(0x70, 1u | (4u << 1) | (5u << 14));
    /* a red rectangle (100..300, 50..150) and a green triangle */
    w8(0x80); w16(4);
    vtx(100, 50, 0xFF0000FFu); vtx(300, 50, 0xFF0000FFu); vtx(300, 150, 0xFF0000FFu); vtx(100, 150, 0xFF0000FFu);
    w8(0x90); w16(3);
    vtx(400, 200, 0x00FF00FFu); vtx(500, 200, 0x00FF00FFu); vtx(400, 300, 0x00FF00FFu);
    /* copy 640x480 to the XFB (1280 bytes per line) and clear to dark blue */
    bp(0x4F, 0x0000u);         /* clear A, R */
    bp(0x50, 0x0040u);         /* clear G, B */
    bp(0x51, 0xFFFFFFu);
    bp(0x49, 0);
    bp(0x4A, 639u | (479u << 10));
    bp(0x4B, XFB_ADDR >> 5);
    bp(0x4D, 1280u >> 5);
    bp(0x4E, 0x100u);
    bp(0x52, 0x4000u | 0x800u);
    bp(0x45, 2);               /* draw done */
    flush_gp();
    uint32_t spins = 0;
    while ((PI_INTSR & CAUSE_PE_FINISH) == 0 && spins < 1000000u) ++spins;
    const uint32_t done = PI_INTSR & CAUSE_PE_FINISH;
    PE_CTRL = 0x0002 | 0x0008;
    /* VI: top field at the XFB, 640 pixels (WPL 40), 1280 bytes per line (STD 40), 480 lines.
     * Set before the first report, so that orca_run (no test port) can still dump the frame. */
    REG16(0xCC002048u) = (uint16_t)((40u << 8) | 40u);
    REG16(0xCC002000u) = (uint16_t)((480u << 4) | 6u);
    REG32(0xCC00201Cu) = 0x10000000u | (XFB_ADDR >> 5);
    orca_emit(done);
    orca_emit(0x600Du);
    return 0;
}
