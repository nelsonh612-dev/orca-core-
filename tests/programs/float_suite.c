/* Floating-point workload compiled natively and for the PowerPC 750.
 * Built with -ffp-contract=off on both sides so no multiply-adds are fused
 * implicitly; explicit fusion is not used. IEEE double/single results must
 * be bit-identical. */
#include "common.h"

#ifndef PROGRAM
#define PROGRAM program_main
#endif

static double newton_sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double r = x > 1.0 ? x : 1.0;
    for (int i = 0; i < 60; ++i) r = 0.5 * (r + x / r);
    return r;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

int PROGRAM(void) {
    /* 1. series in double: harmonic, alternating, Leibniz pi */
    double h = 0.0, alt = 0.0, pi4 = 0.0;
    for (int i = 1; i <= 20000; ++i) {
        h += 1.0 / (double)i;
        alt += ((i & 1) ? 1.0 : -1.0) / ((double)i * (double)i);
        pi4 += ((i & 1) ? 1.0 : -1.0) / (double)(2 * i - 1);
    }
    emit_double(h); emit_double(alt); emit_double(pi4 * 4.0);

    /* 2. single precision accumulation (fadds/fmuls rounding) */
    float fs = 0.0f, fp = 1.0f;
    for (int i = 1; i <= 5000; ++i) {
        fs += 1.0f / (float)i;
        fp *= 1.0001f;
        if (fp > 1000.0f) fp *= 0.001f;
    }
    emit_float(fs); emit_float(fp);

    /* 3. Newton square roots and divisions */
    double roots = 0.0;
    for (int i = 0; i < 200; ++i) roots += newton_sqrt((double)i * 3.75 + 0.125);
    emit_double(roots);
    emit_double(newton_sqrt(2.0));

    /* 4. 4x4 matrix product in single precision */
    float a[16], b[16], c[16];
    for (int i = 0; i < 16; ++i) { a[i] = (float)(i + 1) * 0.37f - 2.0f; b[i] = 1.5f / (float)(i + 2); }
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) {
            float s = 0.0f;
            for (int j = 0; j < 4; ++j) s += a[r * 4 + j] * b[j * 4 + k];
            c[r * 4 + k] = s;
        }
    for (int i = 0; i < 16; ++i) emit_float(c[i]);

    /* 5. conversions int <-> float/double (fctiwz, magic-number int->double) */
    uint32_t conv = 0;
    const double samples[] = {0.0, -0.0, 0.5, -0.5, 1.5, -1.5, 2.999, -2.999, 123456.789, -98765.4321,
                              2147483520.0, -2147483520.0, 1e-300, 65535.9, -65536.1};
    for (unsigned i = 0; i < sizeof samples / sizeof samples[0]; ++i) {
        conv = conv * 31u + (uint32_t)(int32_t)samples[i];
        conv = conv * 31u + (uint32_t)(int16_t)(int32_t)samples[i];
    }
    orca_emit(conv);
    double back = 0.0;
    const int32_t ints[] = {0, 1, -1, 2147483647, -2147483647 - 1, 16777217, -16777217, 123456789};
    const uint32_t uints[] = {0u, 1u, 0x80000000u, 0xFFFFFFFFu, 16777217u, 4000000000u};
    for (unsigned i = 0; i < 8; ++i) { back += (double)ints[i]; emit_float((float)ints[i]); }
    for (unsigned i = 0; i < 6; ++i) { back += (double)uints[i]; emit_float((float)uints[i]); }
    emit_double(back);

    /* 6. comparisons, branches, min/max, abs, negative zero */
    uint32_t cmp = 0;
    const double vals[] = {-3.5, -0.0, 0.0, 1e-310, 2.25, 1e300, -1e300, 7.0};
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned j = 0; j < 8; ++j) {
            cmp = cmp * 5u + (vals[i] < vals[j]) + 2u * (vals[i] == vals[j]);
            const double m = vals[i] > vals[j] ? vals[i] : vals[j];
            const double ab = __builtin_fabs(vals[i] - vals[j]);
            uint64_t mb, abb; memcpy(&mb, &m, 8); memcpy(&abb, &ab, 8);
            cmp ^= (uint32_t)(mb >> 32) + (uint32_t)abb;
        }
    orca_emit(cmp);

    /* 7. double -> float rounding (frsp), denormals, overflow to infinity */
    float narrow = 0.0f;
    const double wide[] = {1.0 / 3.0, 1e-40, 3.4028235677973366e38, 1e39, -1e-46, 0.1, 16777217.0};
    for (unsigned i = 0; i < 7; ++i) { narrow = (float)wide[i]; emit_float(narrow); }
    emit_double(1e308 * 10.0);
    emit_double(4.9406564584124654e-324 / 2.0);

    /* 8. interpolation chain mixing float and double */
    float t = 0.0f;
    double mix = 0.0;
    for (int i = 0; i < 1000; ++i) { t = lerp(t, (float)i, 0.125f); mix += (double)t * 0.001; }
    emit_float(t); emit_double(mix);
    return 0;
}
