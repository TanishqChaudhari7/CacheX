#!/usr/bin/env bash
#
# Runs every benchmark and saves the raw output under benchmarks/results/.
#
# The point is traceability: every figure quoted in ARCHITECTURE.md and
# RESULTS.md should be findable in a file here, so a reader can check a claim
# instead of taking it on trust, and a future run can be diffed against this one.
#
# Usage:  benchmarks/run_all.sh [build-dir]        (default: build/release)

set -euo pipefail

BUILD="${1:-build/release}"
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$REPO/benchmarks/results"
BIN="$REPO/$BUILD/bin"

if [ ! -x "$BIN/cachex_bench_suite" ]; then
  echo "error: benchmarks not built in $BUILD" >&2
  echo "  cmake -S . -B $BUILD -DCMAKE_BUILD_TYPE=Release -DCACHEX_BUILD_BENCHMARKS=ON" >&2
  echo "  cmake --build $BUILD -j" >&2
  exit 1
fi

mkdir -p "$OUT"

# A busy machine distorts every number. Warn rather than refuse, and the load is
# recorded in environment.txt either way.
if [ "$(uname -s)" = "Darwin" ]; then
  LOAD1=$(sysctl -n vm.loadavg | awk '{print $2}')
  CORES=$(sysctl -n hw.ncpu)
  if awk -v l="$LOAD1" -v c="$CORES" 'BEGIN { exit !(l > c / 4) }'; then
    echo "warning: 1-minute load average is $LOAD1 on $CORES cores; results will be noisy" >&2
  fi
fi

# Recorded alongside the numbers, because the numbers mean nothing without it.
{
  echo "CacheX benchmark environment"
  echo "============================"
  echo "date        : $(date -u '+%Y-%m-%dT%H:%M:%SZ') (UTC)"
  echo "git commit  : $(git -C "$REPO" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  # benchmarks/results is excluded: this very file is being written there, so
  # including it would always report uncommitted changes.
  echo "git dirty   : $(test -n "$(git -C "$REPO" status --porcelain -- . ':(exclude)benchmarks/results' 2>/dev/null)" && echo yes || echo no)"
  echo "uname       : $(uname -smr)"
  if [ "$(uname -s)" = "Darwin" ]; then
    echo "cpu         : $(sysctl -n machdep.cpu.brand_string)"
    echo "cores       : $(sysctl -n hw.ncpu)"
    echo "memory      : $(( $(sysctl -n hw.memsize) / 1024 / 1024 / 1024 )) GB"
    echo "os          : macOS $(sw_vers -productVersion)"
    echo "load        : $(uptime | sed 's/.*load/load/')"
  fi
  echo "compiler    : $(c++ --version | head -1)"
} > "$OUT/environment.txt"

echo "=== environment ==="
cat "$OUT/environment.txt"

# The suite is run several times because the machine is a laptop, not an
# isolated host: a single run is not evidence of anything.
RUNS="${RUNS:-3}"
for i in $(seq 1 "$RUNS"); do
  echo "=== bench_suite run $i/$RUNS ==="
  "$BIN/cachex_bench_suite" > "$OUT/bench_suite.$i.txt" 2>&1
done

echo "=== net_bench ==="
"$BIN/cachex_net_bench" > "$OUT/net_bench.txt" 2>&1

echo "=== persist_bench ==="
"$BIN/cachex_persist_bench" > "$OUT/persist_bench.txt" 2>&1

echo "=== cache_bench (single-threaded Cache) ==="
"$BIN/cachex_bench" > "$OUT/cache_bench.txt" 2>&1

echo
echo "raw output written to benchmarks/results/:"
ls -1 "$OUT"
