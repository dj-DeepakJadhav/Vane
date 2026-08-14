# Running Vane on AWS Graviton

A runbook for reproducing the server-class Arm capture. This is the only
hardware in the project that executes the SVE path — no phone or headset we
have tested exposes SVE at all.

Result of the run this describes: [`results/graviton4.txt`](results/graviton4.txt).

## Why `c8g`, not `c7g`

| Family | Core | Reports |
|---|---|---|
| `c7g` | Graviton3, Neoverse V1 | `sve` only |
| **`c8g`** | **Graviton4, Neoverse V2** | **`sve` and `sve2`** |

`c8g.large` (2 vCPU) is enough, and it is billed by the second — the captures
below take a few minutes. Check [current on-demand
pricing](https://aws.amazon.com/ec2/pricing/on-demand/) for your region.

## Launch

1. EC2 → **Launch instance**
2. AMI: **Ubuntu Server, arm64** — any current LTS or interim release. The
   capture in `results/` was taken on kernel `7.0.0-1006-aws` with GCC 15.2.0.
3. Instance type: **`c8g.large`**
4. Key pair: create or reuse one for SSH
5. Security group: allow **SSH (22)** from your IP, and **8080** from your IP
   if you want to open the Atlas demo directly from the instance's public IP
6. Storage: default (8 GB) is enough
7. Launch, then `ssh -i your-key.pem ubuntu@<public-ip>`

## Build

```bash
sudo apt-get update && sudo apt-get install -y build-essential cmake git
git clone https://github.com/dj-DeepakJadhav/Vane.git && cd Vane
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Probe

```bash
./build/vane_probe
```

The two lines that matter:

```
features        : ...
sve vector len  : ... bits
```

Expect `sve sve2` present and a runtime vector length of **128 bits**. SVE2
does not imply a wide vector, and confirming that rather than assuming it is
much of the point of running this at all — see [the analysis of why SVE loses
to NEON here](results/ANALYSIS.md#sve-is-not-faster-than-neon-here-and-that-is-not-a-disappointment).

## Verify

```bash
./build/vane_verify
```

**Must exit 0.** If the SVE path disagrees with the scalar oracle on this
machine, that is the most important thing the exercise can find. Every path is
checked against the portable C reference at four shapes; disagreement exits
non-zero and fails CI.

## Capture

```bash
./scripts/build_and_run.sh graviton4 --seconds 30
```

Writes `results/graviton4.txt` and `.json` verbatim, recording the EC2 instance
type, region, kernel, compiler and exact command line in the header alongside
the numbers. Commit them unedited:

```bash
git add results/ && git commit -m "results: Graviton4 capture"
```

## Atlas on Graviton

The [demo](demo/README.md) runs here too, which is the Cloud AI leg of the same
demo the phone and headset run.

```bash
./build/atlas_server --data demo/atlas.bin --html demo/atlas.html --port 8080
```

Open `http://<public-ip>:8080` from your own machine and you are looking at
semantic search running on Neoverse V2 over the open internet. The per-path
capture from this instance is
[`results/graviton4-atlas.txt`](results/graviton4-atlas.txt).

## When you are done

**Stop or terminate the instance.** `c8g.large` bills continuously while
running; there is no reason to leave it up after the captures are taken.
