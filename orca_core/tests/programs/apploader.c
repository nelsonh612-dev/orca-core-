/* A minimal GameCube apploader, written from the documented IPL protocol:
 * the IPL calls entry(&init, &main, &close); then init(report); then main()
 * repeatedly, performing the disc read it requests (*dst, *size, *offset)
 * until main returns 0; finally close() returns the game entry point.
 * This one loads the main DOL named by the disc header (0x420). */
#include <stdint.h>

typedef void (*report_t)(const char *fmt, ...);
static report_t report;
static uint32_t state, dol_offset, section, entry_point;
static uint8_t disc_header[0x40] __attribute__((aligned(32)));
static uint32_t dol[0x40] __attribute__((aligned(32))); /* 0x100-byte DOL header */

static void app_init(report_t r) { report = r; report("Orca test apploader\n"); state = 0; }

static int app_main(void **dst, uint32_t *size, uint32_t *offset) {
    if (state == 0) { /* disc header bytes 0x400-0x43F (contains the DOL offset at 0x420) */
        *dst = disc_header; *size = 0x40; *offset = 0x400; state = 1; return 1;
    }
    if (state == 1) {
        const uint8_t *p = disc_header + 0x20;
        dol_offset = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
        *dst = dol; *size = 0x100; *offset = dol_offset; state = 2; section = 0; return 1;
    }
    if (state == 2) {
        while (section < 18) {
            const uint32_t i = section++;
            if (dol[36 + i] != 0) { /* sizes at 0x90; addresses at 0x48; offsets at 0x00 */
                *dst = (void *)dol[18 + i]; *size = dol[36 + i]; *offset = dol_offset + dol[i];
                return 1;
            }
        }
        volatile uint8_t *bss = (volatile uint8_t *)dol[54]; /* 0xD8: BSS address, 0xDC: size */
        for (uint32_t n = 0; n < dol[55]; ++n) bss[n] = 0;
        entry_point = dol[56]; /* 0xE0 */
        state = 3;
    }
    *size = 0;
    return 0;
}

static void *app_close(void) { report("close\n"); return (void *)entry_point; }

__attribute__((section(".text.start"))) void apploader_entry(void (**init)(report_t),
                                                             int (**main_fn)(void **, uint32_t *, uint32_t *),
                                                             void *(**close)(void)) {
    *init = app_init; *main_fn = app_main; *close = app_close;
}
