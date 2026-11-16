/* ARAM DMA driven like the GameCube ARAM drivers: main memory -> ARAM ->
 * another buffer, completion by polling the DSP CSR busy bit (bit 9),
 * then ARINT acknowledged without touching the other status bits. */
#include "common.h"

#define REG16(a) (*(volatile uint16_t *)(a))
#define REG32(a) (*(volatile uint32_t *)(a))
#define DSP_CSR REG16(0xCC00500Au)
#define PI_INTSR REG32(0xCC003000u)
#define CSR_ARINT 0x0020u
#define CSR_DMABUSY 0x0200u
#define CAUSE_DSP 0x00000040u

static uint32_t src[256] __attribute__((aligned(32)));
static uint32_t dst[256] __attribute__((aligned(32)));

static void flush(const void *p, uint32_t len) {
    for (uint32_t a = (uint32_t)p; a < (uint32_t)p + len; a += 32u) __asm__ volatile("dcbf 0,%0" ::"r"(a) : "memory");
    __asm__ volatile("sync" ::: "memory");
}
static void invalidate(const void *p, uint32_t len) {
    for (uint32_t a = (uint32_t)p; a < (uint32_t)p + len; a += 32u) __asm__ volatile("dcbi 0,%0" ::"r"(a) : "memory");
}

static uint32_t aram_dma(uint32_t to_main, uint32_t mm, uint32_t ar, uint32_t len) {
    REG16(0xCC005020u) = (uint16_t)(mm >> 16);
    REG16(0xCC005022u) = (uint16_t)mm;
    REG16(0xCC005024u) = (uint16_t)(ar >> 16);
    REG16(0xCC005026u) = (uint16_t)ar;
    REG16(0xCC005028u) = (uint16_t)((to_main << 15) | (len >> 16));
    REG16(0xCC00502Au) = (uint16_t)len; /* starts the transfer */
    uint32_t busy = (DSP_CSR & CSR_DMABUSY) ? 1u : 0u;
    while (DSP_CSR & CSR_DMABUSY) { }
    return busy;
}

static void ack_arint(void) {
    /* write 1 only to ARINT: keep AIDINT/DSPINT (bits 3, 7) and PIINT (bit 1) as 0 */
    uint16_t v = DSP_CSR;
    v &= (uint16_t)~(0x0002u | 0x0008u | 0x0080u);
    DSP_CSR = v;
}

int program_main(void) {
    for (uint32_t i = 0; i < 256; ++i) { src[i] = i; dst[i] = 0xFFFFFFFFu; }
    flush(src, sizeof src);
    DSP_CSR = 0x0004u | 0x0040u; /* HALT, ARINTMASK */

    uint32_t busy = aram_dma(0, (uint32_t)src & 0x3FFFFFFFu, 0x00100000u, sizeof src);
    orca_emit(busy);
    orca_emit((DSP_CSR & CSR_DMABUSY) ? 1u : 0u);
    orca_emit(DSP_CSR & CSR_ARINT);
    ack_arint();

    invalidate(dst, sizeof dst);
    aram_dma(1, (uint32_t)dst & 0x3FFFFFFFu, 0x00100000u, sizeof dst);
    ack_arint();
    uint32_t bad = 0;
    for (uint32_t i = 0; i < 256; ++i)
        if (dst[i] != i) ++bad;
    orca_emit(bad);
    orca_emit(REG16(0xCC005026u));       /* ARADDR low after the second DMA */
    orca_emit(REG16(0xCC00502Au));       /* count */
    orca_emit(PI_INTSR & CAUSE_DSP);
    return 0;
}
