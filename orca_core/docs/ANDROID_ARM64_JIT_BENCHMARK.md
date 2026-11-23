# Android ARM64 JIT benchmark

The CI workflow publishes an ARM64 standalone executable named `orca_bench_interpreter` as the artifact `orca-core-arm64-benchmark-runner`.

## Run on a physical Android ARM64 device

Download and extract that artifact, then connect the device with USB debugging enabled:

```sh
adb push orca_bench_interpreter /data/local/tmp/orca_bench_interpreter
adb shell chmod 755 /data/local/tmp/orca_bench_interpreter
adb shell /data/local/tmp/orca_bench_interpreter 5000000 i compare
```

The command runs the same synthetic guest integer loop twice using independent CPU/Bus instances: first the reference interpreter, then the ARM64 JIT. It prints retired guest instructions, elapsed time, MIPS and speedup. The timed JIT run includes first-block compilation, so it measures a cold-start-inclusive run.

Repeat the command at least three times after the device has reached a stable temperature and compare the results. Avoid battery saver and background workloads. Keep the raw output with the device model, Android version, Orca commit SHA and iteration count.

## Important limits

- This is a synthetic integer/memory loop covering addi, lwz, stw, rlwinm and bdnz. It is not a Resident Evil 4 benchmark and does not predict game FPS.
- The optional `f` mode currently measures FPU arithmetic fallback, not native FPU arithmetic.
- QEMU CI smoke tests validate the benchmark path but are not performance measurements for physical hardware.
- Do not report an ARM64 speedup until the command has been run on the actual device and the output is captured.
