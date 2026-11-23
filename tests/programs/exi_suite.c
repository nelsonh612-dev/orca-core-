/* EXI driven the way GameCube software accesses the RTC/SRAM chip
 * (EXI channel 0, CS1, 8 MHz): a 4-byte command word followed by an
 * immediate or DMA data phase, completion by polling CR.TSTART or the
 * PI EXI interrupt cause. */
#include "common.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define EXI(o) REG32(0xCC006800u + (o))
#define PI_INTSR REG32(0xCC003000u)
#define CSR_SEL_IPL_8MHZ 0x00000130u /* CS1 | CLK=3 (8 MHz) */
#define CSR_TCINTMASK 0x00000004u
#define CSR_TCINT 0x00000008u
#define CAUSE_EXI 0x00000010u

static uint32_t mftbl(void) {
    uint32_t v;
    __asm__ volatile(".long 0x7C6C42E6\n\tmr %0,3" : "=r"(v) : : "r3");
    return v;
}

static void imm(uint32_t cr) { EXI(0x0C) = cr; while (EXI(0x0C) & 1u) { } }
static void imm_write(uint32_t word) { EXI(0x10) = word; imm(0x35u); } /* TLEN=3, write, TSTART */
static uint32_t imm_read(void) { imm(0x31u); return EXI(0x10); }        /* TLEN=3, read, TSTART */

static uint32_t buffer[16] __attribute__((aligned(32)));

int program_main(void) {
    /* 1. RTC counter: command 0x20000000, then 4 bytes in */
    EXI(0x00) = CSR_SEL_IPL_8MHZ;
    imm_write(0x20000000u);
    orca_emit(imm_read());
    EXI(0x00) = 0;

    /* 2. timing of one 4-byte immediate transfer (32 bits at 8 MHz) */
    EXI(0x00) = CSR_SEL_IPL_8MHZ;
    EXI(0x10) = 0x20000000u;
    uint32_t t0 = mftbl();
    imm(0x35u);
    uint32_t t1 = mftbl();
    EXI(0x00) = 0;
    orca_emit(t1 - t0);

    /* 3. SRAM write of the first 4 bytes (write bit 31 | 0x20000100) */
    EXI(0x00) = CSR_SEL_IPL_8MHZ;
    imm_write(0xA0000100u);
    imm_write(0xDEADBEEFu);
    EXI(0x00) = 0;

    /* 4. SRAM read by DMA (64 bytes), completion seen through PI INTSR */
    for (uint32_t a = (uint32_t)buffer; a < (uint32_t)buffer + sizeof buffer; a += 32u)
        __asm__ volatile("dcbi 0,%0" ::"r"(a) : "memory");
    EXI(0x00) = CSR_SEL_IPL_8MHZ | CSR_TCINTMASK;
    imm_write(0x20000100u);
    EXI(0x00) = CSR_SEL_IPL_8MHZ | CSR_TCINTMASK | CSR_TCINT; /* clear TCINT of the command phase */
    EXI(0x04) = (uint32_t)buffer & 0x3FFFFFFFu;
    EXI(0x08) = sizeof buffer;
    EXI(0x0C) = 0x03u; /* DMA, read, TSTART */
    while ((PI_INTSR & CAUSE_EXI) == 0u) { }
    orca_emit(EXI(0x0C));                       /* DMA bit kept, TSTART clear */
    orca_emit(EXI(0x00) & CSR_TCINT);           /* TCINT set */
    EXI(0x00) = CSR_TCINT;                      /* clear TCINT, masks off, deselect */
    orca_emit(PI_INTSR & CAUSE_EXI);            /* PI cause dropped */
    orca_emit(buffer[0]);
    orca_emit(buffer[1]);
    orca_emit(buffer[15]);

    /* 5. RTC write, then read back */
    EXI(0x00) = CSR_SEL_IPL_8MHZ;
    imm_write(0xA0000000u);
    imm_write(0x00000100u);
    EXI(0x00) = 0;
    EXI(0x00) = CSR_SEL_IPL_8MHZ;
    imm_write(0x20000000u);
    orca_emit(imm_read());
    EXI(0x00) = 0;

    /* 6. no memory card in slot A or B: EXT = 0 */
    orca_emit((EXI(0x00) >> 12) & 1u);
    orca_emit((EXI(0x14) >> 12) & 1u);
    return 0;
}
