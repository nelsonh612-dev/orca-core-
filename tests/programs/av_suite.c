/* Video, controller and audio interfaces driven like GameCube software does:
 * NTSC VI timing from the documented default register values, display
 * interrupt 0 at the start of the second field, SI transfers and automatic
 * polling of a standard controller, and an AI sample-count match. */
#include "common.h"

extern const uint32_t vi_handler[], vi_handler_end[], dec_handler[], dec_handler_end[];
#define REG32(a) (*(volatile uint32_t *)(a))
#define REG16(a) (*(volatile uint16_t *)(a))
#define VI16(o) REG16(0xCC002000u + (o))
#define VI32(o) REG32(0xCC002000u + (o))
#define SI(o) REG32(0xCC006400u + (o))
#define AI(o) REG32(0xCC006C00u + (o))

static void install(uint32_t vector, const uint32_t *begin, const uint32_t *end) {
    volatile uint32_t *dst = (volatile uint32_t *)(0x80000000u + vector);
    for (const uint32_t *p = begin; p < end; ++p) *dst++ = *p;
    for (uint32_t a = 0x80000000u + vector; a < 0x80000000u + vector + 0x100u; a += 32u)
        __asm__ volatile("dcbst 0,%0\n\tsync\n\ticbi 0,%0" ::"r"(a) : "memory");
    __asm__ volatile("isync" ::: "memory");
}
static uint32_t mfmsr(void) { uint32_t v; __asm__ volatile("mfmsr %0" : "=r"(v)); return v; }
static void mtmsr(uint32_t v) { __asm__ volatile("mtmsr %0\n\tisync" ::"r"(v) : "memory"); }
static uint32_t mftbl(void) {
    uint32_t v;
    __asm__ volatile(".long 0x7C6C42E6\n\tmr %0,3" : "=r"(v) : : "r3");
    return v;
}

static void si_transfer(uint32_t channel, uint32_t word0, uint32_t out_len, uint32_t in_len) {
    SI(0x80) = word0;
    SI(0x34) = (out_len << 16) | (in_len << 8) | (channel << 1) | 1u; /* TSTART */
    while (SI(0x34) & 1u) { }
}

int program_main(void) {
    /* VI: documented NTSC defaults, timing registers first, then DCR[ENB] */
    VI16(0x00) = 0x0F06;            /* VTR */
    VI32(0x04) = 0x476901ADu;       /* HTR0 */
    VI32(0x08) = 0x02EA5140u;       /* HTR1 */
    VI32(0x0C) = 0x00030018u;       /* VTO */
    VI32(0x10) = 0x00020019u;       /* VTE */
    VI32(0x30) = 0x110701AEu;       /* DI0: enabled, VCT 263, HCT 430 */
    VI32(0x34) = 0;                 /* DI1-DI3 off */
    VI32(0x38) = 0;
    VI32(0x3C) = 0;
    VI16(0x02) = 0x0001;            /* DCR: enable, interlaced NTSC */

    /* SI: identify, origin and poll on channel 0; channel 1 is empty */
    si_transfer(0, 0x00000000u, 1, 3);
    orca_emit(SI(0x80) & 0xFFFFFF00u);
    si_transfer(0, 0x41000000u, 1, 10);
    orca_emit(SI(0x80));
    si_transfer(0, 0x40030000u, 3, 8);
    orca_emit(SI(0x80));
    SI(0x34) = 0x80000000u;         /* ack TCINT */
    si_transfer(1, 0x00000000u, 1, 3);
    orca_emit(SI(0x38));            /* SISR: NOREP for channel 1 */
    SI(0x38) = 0x0F0F0F0Fu;         /* clear error bits */

    /* SI automatic polling every 16 lines on channel 0 */
    SI(0x00) = 0x00400300u;
    SI(0x30) = (16u << 16) | 0x80u;
    while ((SI(0x38) & 0x20000000u) == 0u) { }   /* RDST0 */
    orca_emit(SI(0x04));
    (void)SI(0x08);

    /* VI display interrupt: count three, measure the time between them */
    REG32(0x80002010u) = 0;
    install(0x500, vi_handler, vi_handler_end);
    install(0x900, dec_handler, dec_handler_end); /* a decrementer request is pending since boot */
    REG32(0xCC003004u) = 0x100u;    /* PI INTMR: VI */
    mtmsr(mfmsr() | 0x8000u);
    while (REG32(0x80002010u) < 3u) { }
    mtmsr(mfmsr() & ~0x8000u);
    REG32(0xCC003004u) = 0;
    orca_emit(REG32(0x80002010u));
    orca_emit(REG32(0x80002024u) - REG32(0x80002020u));
    orca_emit(REG32(0x80002028u) - REG32(0x80002024u));
    orca_emit(REG32(0x80002014u));

    /* AI: play at 48 kHz (AISFR = 1), wait for the AISCNT == AIIT match (polled) */
    AI(0x0C) = 480u;
    const uint32_t t0 = mftbl();
    AI(0x00) = 0x20u | 0x02u | 0x01u; /* SCRESET | AISFR | PSTAT */
    while ((AI(0x00) & 0x08u) == 0u) { }
    const uint32_t t1 = mftbl();
    orca_emit(1u);
    orca_emit(AI(0x08));
    orca_emit(t1 - t0);
    AI(0x00) = 0x08u;                /* stop, ack */
    return 0;
}
