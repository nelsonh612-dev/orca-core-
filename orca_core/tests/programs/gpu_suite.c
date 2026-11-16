/* The CPU-GPU FIFO set up the way the GX library does it: one ring buffer
 * shared by the PI (CPU side) and the CP (GPU side), linked, reads enabled.
 * Commands go through the write-gather pipe at 0xCC008000; the program waits
 * for the PE token and the draw-done interrupt cause by polling. */
#include "common.h"

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define GP8 (*(volatile uint8_t *)0xCC008000u)
#define GP32 (*(volatile uint32_t *)0xCC008000u)
#define PI_INTSR REG32(0xCC003000u)
#define CP_STATUS REG16(0xCC000000u)
#define CP_CTRL REG16(0xCC000002u)
#define PE_CTRL REG16(0xCC00100Au)
#define PE_TOKEN REG16(0xCC00100Eu)
#define CAUSE_PE_TOKEN 0x200u
#define CAUSE_PE_FINISH 0x400u

static uint8_t fifo[4096] __attribute__((aligned(32)));
static uint32_t gp_count;

static void cp_reg32(uint32_t off, uint32_t v) {
    REG16(0xCC000000u + off) = (uint16_t)v;
    REG16(0xCC000002u + off) = (uint16_t)(v >> 16);
}
static uint32_t cp_read32(uint32_t off) {
    return REG16(0xCC000000u + off) | ((uint32_t)REG16(0xCC000002u + off) << 16);
}
static void bp(uint32_t v) { GP8 = 0x61; GP32 = v; gp_count += 5; }
static void flush_gp(void) { while (gp_count & 31u) { GP8 = 0; ++gp_count; } }

int program_main(void) {
    const uint32_t base = (uint32_t)fifo & 0x03FFFFFFu;
    const uint32_t end = base + sizeof fifo - 32u;
    for (uint32_t a = (uint32_t)fifo; a < (uint32_t)fifo + sizeof fifo; a += 32u) __asm__ volatile("dcbi 0,%0" ::"r"(a));
    CP_CTRL = 0;
    REG32(0xCC00300Cu) = base; /* PI FIFO base, end, write pointer */
    REG32(0xCC003010u) = end;
    REG32(0xCC003014u) = base;
    cp_reg32(0x20, base);      /* CP base, end, watermarks, distance, pointers */
    cp_reg32(0x24, end);
    cp_reg32(0x28, 0x0FE0u);   /* high watermark */
    cp_reg32(0x2C, 0);
    cp_reg32(0x30, 0);
    cp_reg32(0x34, base);
    cp_reg32(0x38, base);
    CP_CTRL = 0x0011;          /* link + GP read */
    PE_CTRL = 0x0003;          /* token and finish interrupt enables */

    bp(0x47001234u);           /* token without interrupt */
    flush_gp();
    uint32_t spins = 0;
    while (PE_TOKEN != 0x1234u && spins < 100000u) ++spins;
    orca_emit(PE_TOKEN);
    orca_emit(cp_read32(0x34) - base); /* write pointer moved one burst */
    orca_emit(cp_read32(0x38) - base); /* read pointer followed */
    orca_emit(cp_read32(0x30));        /* nothing left to read */
    orca_emit(PI_INTSR & CAUSE_PE_TOKEN);

    bp(0x48005678u);           /* token with interrupt */
    bp(0x45000002u);           /* draw done */
    flush_gp();
    spins = 0;
    while ((PI_INTSR & (CAUSE_PE_TOKEN | CAUSE_PE_FINISH)) != (CAUSE_PE_TOKEN | CAUSE_PE_FINISH) && spins < 100000u) ++spins;
    orca_emit(PE_TOKEN);
    orca_emit(PI_INTSR & (CAUSE_PE_TOKEN | CAUSE_PE_FINISH));
    PE_CTRL = 0x0003 | 0x0004 | 0x0008; /* acknowledge both */
    orca_emit(PI_INTSR & (CAUSE_PE_TOKEN | CAUSE_PE_FINISH));
    orca_emit(CP_STATUS & 0x1Fu);      /* read idle + command idle */
    return 0;
}
