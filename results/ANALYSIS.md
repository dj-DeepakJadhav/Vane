# Analysis

What the captures in this directory actually say, device by device. Every
number below is copied out of a file here; see [README.md](README.md) for how
the captures are produced and how to read the fields.

The short version lives in the [root README](../README.md#measured-results).
This is the long version, including the results that did not go the way we
expected.

## Contents

- [AWS Graviton4 — Neoverse V2](#aws-graviton4--neoverse-v2)
- [SVE is not faster than NEON here](#sve-is-not-faster-than-neon-here-and-that-is-not-a-disappointment)
- [Galaxy S23 Ultra](#galaxy-s23-ultra)
- [Sustained: 180 s per path](#sustained-180-s-per-path)
- [Shape sweep](#shape-sweep)
- [Redmi K20 Pro — Snapdragon 855, 2019](#redmi-k20-pro--snapdragon-855-2019)
- [Meta Quest 2 — XR2](#meta-quest-2--xr2)
- [Quest 2 sustained — the device that should have throttled](#quest-2-sustained--the-device-that-should-have-throttled)
- [What `temp_C` actually measures](#what-temp_c-actually-measures)
- [The S23 Ultra has no SVE](#the-s23-ultra-has-no-sve)
- [The SVE path had never run](#the-sve-path-had-never-run)

---

## AWS Graviton4 — Neoverse V2

[`graviton4.txt`](graviton4.txt) · `c8g.large`, `eu-central-1`, kernel
`7.0.0-1006-aws`, GCC 15.2.0 · 512 × 4096, 20 s per path

| Operation | scalar | neon | sve | best vs scalar |
|---|---:|---:|---:|---:|
| `dequant_gemv_int4` | 784,542 ns | **368,077 ns** | 376,765 ns | **2.13×** |
| `gemv_int8` (SDOT) | 363,820 ns | 52,230 ns | **51,862 ns** | **7.02×** |
| `quantize_int4` | 7,125,481 ns | 1,700,340 ns | **1,685,292 ns** | **4.23×** |

The SVE path executed here for the first time. It had been written on an x86
host, compile-verified and codegen-verified, and never once run. It agrees with
the scalar oracle at all four test shapes on first execution.

Thermals are `null` on this platform — a server exposes no readable sensor to an
unprivileged process — and sustained ratios are 1.00 across every path, which is
what a properly cooled machine should look like.

## SVE is not faster than NEON here, and that is not a disappointment

Look at the `dequant_gemv_int4` row again: **SVE is 2% slower than NEON**. On the
other two kernels they are within 1% of each other. The reason is in the probe
output — `sve vector len: 128 bits`. Neoverse V2 implements SVE at the *same
width as NEON*, so the SVE path buys predication and length-agnosticism but no
extra lanes, while paying for predicate setup. There is nothing left to win.

This matters because the dispatcher currently prefers SVE whenever it is
present, and on this CPU that preference is very slightly wrong. A feature bit
tells you an instruction set *exists*; it does not tell you it is *faster*. SVE's
width advantage would appear on 256-bit hardware such as Graviton3's Neoverse
V1, which we have not tested.

We are reporting this rather than quietly dispatching to NEON and describing the
SVE work as a win. It is also the concrete argument for `vn_autotune()`: measure
the paths on first run and cache the winner, instead of trusting a tier
ordering.

---

## Galaxy S23 Ultra

[`s23-ultra.txt`](s23-ultra.txt) · SM8550, Android 16, NDK 27 · 512 × 4096,
5 s per path

Medians from a live monotonic clock on the device. `vs scalar` is a ratio of
measured medians in the same run, not a modelled figure.

| Operation | scalar | neon | vs scalar | scalar GB/s | neon GB/s |
|---|---:|---:|---:|---:|---:|
| `dequant_gemv_int4` | 682,813 ns | 316,094 ns | **2.16×** | 1.91 | 4.16 |
| `gemv_int8` (SDOT) | 157,917 ns | 53,177 ns | **2.97×** | 13.23 | 38.96 |
| `quantize_int4` | 4,379,167 ns | 964,271 ns | **4.54×** | 2.14 | 9.55 |

Equivalence against the scalar oracle on the same device: `quantize_int4` and
`gemv_int8` are **exact** (zero byte mismatches, zero integer mismatches);
`dequant_gemv_int4` differs by at most `1.33e-05` relative at 128 × 4096, which
is float reduction-order noise well inside the `1e-4` tolerance.

The hottest readable thermal zone across the run ranged **65.8 °C to 75.3 °C**
— see [what that field actually measures](#what-temp_c-actually-measures)
before reading it as core temperature; sustained ratios ranged 0.96–1.04.

Run-to-run variance is real and worth stating: an earlier capture of the same
build measured `gemv_int8` at 2.36× where this one measures 2.97×, because the
device started the run cooler. Single-number speedup claims from mobile silicon
should be read with that in mind, which is why every capture here records its
own temperatures.

### Sustained: 180 s per path

[`s23-ultra-sustained.txt`](s23-ultra-sustained.txt) — `dequant_gemv_int4`,
512 × 4096, 180 seconds per path.

| path | median | sustained | temp | iterations |
|---|---:|---:|---:|---:|
| scalar | 743,854 ns | 0.98 | 70.1 °C | 240,088 |
| neon | 330,156 ns | 0.95 | 72.9 °C | 531,014 |

**This device does not meaningfully throttle on this workload.** We expected it
to and it did not — sustained stays within 5% and temperature is flat. The
speedup is **2.25× at 180 s against 2.16× at 5 s**, over a 531,014-iteration
sample.

We are reporting the result we got rather than the one the pitch wanted. The
value of measuring sustained throughput is not that it always collapses; it is
that you stop guessing whether it does.

### Shape sweep

[`s23-ultra-shapes.txt`](s23-ultra-shapes.txt) — 4 s per path.

| rows × cols | scalar | neon | speedup | scalar GB/s | neon GB/s |
|---|---:|---:|---:|---:|---:|
| 512 × 512 | 85,208 ns | 40,104 ns | 2.12× | 1.90 | 4.09 |
| 512 × 1024 | 183,593 ns | 79,427 ns | 2.31× | 1.82 | 4.11 |
| 512 × 4096 | 735,208 ns | 316,354 ns | 2.32× | 1.82 | 4.12 |
| 256 × 16384 | 1,469,063 ns | 658,020 ns | 2.23× | 1.82 | 4.09 |
| 2048 × 1024 | 734,479 ns | 329,323 ns | 2.23× | 1.79 | 4.04 |

Both paths pin to a flat bandwidth ceiling — scalar ≈1.8 GB/s, NEON ≈4.1 GB/s —
independent of shape. That is the signature of a **memory-bandwidth-bound**
kernel, which is what INT4 dequant-GEMV should be: 0.625 bytes per element and
a single FMA per element is very low arithmetic intensity. It also explains the
absence of thermal throttling above, and it sets the honest expectation that
this kernel's speedup comes from moving less data, not from more FLOPs.

The 2.12× at 512 × 512 is the only outlier, where per-call overhead is a larger
share of a shorter call.

---

## Redmi K20 Pro — Snapdragon 855, 2019

[`redmi-k20-pro.txt`](redmi-k20-pro.txt) · Android 11, Cortex-A76 · 512 × 4096,
5 s per path

| Operation | scalar | neon | vs scalar | scalar GB/s | neon GB/s |
|---|---:|---:|---:|---:|---:|
| `dequant_gemv_int4` | 1,396,771 ns | 587,500 ns | **2.38×** | 0.94 | 2.25 |
| `gemv_int8` (SDOT) | 229,792 ns | 115,573 ns | **1.99×** | 8.96 | 18.22 |
| `quantize_int4` | 6,001,562 ns | 1,846,667 ns | **3.25×** | 1.57 | 5.09 |

Four years older than the S23 Ultra and roughly half its memory bandwidth on
every path — but the *ratios* land in the same 2–3.3× band, which is what a
bandwidth-bound kernel should do on a slower memory system. Verify passed at all
shapes, exit 0.

Two details worth pulling out of this capture:

**The feature line is shorter, and that is the product working.** This part
reports `neon fp16 dotprod` — no `i8mm`, no `bf16`, which are ARMv8.6 features
the Cortex-A76 does not implement. The *same* `vane.so` that lit up `i8mm bf16`
on the S23 and `sve2` on Graviton detected three fewer features here and bound
itself accordingly, with no recompilation and no per-device build.

**`cpu` is populated here and blank on the S23.** This device reports
`Qualcomm Technologies, Inc SM8150`; the S23 Ultra reports `(not exposed)`.
Same binary, same `read_cpu_desc`. Android 11 still carries a `Hardware` line in
`/proc/cpuinfo` and Android 16 does not. That is the difference between a field
the platform will give you and one it will not — which is exactly why the blank
is a blank instead of a plausible-looking string.

---

## Meta Quest 2 — XR2

[`quest2.txt`](quest2.txt) · SM8250, Android 14, standalone headset ·
512 × 4096, 5 s per path

| Operation | scalar | neon | vs scalar | scalar GB/s | neon GB/s |
|---|---:|---:|---:|---:|---:|
| `dequant_gemv_int4` | 1,566,927 ns | 681,928 ns | **2.30×** | 0.87 | 1.97 |
| `gemv_int8` (SDOT) | 245,521 ns | 104,531 ns | **2.35×** | 8.61 | 19.80 |
| `quantize_int4` | 6,931,042 ns | 1,980,833 ns | **3.50×** | 1.37 | 4.87 |

The slowest of the four CPUs on every path — 0.87 GB/s scalar against the S23's
1.91 — which is what a passively-cooled headset holding a 72 Hz deadline should
look like.

**Identical feature set to the K20 Pro, different performance.** The XR2 reports
itself as `KONA` (the Snapdragon 865 family) and detects exactly
`neon fp16 dotprod` — character for character the same string as the 2019 Redmi.
Same detection, same dispatch decision, and `dequant_gemv_int4` still runs 16%
slower here than on the K20. Feature parity is not performance parity, which is
the sharpest argument in this repository for `vn_autotune()`: a feature bit tells
you an instruction *exists*, never what it will cost you.

**This device exposes no thermal sensor at all.** The bench header says so
outright — `thermal : no readable sensor on this platform (reported as null)` —
and every `temp_C` cell is `-`, serialising to JSON `null`. Nothing was
estimated, defaulted, or filled in from a nearby device. That is the telemetry
policy doing its job on the one device where a plausible-looking invented number
would have been easiest to get away with.

### Quest 2 sustained — the device that should have throttled

[`quest2-sustained.txt`](quest2-sustained.txt) — `dequant_gemv_int4`,
512 × 4096, 180 seconds per path.

| path | median | sustained | iterations |
|---|---:|---:|---:|
| scalar | 1,566,458 ns | 0.98 | 115,927 |
| neon | 681,875 ns | 1.01 | 264,455 |

**It doesn't throttle either.** 2.30× at 180 s against 2.30× at 5 s, over
380,382 timed iterations and six minutes of continuous load, on a passively
cooled headset with no fan and a 72 Hz frame deadline. If anything we own was
going to fall over, it was this.

The medians from the two runs agree to within 0.03% — 1,566,458 ns against
1,566,927 ns on scalar, 681,875 against 681,928 on NEON. That is a much tighter
reproduction than the S23 Ultra managed between its own runs, where the same
build measured `gemv_int8` at 2.36× and then 2.97×. A headset running a fixed
workload turns out to be a *better* benchmark host than a phone, because there
is no foreground app, no user, and far less aggressive DVFS competing with you.

So the honest fleet-wide result is that **nothing we tested throttles on this
kernel** — S23 0.95–0.98, K20 Pro 1.00–1.02, Quest 2 0.98–1.01. That is not the
result the pitch wanted, and it has a clean explanation already visible in the
[shape sweep](#shape-sweep): this kernel is memory-bandwidth-bound at 0.625
bytes per element and one FMA. Low arithmetic intensity means low power, and low
power means no thermal wall. A compute-dense kernel on the same silicon would
tell a completely different story.

The point of measuring sustained throughput was never that it always collapses.
It is that four devices now say it doesn't, and we know that instead of assuming
either way.

---

## What `temp_C` actually measures

It is **the hottest thermal zone the process can read**, not CPU package
temperature. That distinction is usually academic and on this device it is not.

The K20 Pro capture reports ~100.5 °C. Its zones say:

| zone | reading |
|---|---|
| `soc` | 274 °C — rejected by the 0–150 °C plausibility filter |
| `pm8150b-wp-therm` | **101.0 °C** — a PMIC thermistor, and what got reported |
| `cpu-0-*-step`, `cpuss-*` | **~36.3 °C** — the actual cores |

The reported figure is off the CPU by 64 degrees, because the hottest readable
zone on this phone is a power-management sensor. The number is real and
[`vn_read_thermal_c`](../src/vane_telemetry.cpp) did precisely what it documents
— but "hottest zone" and "how hot the CPU got" are different claims, and only
the first one is supported.

So: read `temp_C` as a coarse thermal-environment signal, not as core
temperature, and do not infer throttling headroom from it. The **`sustained`**
column is the trustworthy throttling indicator, because it is derived from
measured throughput rather than from a sensor whose identity varies per device.
On the K20 Pro `sustained` is 1.00–1.02 across all six runs, which is the real
finding: no throughput decay, whatever that thermistor says.

Across the fleet this one field lands in three different states, and all three
are honest:

| Device | `temp_C` | What the platform gave us |
|---|---|---|
| Galaxy S23 Ultra | 65.8–75.3 °C | a readable zone that tracks the workload |
| Redmi K20 Pro | ~100.5 °C | a readable zone that is a PMIC thermistor, not the CPU |
| Meta Quest 2 | `null` | no readable sensor of any kind |
| AWS Graviton4 | `null` | no sensor exposed to an unprivileged process |

Two nulls, one useful reading, one misleading-if-unlabelled reading. A library
willing to invent a number would have printed four plausible temperatures and
you would never have known which one to trust.

Fixing this properly means preferring zones whose `type` names a CPU and
falling back to hottest — a small change, deliberately not made mid-submission
because it would invalidate every capture here that was taken with the current
definition.

---

## The S23 Ultra has no SVE

`vane_probe` reports `neon fp16 dotprod i8mm bf16` — no `sve`, no `sve2` —
on a Snapdragon 8 Gen 2 whose Cortex-X3 and A715 cores are ARMv9. Qualcomm did
not expose SVE on this part.

This is the tool working correctly, and it is the reason the Graviton4 run had
to happen: **no phone in this results directory can execute a single SVE
instruction.** The mobile silicon we have reports ARMv9 and still exposes no
SVE, so the path stayed unrun until server-class Arm hardware was brought in to
run it. Feature detection is not pedantry when the flagship part of a
generation ships without the extension its architecture version implies.

---

## The SVE path had never run

Until the Graviton4 capture, the SVE kernel was written on an x86 Windows host,
compile-verified and confirmed by disassembly, and had never executed a single
instruction — the Galaxy S23 Ultra exposes no SVE, so there was nothing to run
it on.

A `c8g.large` closed the gap. It passed all four shapes on first execution, and
in doing so exposed a real defect in the dispatcher, which had gated its
vector-length query on `__ARM_FEATURE_SVE`. That macro reflects the flags of the
translation unit it appears in, and the dispatcher is compiled at the library
baseline without `+sve`, so the guard was permanently false and the vector
length was never read on any machine. The probe reported
`sve vector len: n/a` while simultaneously reporting `sve2` and dispatching to
SVE.

Compiling cleanly had hidden it. Running it did not. That is the whole argument
for this directory existing.
