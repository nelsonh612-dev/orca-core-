/* Integer workload compiled twice: natively for the host and with clang for the
 * PowerPC 750. Both builds must emit the same sequence of values.
 * Only well-defined C is used (unsigned wrap-around, no signed overflow). */
#include "common.h"

#ifndef PROGRAM
#define PROGRAM program_main
#endif

static uint32_t lcg_state;
static uint32_t lcg(void) { lcg_state = lcg_state * 1664525u + 1013904223u; return lcg_state; }

static uint32_t crc_table[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}
static uint32_t crc32(const uint8_t *p, uint32_t n) {
    uint32_t c = 0xFFFFFFFFu;
    while (n--) c = crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void quicksort(int32_t *a, int lo, int hi) {
    while (lo < hi) {
        const int32_t pivot = a[(lo + hi) / 2];
        int i = lo, j = hi;
        while (i <= j) {
            while (a[i] < pivot) ++i;
            while (a[j] > pivot) --j;
            if (i <= j) { const int32_t t = a[i]; a[i] = a[j]; a[j] = t; ++i; --j; }
        }
        if (j - lo < hi - i) { quicksort(a, lo, j); lo = i; } else { quicksort(a, i, hi); hi = j; }
    }
}

static uint32_t fib_rec(uint32_t n) { return n < 2 ? n : fib_rec(n - 1) + fib_rec(n - 2); }

static uint32_t op_add(uint32_t a, uint32_t b) { return a + b; }
static uint32_t op_xor(uint32_t a, uint32_t b) { return a ^ b; }
static uint32_t op_mul(uint32_t a, uint32_t b) { return a * b; }
static uint32_t op_rot(uint32_t a, uint32_t b) { b &= 31u; return b ? (a << b) | (a >> (32u - b)) : a; }
static uint32_t (*const ops[4])(uint32_t, uint32_t) = {op_add, op_xor, op_mul, op_rot};

static uint32_t classify(uint32_t x) {
    switch (x % 12u) { /* dense switch: jump table (bctr) on PowerPC */
    case 0: return 11; case 1: return 23; case 2: return 37; case 3: return 41;
    case 4: return 53; case 5: return 67; case 6: return 71; case 7: return 83;
    case 8: return 97; case 9: return 101; case 10: return 113; default: return 127;
    }
}

struct Record { int8_t s8; uint8_t u8; int16_t s16; uint16_t u16; int32_t s32; uint32_t u32; };

int PROGRAM(void) {
    /* 1. CRC32 over a pseudo-random buffer */
    static uint8_t buf[4096];
    lcg_state = 12345u;
    for (uint32_t i = 0; i < sizeof buf; ++i) buf[i] = (uint8_t)(lcg() >> 24);
    crc_init();
    orca_emit(crc32(buf, sizeof buf));
    orca_emit(crc32((const uint8_t *)"123456789", 9)); /* standard check value 0xCBF43926 */

    /* 2. sort 2000 signed values */
    static int32_t arr[2000];
    for (int i = 0; i < 2000; ++i) arr[i] = (int32_t)(lcg() % 200001u) - 100000;
    quicksort(arr, 0, 1999);
    uint32_t sorted = 1, sum = 0;
    for (int i = 1; i < 2000; ++i) if (arr[i - 1] > arr[i]) sorted = 0;
    for (int i = 0; i < 2000; ++i) sum = sum * 31u + (uint32_t)arr[i];
    orca_emit(sorted); orca_emit(sum); orca_emit((uint32_t)arr[0]); orca_emit((uint32_t)arr[1999]);

    /* 3. sieve of Eratosthenes */
    static uint8_t composite[20001];
    uint32_t primes = 0, last = 0;
    for (uint32_t i = 2; i <= 20000; ++i) {
        if (composite[i]) continue;
        ++primes; last = i;
        for (uint32_t j = i * i; j <= 20000; j += i) composite[j] = 1;
    }
    orca_emit(primes); orca_emit(last);

    /* 4. 64-bit arithmetic (addc/adde, mulhwu, 64-bit shifts and compares) */
    uint64_t acc = 0x0123456789ABCDEFull, h = 1469598103934665603ull;
    for (uint32_t i = 0; i < 1000; ++i) {
        acc = acc * 6364136223846793005ull + 1442695040888963407ull;
        h ^= acc >> 17; h *= 1099511628211ull;
        if (acc > h) h += acc << 3; else h -= acc >> 5;
    }
    emit_u64(acc); emit_u64(h);

    /* 5. signed and unsigned division / modulo */
    uint32_t dsum = 0;
    const int32_t dividends[] = {7, -7, 100000, -100000, 2147483647, -2147483647, 0, 1, -1, 123456789};
    const int32_t divisors[] = {1, -1, 2, -2, 3, -3, 7, -7, 1000, -1000, 65536, 2147483647};
    for (unsigned i = 0; i < 10; ++i)
        for (unsigned j = 0; j < 12; ++j) {
            dsum = dsum * 33u + (uint32_t)(dividends[i] / divisors[j]);
            dsum = dsum * 33u + (uint32_t)(dividends[i] % divisors[j]);
            dsum = dsum * 33u + (uint32_t)dividends[i] / (uint32_t)divisors[j];
            dsum = dsum * 33u + (uint32_t)dividends[i] % (uint32_t)divisors[j];
        }
    orca_emit(dsum);

    /* 6. recursion, function pointers, jump tables */
    orca_emit(fib_rec(24));
    uint32_t fp = 0x9E3779B9u, sw = 0;
    for (uint32_t i = 0; i < 5000; ++i) { fp = ops[i & 3u](fp, i * 2654435761u); sw += classify(fp ^ i); }
    orca_emit(fp); orca_emit(sw);

    /* 7. sign/zero extension through memory and struct copies */
    struct Record recs[64], copy[64];
    for (int i = 0; i < 64; ++i) {
        const uint32_t r = lcg();
        recs[i].s8 = (int8_t)(r >> 3); recs[i].u8 = (uint8_t)(r >> 5);
        recs[i].s16 = (int16_t)(r >> 7); recs[i].u16 = (uint16_t)(r >> 9);
        recs[i].s32 = (int32_t)r; recs[i].u32 = r ^ 0xA5A5A5A5u;
    }
    memcpy(copy, recs, sizeof copy);
    int32_t ext = 0;
    for (int i = 0; i < 64; ++i)
        ext = (int32_t)((uint32_t)ext * 7u + (uint32_t)(copy[i].s8 + copy[i].u8 + copy[i].s16 + copy[i].u16) + (copy[i].u32 >> 7));
    orca_emit((uint32_t)ext);

    /* 8. bit tricks: popcount, leading zeros, byte reverse, rotate */
    uint32_t bits = 0;
    for (uint32_t i = 1; i < 3000; ++i) {
        uint32_t v = i * 0x01010101u ^ (i << 13), pc = 0;
        while (v) { v &= v - 1u; ++pc; }
        const uint32_t x = i * 0x9E3779B9u;
        const uint32_t rev = (x >> 24) | ((x >> 8) & 0xFF00u) | ((x << 8) & 0xFF0000u) | (x << 24);
        bits = bits * 3u + pc + (uint32_t)__builtin_clz(x | 1u) + op_rot(rev, i);
    }
    orca_emit(bits);

    /* 9. byte strings with explicit signedness */
    unsigned char text[] = "Orca Core: PowerPC 750 integer suite";
    uint32_t len = 0;
    while (text[len]) ++len;
    for (uint32_t i = 0; i < len / 2; ++i) { const unsigned char t = text[i]; text[i] = text[len - 1 - i]; text[len - 1 - i] = t; }
    orca_emit(len); orca_emit(crc32(text, len));
    return 0;
}
