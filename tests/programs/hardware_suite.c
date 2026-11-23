/* Target-only program: exceptions, interrupts, time base, cache control and
 * the PI FIFO / gather pipe, driven the way GameCube software does it. */
#include "common.h"

extern const uint32_t dec_handler[], dec_handler_end[], sc_handler[], sc_handler_end[];

#define REG32(a) (*(volatile uint32_t *)(a))

static void install(uint32_t vector, const uint32_t *begin, const uint32_t *end) {
    volatile uint32_t *dst = (volatile uint32_t *)(0x80000000u + vector);
    for (const uint32_t *p = begin; p < end; ++p) *dst++ = *p;
    for (uint32_t a = 0x80000000u + vector; a < 0x80000000u + vector + 0x100u; a += 32u)
        __asm__ volatile("dcbst 0,%0\n\tsync\n\ticbi 0,%0" ::"r"(a) : "memory");
    __asm__ volatile("isync" ::: "memory");
}

static uint32_t mfmsr(void) { uint32_t v; __asm__ volatile("mfmsr %0" : "=r"(v)); return v; }
static void mtmsr(uint32_t v) { __asm__ volatile("mtmsr %0\n\tisync" ::"r"(v) : "memory"); }
/* clang assembles "mftb" as the newer "mfspr rD,268" alias; the 750CL documents
 * mftb as XO 371, so the instruction word is emitted directly: mftb r3. */
static uint32_t mftbl(void) {
    uint32_t v;
    __asm__ volatile(".long 0x7C6C42E6\n\tmr %0,3" : "=r"(v) : : "r3");
    return v;
}

int program_main(void) {
    REG32(0x80002000u) = 0; /* decrementer counter */
    REG32(0x80002004u) = 0; /* system call counter */
    install(0x900, dec_handler, dec_handler_end);
    install(0xC00, sc_handler, sc_handler_end);

    /* 1. system calls */
    for (int i = 0; i < 3; ++i) __asm__ volatile("sc" ::: "memory");
    orca_emit(REG32(0x80002004u));

    /* 2. decrementer interrupts with MSR[EE] */
    const uint32_t tb0 = mftbl();
    __asm__ volatile("mtspr 22,%0" ::"r"(2000u));
    mtmsr(mfmsr() | 0x8000u);
    while (REG32(0x80002000u) < 5u) { }
    mtmsr(mfmsr() & ~0x8000u);
    const uint32_t ticks = mftbl() - tb0;
    /* DEC starts at 0 after boot, so it already went 0 -> -1 and a decrementer
     * exception is pending before EE is enabled. Per the PEM that request is
     * only cleared when the exception is taken (writing DEC does not cancel it),
     * so the first interrupt arrives immediately and the next four after one
     * full period each: 4 periods of 2001 ticks in total. */
    orca_emit(REG32(0x80002000u));
    orca_emit(ticks >= 4u * 2001u ? 1u : 0u);
    orca_emit(ticks < 5u * 2001u ? 1u : 0u);
    orca_emit((mfmsr() & 0x30u) == 0x30u ? 1u : 0u); /* rfi restored IR|DR */

    /* 3. PI FIFO + gather pipe: 64 bytes written to 0xCC008000 land in RAM */
    REG32(0xCC00300Cu) = 0x00100000u; /* FIFO base */
    REG32(0xCC003010u) = 0x00100040u; /* FIFO end (64 bytes) */
    REG32(0xCC003014u) = 0x00100000u; /* write pointer */
    for (uint32_t i = 0; i < 16; ++i) REG32(0xCC008000u) = 0xF0000000u | i;
    orca_emit(REG32(0xC0100000u));
    orca_emit(REG32(0xC010003Cu));
    orca_emit(REG32(0xCC003014u)); /* wrapped back to base */

    /* 4. PI interrupt mask register round trip */
    REG32(0xCC003004u) = 0x00000100u;
    orca_emit(REG32(0xCC003004u));
    REG32(0xCC003004u) = 0;
    return 0;
}
