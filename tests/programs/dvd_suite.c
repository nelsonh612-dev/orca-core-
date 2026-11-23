/* Game-side DVD access through the Disc Interface registers, booted from a
 * synthetic disc by the HLE apploader path. The test disc holds the words
 * 0xD15C0000 + i at offset 0x100000. */
#include "common.h"

extern const uint32_t ext_handler[], ext_handler_end[], dec_handler[], dec_handler_end[];
#define REG32(a) (*(volatile uint32_t *)(a))
#define DI(o) REG32(0xCC006000u + (o))
#define PHYS(p) ((uint32_t)(p) & 0x3FFFFFFFu)

static uint32_t buf[16] __attribute__((aligned(32)));
static uint32_t buf2[8] __attribute__((aligned(32)));

static void install(uint32_t vector, const uint32_t *begin, const uint32_t *end) {
    volatile uint32_t *dst = (volatile uint32_t *)(0x80000000u + vector);
    for (const uint32_t *p = begin; p < end; ++p) *dst++ = *p;
    for (uint32_t a = 0x80000000u + vector; a < 0x80000000u + vector + 0x100u; a += 32u)
        __asm__ volatile("dcbst 0,%0\n\tsync\n\ticbi 0,%0" ::"r"(a) : "memory");
    __asm__ volatile("isync" ::: "memory");
}
static uint32_t mfmsr(void) { uint32_t v; __asm__ volatile("mfmsr %0" : "=r"(v)); return v; }
static void mtmsr(uint32_t v) { __asm__ volatile("mtmsr %0\n\tisync" ::"r"(v) : "memory"); }

static void start_read(void *dst, uint32_t length, uint32_t offset) {
    DI(0x14) = PHYS(dst);          /* DIMAR */
    DI(0x18) = length;             /* DILENGTH */
    DI(0x08) = 0xA8000000u;        /* read */
    DI(0x0C) = offset >> 2;
    DI(0x10) = length;
    DI(0x1C) = 3u;                 /* TSTART | DMA */
}

int program_main(void) {
    orca_emit(REG32(0x80000000u));  /* game code copied to low memory by the IPL */
    orca_emit(REG32(0x800000FCu));  /* CPU clock */

    /* 1. polled DMA read */
    DI(0x00) = 0x54u;               /* clear interrupts, all masks off */
    start_read(buf, 0x40, 0x100000);
    while (DI(0x1C) & 1u) { }
    orca_emit(buf[0]); orca_emit(buf[15]);
    orca_emit(DI(0x00) & 0x10u);    /* TCINT */
    orca_emit(DI(0x18));            /* DILENGTH -> 0 */
    orca_emit(DI(0x14) - PHYS(buf));/* DIMAR advanced by the length */
    DI(0x00) = 0x10u;               /* acknowledge TCINT */

    /* 2. interrupt-driven read */
    REG32(0x80002008u) = 0;
    /* Like a real OS, install every vector that can fire before enabling EE:
     * a decrementer request is already pending since boot (DEC 0 -> -1). */
    install(0x500, ext_handler, ext_handler_end);
    install(0x900, dec_handler, dec_handler_end);
    REG32(0xCC003004u) = 0x4u;      /* PI INTMR: DI */
    DI(0x00) = 0x08u;               /* TCINTMASK */
    mtmsr(mfmsr() | 0x8000u);
    start_read(buf2, 0x20, 0x100040);
    while (REG32(0x80002008u) == 0u) { }
    mtmsr(mfmsr() & ~0x8000u);
    orca_emit(REG32(0x80002008u));
    orca_emit(buf2[0]); orca_emit(buf2[7]);
    orca_emit(REG32(0xCC003000u) & 0x4u); /* DI cause cleared by the handler */

    /* 3. read past the end of the disc: drive error (DEINT) */
    DI(0x00) = 0x0u;
    start_read(buf2, 0x20, 0x00200000);
    while (DI(0x1C) & 1u) { }
    orca_emit((DI(0x00) & 0x4u) ? 1u : 0u);
    return 0;
}
