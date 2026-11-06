#!/usr/bin/env bash
# run_sweep.sh — sweep the benchmark over thread counts, read mixes, and
# distributions, collecting one CSV line per configuration.
#
# Prereq: build lru_bench first (see README). This script does not build.
#
# Output: a single CSV on stdout with a header, suitable for piping to a file:
#   ./run_sweep.sh > results.csv
set -euo pipefail

# Locate the benchmark binary. Allow override via $BENCH.
BENCH="${BENCH:-./build/lru_bench}"
if [[ ! -x "$BENCH" ]]; then
  echo "error: benchmark binary not found at '$BENCH'." >&2
  echo "       build it first, or set BENCH=/path/to/lru_bench" >&2
  exit 1
fi

# Tunables (kept modest so a sweep finishes in a reasonable time).
KEYS="${KEYS:-100000}"
CAPACITY="${CAPACITY:-10000}"
DURATION="${DURATION:-5}"
RUNS="${RUNS:-3}"
SEED="${SEED:-1}"

# Thread counts: 1 2 4 8 plus the machine's core count (deduped if it is one of those).
NPROC="$(nproc)"
THREAD_LIST=(1 2 4 8)
if [[ ! " ${THREAD_LIST[*]} " =~ " ${NPROC} " ]]; then
  THREAD_LIST+=("$NPROC")
fi

READ_PCTS=(50 90)
DISTS=(uniform zipf)

# CSV header matches bench.cpp's CSV line fields.
echo "threads,read_pct,dist,median_ops_sec,p50_ns,p99_ns"

for dist in "${DISTS[@]}"; do
  for rp in "${READ_PCTS[@]}"; do
    for th in "${THREAD_LIST[@]}"; do
      # Human-readable summary goes to stderr; CSV line to stdout.
      "$BENCH" \
        --threads "$th" \
        --read-pct "$rp" \
        --keys "$KEYS" \
        --capacity "$CAPACITY" \
        --duration-sec "$DURATION" \
        --dist "$dist" \
        --runs "$RUNS" \
        --seed "$SEED"
    done
  done
done
