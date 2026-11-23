/* Shared by the target (PowerPC) and host builds of the test programs. */
#ifndef ORCA_TEST_COMMON_H
#define ORCA_TEST_COMMON_H
#include <stddef.h>
#include <stdint.h>
#ifdef ORCA_HOST
#include <string.h>
#else
void *memcpy(void *dst, const void *src, size_t n); /* provided by runtime.c */
#endif
#ifdef ORCA_HOST
void orca_emit(uint32_t v);
#else
static inline void orca_emit(uint32_t v) { *(volatile uint32_t *)0xCE000000u = v; }
#endif
static inline void emit_u64(uint64_t v) { orca_emit((uint32_t)(v >> 32)); orca_emit((uint32_t)v); }
static inline void emit_double(double d) { uint64_t b; memcpy(&b, &d, 8); emit_u64(b); }
static inline void emit_float(float f) { uint32_t b; memcpy(&b, &f, 4); orca_emit(b); }
#endif
