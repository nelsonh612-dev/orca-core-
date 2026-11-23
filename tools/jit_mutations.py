#!/usr/bin/env python3
"""Mutation test of the AArch64 JIT, run on an AArch64 machine (CI job jit-mutations).

Each mutation changes one detail of src/ppc/jit.cpp; the JIT tests and the program
tests must fail for it ("killed"). A mutation that builds and passes ("survived")
shows a gap in the tests. The build is not -Werror (an unused variable is not a kill).
Usage: tools/jit_mutations.py <build-dir>
"""
import os, subprocess, sys, pathlib

SRC = pathlib.Path("src/ppc/jit.cpp")
MUTATIONS = [
    # Equivalent (cannot change behaviour): when the second rounding changed the value, |t| != |s|
    # (same sign), so HS and HI agree.
    ("EQUIVALENT FP: single FR compares >= instead of >", "e.cset(false, T4, Cond::HI);        // ... away from zero", "e.cset(false, T4, Cond::HS);        // ... away from zero"),
    ("FP: no single-midpoint slow path", "to_slow.push_back(e.cbnz(false, T1));", "(void)0;"),
    ("FP: div remainder range not checked", "exp_range(S, 256, 2046, false); // |a| >= 2^-767", "exp_range(S, 0, 2047, false); // |a| >= 2^-767"),
    ("FP: FX not raised", "e.orr(false, T0, T0, T4, Shift::LSL, 31);", "(void)0;"),
    ("FP: VX not recomputed", "e.orr(false, T0, T0, T4, Shift::LSL, 29);\n    st(T0, o.fpscr);\n    if (rc)", "st(T0, o.fpscr);\n    if (rc)"),
    ("FP: RN not required", "e.mov_imm(false, T1, k == K::Cmp ? kFpEnables : (kFpEnables | fpu::RN_MASK));", "e.mov_imm(false, T1, kFpEnables);"),
    ("FP: madd error sign from r3 always", "e.csel(true, E, E, X15, Cond::NE); // the error has the sign", "e.mov(true, E, X15); // the error has the sign"),
    ("FP: ps slot 1 inexact ignored", "e.orr(false, T3, 0, T1);", "e.mov(false, T3, 0);"),
    ("FP: fsel uses GT", "e.fcsel(s == 0U ? 24 : 25, 2, 1, Cond::GE);", "e.fcsel(s == 0U ? 24 : 25, 2, 1, Cond::GT);"),
    ("TLB: page crossing not checked", "e.cmp_imm(false, 13, 0x1000U - size);", "e.cmp_imm(false, 13, 0xFFFU);"),
    ("TLB: locked-cache lines ignored", "        if ((line.first & ~0xFFFU) == pp) return;", "        (void)line;"),
    ("TLB: DBAT changes ignored", "    now.dbat = s.dbat;\n", ""),
    ("TLB: write entries kept on code pages", "        if (en.write_tag != 0U && en.phys_page == phys_page) en.write_tag = 0;", "        (void)en; (void)phys_page;"),
    ("TLB: write stamps not advanced", "        e.add_imm(true, 14, 14, 1);\n", ""),
    # Equivalent: a write entry is only created after a store succeeded through the full
    # write translation, and BAT changes flush the cache, so the access type cannot differ.
    ("EQUIVALENT TLB: write entries from a read translation", "write ? AccessType::Write : AccessType::Read, cpu_.config_.extended_bats, t)", "AccessType::Read, cpu_.config_.extended_bats, t)"),
    ("TLB: write entries on code pages", "if (write && (bus_.is_code_page(pp) || bus_.observes_cpu_writes())) return;", "if (write && bus_.observes_cpu_writes()) return;"),
    ("Links: budget never refuses", "        refuse = e.b_cond(Cond::HI);", "        refuse = e.b_cond(Cond::VS);"),
    ("Links: not unpatched on invalidation", "    for (const u32* site : b->incoming) {", "    for (const u32* site : std::vector<const u32*>{}) {"),
    ("Links: one context for all", "        if (contexts_[k] == now) { link_context_ = static_cast<u32>(k); return; }", "        { link_context_ = static_cast<u32>(k); return; }"),
    ("Links: running block by pointer only", "if (b == running_ || (current_pc_valid_ && current_pc_ - b->ea < b->count * 4U)) running_invalidated_ = true;", "if (b == running_) running_invalidated_ = true;"),
    ("Exits: steps not accumulated", "        if (steps != 0U) e.add_imm(false, T0, T0, steps);", "        e.movz(false, T0, static_cast<u16>(steps));"),
    ("Store helper: own-code check after update dropped", "        e.cmp_imm(false, 0, 2);\n        const auto same = e.b_cond(Cond::NE);\n        exit_const(pc + 4U, i + 1U, kExitNormal);\n        e.bind_here(same);\n    }\n    return true;", "    }\n    return true;"),
]

def run(cmd, cwd=None, timeout=None):
    try:
        return subprocess.run(cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        # a hang (e.g. a linked loop that never leaves translated code) also detects the mutation
        return subprocess.CompletedProcess(cmd, 124, "timeout")

def main():
    build = os.path.abspath(sys.argv[1])
    original = SRC.read_text()
    results = []
    for name, old, new in MUTATIONS:
        if original.count(old) != 1:
            results.append((name, f"NOT APPLIED ({original.count(old)} matches)"))
            continue
        SRC.write_text(original.replace(old, new))
        try:
            b = run(["ninja", "-C", build, "orca_test_jit", "orca_test_programs"])
            if b.returncode != 0:
                results.append((name, "does not build"))
                continue
            t1 = run([f"{build}/orca_test_jit"], cwd=build, timeout=300)
            t2 = run([f"{build}/orca_test_programs"], cwd=build, timeout=300) if t1.returncode == 0 else t1
            hang = t1.returncode == 124 or t2.returncode == 124
            results.append((name, ("killed (hang)" if hang else "killed") if (t1.returncode != 0 or t2.returncode != 0) else "SURVIVED"))
        finally:
            SRC.write_text(original)
        print(f"[mutation] {results[-1][1]:>14}  {name}", flush=True)
    run(["ninja", "-C", build])
    killed = sum(1 for _, r in results if r.startswith("killed"))
    print(f"[mutation] {killed} of {len(results)} killed")
    for name, r in results:
        if not r.startswith("killed"):
            print(f"[mutation] not killed: {name}: {r}")

if __name__ == "__main__":
    main()
