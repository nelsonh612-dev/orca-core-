# Orca Core test programs: minimal start-up code (PowerPC 750, EABI).
# The HLE boot leaves MSR = FP|IR|DR with BAT0 (cached) and DBAT1 (uncached).
# Test port (test harness only): physical 0x0E000000, uncached 0xCE000000.
#   +0 EMIT: store a 32-bit value into the result log
#   +4 EXIT: store the exit code; the harness halts the CPU

        .section .text.start,"ax",@progbits
        .globl _start
_start:
        lis     1, 0x8040               # stack top: 0x80400000
        li      0, 0
        stwu    0, -16(1)
        li      2, 0
        li      13, 0
        bl      program_main
        lis     4, 0xCE00
        stw     3, 4(4)                 # EXIT
1:      b       1b

# Exception handlers copied to the vectors at run time by hardware_suite.c.
# They run in real mode (MSR[IR]=MSR[DR]=0), so they use physical addresses:
# counters at physical 0x2000 (decrementer) and 0x2004 (system call).
        .text
        .globl  dec_handler, dec_handler_end, sc_handler, sc_handler_end, ext_handler, ext_handler_end
dec_handler:
        mtspr   272, 3                  # SPRG0
        mtspr   273, 4                  # SPRG1
        li      3, 0
        lwz     4, 0x2000(3)
        addi    4, 4, 1
        stw     4, 0x2000(3)
        li      4, 2000
        mtspr   22, 4                   # DEC = 2000
        mfspr   3, 272
        mfspr   4, 273
        rfi
dec_handler_end:
sc_handler:
        mtspr   272, 3
        mtspr   273, 4
        li      3, 0
        lwz     4, 0x2004(3)
        addi    4, 4, 1
        stw     4, 0x2004(3)
        mfspr   3, 272
        mfspr   4, 273
        rfi
sc_handler_end:

# External interrupt handler for dvd_suite.c: acknowledges the DI transfer-
# complete interrupt (DISR = TCINT | TCINTMASK: clears TCINT, keeps the mask)
# and counts it at physical 0x2008. Real mode: DI is at physical 0x0C006000.
ext_handler:
        mtspr   272, 3
        mtspr   273, 4
        lis     3, 0x0C00
        ori     3, 3, 0x6000
        li      4, 0x18
        stw     4, 0(3)
        li      3, 0
        lwz     4, 0x2008(3)
        addi    4, 4, 1
        stw     4, 0x2008(3)
        mfspr   3, 272
        mfspr   4, 273
        rfi
ext_handler_end:
