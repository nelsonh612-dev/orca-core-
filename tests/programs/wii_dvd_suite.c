/* Game-side disc access on a Wii, booted from an encrypted partition by the HLE
 * apploader path: IOS requests through the Hollywood IPC registers with the same
 * register sequence as libogc's ipc.c (0x38 at init; X1 to send; Y2 acknowledged;
 * Y1 reply taken, then X2), opening /dev/di and reading the partition (decrypted
 * and hash-checked by IOS) while the program runs. The test fills the partition
 * at 0x44000-0x48000 with the words 0xA5000000 + i. */
#include "common.h"

#define REG32(a) (*(volatile uint32_t *)(a))
#define IPC(o) REG32(0xCD000000u + (o))
#define PHYS(p) ((uint32_t)(p) & 0x3FFFFFFFu)

static volatile uint32_t req[16] __attribute__((aligned(32)));
static volatile uint32_t in[8] __attribute__((aligned(32)));
static volatile uint32_t out[32] __attribute__((aligned(32)));
static const char path[16] __attribute__((aligned(32))) = "/dev/di";

static uint32_t keep(void) { return IPC(4) & 0x30u; }

static int32_t ios_call(void) {
    IPC(0) = PHYS(req);
    IPC(4) = keep() | 0x01u;            /* X1: execute */
    while ((IPC(4) & 0x02u) == 0u) { }  /* Y2: acknowledged */
    IPC(4) = keep() | 0x02u;
    IPC(0x30) = 0x40000000u;            /* clear the IPC interrupt flag */
    while ((IPC(4) & 0x04u) == 0u) { }  /* Y1: reply */
    const uint32_t reply = IPC(8);
    IPC(4) = keep() | 0x04u;
    IPC(0x30) = 0x40000000u;
    IPC(4) = keep() | 0x08u;            /* X2: reply taken (libogc does this after every reply) */
    if (reply != PHYS(req)) return -999;
    return (int32_t)req[1];
}

static int32_t di_ioctl(int32_t fd, uint32_t cmd, uint32_t a1, uint32_t a2, uint32_t out_len) {
    for (int i = 0; i < 8; ++i) in[i] = 0;
    in[0] = cmd << 24;
    in[1] = a1;
    in[2] = a2;
    for (int i = 0; i < 16; ++i) req[i] = 0;
    req[0] = 6; /* ioctl */
    req[2] = (uint32_t)fd;
    req[3] = cmd;
    req[4] = PHYS(in);
    req[5] = 0x20;
    req[6] = PHYS(out);
    req[7] = out_len;
    return ios_call();
}

int program_main(void) {
    IPC(4) = 56u; /* IY1 | IY2 | X2, as libogc's IPC init */

    for (int i = 0; i < 16; ++i) req[i] = 0;
    req[0] = 1; /* open */
    req[3] = PHYS(path);
    req[4] = 0;
    const int32_t fd = ios_call();
    orca_emit((uint32_t)fd);
    if (fd < 0) return 1;

    /* disc ID through the drive */
    orca_emit((uint32_t)di_ioctl(fd, 0x70, 0, 0, 0x20));
    orca_emit(out[0]);
    orca_emit(out[6]); /* Wii magic at 0x18 */

    /* partition reads (offsets in words): inside one cluster, then across a cluster boundary (0x45C00) */
    orca_emit((uint32_t)di_ioctl(fd, 0x71, 0x20, 0x44000u >> 2, 0x20));
    orca_emit(out[0]);
    orca_emit(out[7]);
    orca_emit((uint32_t)di_ioctl(fd, 0x71, 0x80, 0x45BC0u >> 2, 0x80));
    for (int i = 0; i < 32; i += 15) orca_emit(out[i]);
    orca_emit(out[31]);

    /* past the partition's end: the drive reports an error, the program goes on */
    orca_emit((uint32_t)di_ioctl(fd, 0x71, 0x20, 0x3FFFFFF0u, 0x20));

    /* cover status: disc present */
    orca_emit((uint32_t)di_ioctl(fd, 0x88, 0, 0, 4));
    orca_emit(out[0]);

    for (int i = 0; i < 16; ++i) req[i] = 0;
    req[0] = 2; /* close */
    req[2] = (uint32_t)fd;
    orca_emit((uint32_t)ios_call());
    return 0;
}
