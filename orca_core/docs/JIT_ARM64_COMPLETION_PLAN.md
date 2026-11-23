# JIT ARM64 completion plan (M054+)

This document is an execution checklist, not a claim that the JIT is complete.

## Baseline

Start from `m034-jit-coverage-mainline`, whose milestone document records work through M053. Preserve the interpreter as the semantic oracle and fallback. Do not enable an instruction in the native dispatcher until its differential tests pass on AArch64.

## Gate 0 — stabilize M052/M053

- [ ] Obtain a functioning GitHub Actions runner; runs 44058601048–44058941750 had runner_id=0 and no steps, so they are infrastructure failures, not passing builds.
- [ ] Run GCC and Clang warning-as-error builds.
- [ ] Run the complete host CTest suite.
- [ ] Run AArch64 differential tests under QEMU.
- [ ] Verify guarded FP memory fallback for MMIO, page crossings, invalid mappings, locked cache and WIMG cases.
- [ ] Verify the corrected stfiwx FPR low-word offset with distinct high/low sentinel patterns.
- [ ] Build the Android ARM64 APK with ORCA_GLES=ON.

## Gate 1 — complete scalar floating-point memory

- [ ] Verify all M052 lfs/lfd/stfs/stfd D-form and indexed/update forms against the interpreter.
- [ ] Verify NaN payloads, signaling NaNs, signed zero, subnormals, alignment and endian conversion.
- [ ] Preserve precise interpreter fallback for all resolver declines.

## Gate 2 — scalar FPU arithmetic

Implement each instruction family only after documenting the interpreter's exact semantics:
- [ ] fadd/fadds, fsub/fsubs, fmul/fmuls, fdiv/fdivs.
- [ ] fmadd/fmadds, fmsub/fmsubs, fnmadd/fnmadds, fnmsub/fnmsubs.
- [ ] frsp, fctiw/fctiwz and conversion/rounding variants.
- [ ] fcmpo/fcmpu and FPSCR/FPRF/CR1 updates.
- [ ] FPSCR control/status instructions, exception enables, sticky flags and rounding modes.
- [ ] NaN, infinities, denormals, signed zero, overflow/underflow and precise exception behavior.

Do not use host fast-math. Match the reference engine's rounding and exception policy. If host FP cannot reproduce a case exactly, keep that instruction in interpreter fallback.

## Gate 3 — paired-single and quantized operations

- [ ] Arithmetic and fused operations on both lanes.
- [ ] Paired comparisons, select, sum and merge variants.
- [ ] Quantized psq_l/psq_st for every GQR type, scale, signedness and W bit.
- [ ] Exact FPSCR/CR1 behavior and HID2[PSE]/MSR[FP] checks.
- [ ] Differential tests for both lanes, NaNs, infinities, subnormals and quantization boundaries.

## Gate 4 — remaining integer, memory and system instructions

- [ ] Audit the interpreter decode tables against native translator coverage.
- [ ] Implement missing multiply/divide and OE forms only with exact CA/OV/SO behavior.
- [ ] Complete CR/XER transfer and logical forms.
- [ ] lwarx/stwcx. reservation semantics, including invalidation by all relevant writes.
- [ ] lswx/lswi/stswx/stswi string operations with precise partial-fault behavior.
- [ ] Audit cache, TLB, MMU, BAT, exception, privilege and self-modifying-code interactions.
- [ ] Keep MMIO, page-crossing, locked-cache and special WIMG accesses on safe interpreter paths unless a separately validated helper exists.

## Gate 5 — JIT correctness, scheduler and performance

- [ ] Differential randomized instruction tests comparing the full CpuState and memory image.
- [ ] Stress self-modifying code, DMA writes, icbi, aliases and BAT/PTE remapping.
- [ ] Verify scheduler event boundaries, decrementer, external interrupts and exception delivery.
- [ ] Test run_until/stop_pc and exact retired-instruction accounting.
- [ ] Measure cold compile, warm dispatch, integer, memory, scalar FP and paired-single workloads on physical Android ARM64.
- [ ] Compare against the interpreter on the same device; publish reproducible results and no QEMU-derived speedup claims.

## Completion criteria

The JIT is complete only when every instruction accepted by the native translator has differential coverage, all unsupported instructions take a correct fallback, the full test matrix passes, Android ARM64 builds and runs on physical hardware, and scheduler/interrupt/memory correctness is demonstrated. Report remaining unsupported ISA forms explicitly. An APK build alone is not completion.
