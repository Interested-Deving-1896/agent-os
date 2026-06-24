#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
cd "$REPO_ROOT"

echo "=== Building ===" >&2
pnpm build >&2

RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

run() {
  local name="$1"
  shift
  echo "" >&2
  echo "=== Running $name ===" >&2
  pnpm exec tsx "$@" \
    1> "$RESULTS_DIR/${name}.json" \
    2> >(tee "$RESULTS_DIR/${name}.log" >&2)
}

# Cold-start benchmarks
run "coldstart-echo" \
  scripts/benchmarks/coldstart.bench.ts --workload=echo

run "coldstart-pi-prompt-turn" \
  scripts/benchmarks/coldstart.bench.ts --workload=pi-prompt-turn --iterations=3

# Memory benchmarks
# run "memory-sleep" \
#   --expose-gc scripts/benchmarks/memory.bench.ts --workload=sleep --count=5

run "memory-pi-session" \
  --expose-gc scripts/benchmarks/memory.bench.ts --workload=pi-session --count=3

run "memory-claude-session" \
  --expose-gc scripts/benchmarks/memory.bench.ts --workload=claude-session --count=3

# Session-creation VM-tax benchmark (deterministic, llmock-backed).
# Compares the agentOS VM path vs the bare-node pi-SDK equivalent and gates the
# deterministic metrics against scripts/benchmarks/baseline.json.
# Set BENCH_GATE=1 to fail the run on a regression (CI); set BENCH_UPDATE_BASELINE=1
# to refresh the committed baseline (do this on a clean checkout, review in PR).
echo "" >&2
echo "=== Running session ===" >&2
pnpm exec tsx scripts/benchmarks/session.bench.ts --iterations=5 \
  ${BENCH_GATE:+--gate} ${BENCH_UPDATE_BASELINE:+--update-baseline} \
  1> "$RESULTS_DIR/session.json" \
  2> >(tee "$RESULTS_DIR/session.log" >&2)

echo "" >&2
echo "=== Done. Results in $RESULTS_DIR ===" >&2
