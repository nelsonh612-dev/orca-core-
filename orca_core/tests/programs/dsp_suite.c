/* Boots a DSP microcode the way SDK titles start the DSP (DSP manual,
 * "Initialization"): the code is placed at physical 0x01000000, the DSP
 * control register is written with 0x08AD (reset, halt, bit 11) and then
 * released. The microcode sends one mail, waits for a mail from the CPU,
 * answers with the low half + 1, raises DIRQ and halts. */
#include "common.h"

#define REG16(a) (*(volatile uint16_t *)(a))
#define DSP_MBH_IN REG16(0xCC005000u)   /* CPU -> DSP */
#define DSP_MBL_IN REG16(0xCC005002u)
#define DSP_MBH_OUT REG16(0xCC005004u)  /* DSP -> CPU */
#define DSP_MBL_OUT REG16(0xCC005006u)
#define DSP_CSR REG16(0xCC00500Au)

static const uint16_t ucode[] = {
    0x16FC, 0xDCD1,          /* SI   @DMBH, #0xDCD1 */
    0x16FD, 0x0000,          /* SI   @DMBL, #0x0000 */
    0x00DE, 0xFFFE,          /* 4: LR $ac0.m, @CMBH */
    0x02C0, 0x8000,          /* ANDCF $ac0.m, #0x8000 */
    0x029C, 0x0004,          /* JLNZ 4 (no mail yet) */
    0x00DE, 0xFFFE,          /* LR $ac0.m, @CMBH */
    0x00DF, 0xFFFF,          /* LR $ac1.m, @CMBL (clears the pending bit) */
    0x0240, 0x7FFF,          /* ANDI $ac0.m, #0x7FFF */
    0x0501,                  /* ADDIS $ac1, #1 */
    0x00FE, 0xFFFC,          /* SR @DMBH, $ac0.m */
    0x00FF, 0xFFFD,          /* SR @DMBL, $ac1.m */
    0x16FB, 0x0001,          /* SI @DIRQ, #1 */
    0x0021,                  /* HALT */
};

int program_main(void) {
    volatile uint16_t *dst = (volatile uint16_t *)0xC1000000u; /* uncached view of 0x01000000 */
    for (uint32_t i = 0; i < sizeof ucode / sizeof ucode[0]; ++i) dst[i] = ucode[i];
    DSP_CSR = 0x08ADu;
    DSP_CSR = 0x0000u;
    while ((DSP_MBH_OUT & 0x8000u) == 0u) { }
    orca_emit(DSP_MBH_OUT & 0x7FFFu);
    orca_emit(DSP_MBL_OUT);
    DSP_MBH_IN = 0x9234u;
    DSP_MBL_IN = 0x5678u;
    while ((DSP_MBH_OUT & 0x8000u) == 0u) { }
    orca_emit(DSP_MBH_OUT & 0x7FFFu);
    orca_emit(DSP_MBL_OUT);
    orca_emit(DSP_CSR & 0x0080u);
    while ((DSP_CSR & 0x0004u) == 0u) { }
    orca_emit(DSP_CSR & 0x0004u);
    return 0;
}
