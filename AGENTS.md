# AGENTS.md

Working agreement for this repository — for human contributors and for coding
agents alike.

## What this is

Vane is a runtime ISA dispatch layer for Arm. It detects a CPU's vector
features at startup, binds each operation to the best available kernel, and
measures the result honestly. See [README.md](README.md).

Roughly 1,400 lines across `src/`, `include/` and `tools/`. Keep it that way —
this is infrastructure, not a framework.

## Build and verify

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
./build/vane_verify        # must exit 0
ctest --test-dir build
```

Android and Quest:

```powershell
.\scripts\run_on_android.ps1 -Label <device>
```

Linux, macOS, Graviton:

```bash
./scripts/build_and_run.sh <label> --seconds 30
```

## The four rules

These exist because this repository previously violated every one of them.

### 1. Editing is not completing

A change is done when a compiler has accepted it and a test has exercised it.
Cross-compiling proves syntax; only a device proves behaviour. If you cannot
run it, say so in exactly those terms:

> `STATUS: UNVERIFIED ON HOST — REQUIRES TARGET ARM HARDWARE`

### 2. Never emit a number you did not measure

No hardcoded latency, throughput, speedup, temperature or utilisation. Not in
code, not in a report, not in the README, not as a plausible-looking example in
documentation. Telemetry uses `VN_UNMEASURED`, which serialises to JSON `null`.

Do not name a variable `measured_*` unless it holds a measurement.

### 3. A kernel with no callers is not a feature

Before claiming an optimisation exists, trace it to a public entry point. Every
kernel in `src/kernels/` must be reachable from `vane.h` through the
dispatcher. Two kernels in this project's history were written, documented,
benchmarked in prose and never once executed.

### 4. Tests call the shipped code

`tools/vane_verify.cpp` must exercise the real exported functions. A test
that re-implements the algorithm inline validates the test. That is precisely
how the SVE interleave defect survived.

## Adding a kernel path

1. Implement in `src/kernels/kernel_<isa>.cpp`, guarded by the relevant
   `__ARM_FEATURE_*` macro.
2. Declare it in `src/vane_internal.h`.
3. Give the translation unit its own `-march` in `CMakeLists.txt`. Runtime
   dispatch keeps unsupported paths unreachable, so a per-file baseline never
   raises the library's minimum CPU requirement.
4. Add a feature bit and a `bind_path` case in `src/vane_dispatch.cpp`.
5. Add it to the `paths[]` array in `tools/vane_verify.cpp`. **A path that
   the verifier does not check does not ship.**

## Conventions

- C++17, C ABI at the boundary. Public symbols are `vn_*`; internal kernels are
  `vn_kern_*_<isa>`.
- The scalar path in `kernel_scalar.cpp` is the correctness **oracle**, not a
  fallback. It stays simple and portable so it can be trusted. Do not optimise
  it.
- Never add `-ffast-math`. It permits reassociation, which would invalidate the
  scalar-versus-vector equivalence the verifier asserts.
- Accumulate in `float`, grouped per block, so vector paths differ from the
  oracle only by reduction order.
- One INT4 encoding: `+8` zero point, 32 elements per block, one fp32 scale.
  Compatible with llama.cpp `Q4_0`. Do not introduce a second.

## results/

Written by scripts, committed unedited. See [results/README.md](results/README.md)
for how captures are produced, and [results/ANALYSIS.md](results/ANALYSIS.md)
for what they mean. Do not hand-write, adjust, or re-run a capture until it
looks better.

Prose in `ANALYSIS.md` and the READMEs is subject to rule 2: every number in
them must be copied from a file in `results/`.

## Not in this repository

- `UnityIntegration/` — 1.6 GB local demo project. Its art is third-party
  Asset Store content that cannot be redistributed.
- Planning and competition notes — about the process, not the software.
