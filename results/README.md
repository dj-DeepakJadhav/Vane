# results/

Device captures. **Every file here is written by a script and committed
unedited.**

For what these numbers mean device by device, see
[**ANALYSIS.md**](ANALYSIS.md). This file covers where they come from and how
to read the fields.

## The rule

A number reaches the README only by being copied out of a file in this
directory. Nothing here is hand-written, adjusted, rounded for presentation, or
re-run until it looked better. If a measurement is disappointing, it goes in
disappointing — [`s23-ultra-sustained.txt`](s23-ultra-sustained.txt) records a
thermal-throttling prediction that the hardware refused to confirm.

## How captures are produced

```powershell
.\scripts\run_on_android.ps1 -Label s23-ultra          # Android, Quest
```

```bash
./scripts/build_and_run.sh graviton4 --seconds 30      # Linux, macOS
```

Both scripts build from source, run `vane_probe`, `vane_verify` and
`vane_bench` in that order, and record the device model, SoC, OS, compiler
and exact command line in the file header alongside the numbers.

Both refuse to present benchmark output as trustworthy when `vane_verify`
exits non-zero. A build whose vector paths disagree with their own scalar
oracle has nothing worth measuring.

## Files

### Library captures — `vane_probe`, `vane_verify`, `vane_bench`

| File | Device | Contents |
|---|---|---|
| `graviton4.txt` | AWS `c8g.large` (Neoverse V2) | probe, verify, all three kernels — the only capture with an SVE path |
| `s23-ultra.txt` | Galaxy S23 Ultra (SM8550) | probe, verify, all three kernels at 512 × 4096 |
| `s23-ultra-sustained.txt` | Galaxy S23 Ultra | `dequant_gemv_int4`, 180 s per path |
| `s23-ultra-shapes.txt` | Galaxy S23 Ultra | `dequant_gemv_int4` across five shapes |
| `quest2.txt` | Meta Quest 2 (XR2 / SM8250) | probe, verify, all three kernels — no readable thermal sensor, so `temp_C` is `null` throughout |
| `quest2-sustained.txt` | Meta Quest 2 | `dequant_gemv_int4`, 180 s per path, 380,382 timed iterations |
| `redmi-k20-pro.txt` | Redmi K20 Pro (SM8150) | probe, verify, all three kernels — 2019 silicon, `dotprod` but no `i8mm`/`bf16` |
| `*.json` | — | machine-readable report; unmeasurable fields are `null` |

### Atlas demo captures — [`demo/`](../demo/README.md)

End-to-end: the same dispatched kernel serving real semantic-search queries
over 25,000 INT4 word vectors. Both include the top-5 neighbours the corpus
returned, so the search is checkable and not just timed.

| File | Device | Contents |
|---|---|---|
| `graviton4-atlas.txt` | AWS `c8g.large` | per-path search timing, all three paths — scalar 3,011 µs, neon 1,455 µs, sve 1,536 µs |
| `k20-pro-atlas.txt` | Redmi K20 Pro | interleaved scalar/NEON A/B on device, median of 15 pairs |

The K20 Pro capture interleaves the two paths request by request rather than
running one path to completion and then the other, so both see the same core
and the same cache state. Each A/B line is an independent request; the capture
aborts on a failure rather than repeating a previous reading.

## Reading the numbers

- `median_ns` / `p95_ns` — per-call, from `std::chrono::steady_clock` on the
  device. Every timed iteration is recorded; these are order statistics over
  the full sample, not an average of a chosen few.
- `vs scalar` — a ratio of two medians measured in the **same run on the same
  machine**, never across devices or sessions.
- `sustained` — median of the first fifth of the run divided by the median of
  the last fifth. Below 1.00 means throughput fell while the run proceeded.
  Reported as `null` for windows under two seconds, where the comparison would
  be noise.
- `temp_C` — the **hottest thermal zone this process could read**, which is not
  the same thing as CPU temperature and on some devices is nowhere near it. On
  the Redmi K20 Pro the hottest readable zone is a PMIC thermistor at ~101 °C
  while the CPU zones read ~36 °C. Treat it as a coarse environmental signal;
  use `sustained` to reason about throttling, because that one is derived from
  measured throughput rather than from a sensor whose identity varies by vendor.
- `null` — the platform exposed no way to observe this. Thermal sensors are
  frequently unreadable without root. A null is never a zero and never an
  estimate.
