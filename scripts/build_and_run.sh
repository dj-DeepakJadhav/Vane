#!/usr/bin/env bash
# Build Vane and capture a device run on Linux (AWS Graviton, Raspberry Pi,
# Asahi, any AArch64 box) or macOS.
#
#   ./scripts/build_and_run.sh graviton4
#   ./scripts/build_and_run.sh graviton4 --seconds 30
#
# Writes results/<label>.txt verbatim, plus results/<label>.json.
# Exits non-zero if the equivalence verifier fails, because benchmark
# numbers from a build that disagrees with its own oracle are worthless.
#
# Copyright 2026 The Vane Authors. Apache License 2.0.
set -euo pipefail

LABEL="${1:-$(uname -m)-$(date +%s)}"
shift || true
SECONDS_ARG=10
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
        --seconds) SECONDS_ARG="$2"; shift 2 ;;
        *) EXTRA+=("$1"); shift ;;
    esac
done

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

echo "==> configuring"
cmake -B build -DCMAKE_BUILD_TYPE=Release

echo "==> building"
cmake --build build -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)"

mkdir -p results
OUT="results/${LABEL}.txt"

# --- provenance header -------------------------------------------------------
{
    echo "vane device run"
    echo "label      : ${LABEL}"
    echo "captured   : $(date -Is)"
    echo "uname      : $(uname -srm)"
    echo "compiler   : $("${CXX:-c++}" --version | head -n1)"
    if [ -r /proc/cpuinfo ]; then
        echo "cpu impl   : $(grep -m1 -E 'CPU implementer|model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ *//' || true)"
        echo "cores      : $(nproc)"
    fi
    if command -v lscpu >/dev/null 2>&1; then
        echo "lscpu model: $(lscpu | grep -m1 -E 'Model name' | cut -d: -f2- | sed 's/^ *//' || true)"
        echo "lscpu flags: $(lscpu | grep -m1 -E '^Flags' | cut -d: -f2- | sed 's/^ *//' || true)"
    fi
    # Cloud provenance, when present. Harmless and silent elsewhere.
    if command -v curl >/dev/null 2>&1; then
        TOK=$(curl -s -m 1 -X PUT "http://169.254.169.254/latest/api/token" \
              -H "X-aws-ec2-metadata-token-ttl-seconds: 60" 2>/dev/null || true)
        if [ -n "${TOK}" ]; then
            echo "ec2 type   : $(curl -s -m 1 -H "X-aws-ec2-metadata-token: ${TOK}" \
                 http://169.254.169.254/latest/meta-data/instance-type 2>/dev/null || true)"
            echo "ec2 region : $(curl -s -m 1 -H "X-aws-ec2-metadata-token: ${TOK}" \
                 http://169.254.169.254/latest/meta-data/placement/region 2>/dev/null || true)"
        fi
    fi
    echo "args       : --seconds ${SECONDS_ARG} ${EXTRA[*]:-}"
    echo
    echo "This file is captured verbatim from the machine. Nothing in it is edited."
    printf '=%.0s' {1..78}; echo
    echo
} > "${OUT}"

# --- probe -------------------------------------------------------------------
echo "--- vane_probe ---"  >> "${OUT}"
./build/vane_probe         >> "${OUT}"
echo                         >> "${OUT}"

# --- verify ------------------------------------------------------------------
echo "--- vane_verify ---" >> "${OUT}"
set +e
./build/vane_verify        >> "${OUT}"
VERIFY_RC=$?
set -e
echo "verify exit code: ${VERIFY_RC}" >> "${OUT}"
echo                         >> "${OUT}"

# --- bench -------------------------------------------------------------------
echo "--- vane_bench ---"  >> "${OUT}"
# ${EXTRA[@]+"${EXTRA[@]}"} expands to NOTHING when EXTRA is empty.
# "${EXTRA[@]:-}" would instead expand to a single empty string, which
# vane_bench rightly rejects as an unknown argument, exiting 2 and taking the
# whole script down under `set -e`. Caught on Graviton4.
./build/vane_bench --seconds "${SECONDS_ARG}" ${EXTRA[@]+"${EXTRA[@]}"} \
    --report "results/${LABEL}.json" >> "${OUT}"

echo
cat "${OUT}"
echo
if [ "${VERIFY_RC}" -eq 0 ]; then
    echo "VERIFY PASSED - all available paths agree with the scalar oracle"
    echo "wrote results/${LABEL}.txt and results/${LABEL}.json"
else
    echo "VERIFY FAILED (exit ${VERIFY_RC}) - do not publish these numbers" >&2
    exit "${VERIFY_RC}"
fi
