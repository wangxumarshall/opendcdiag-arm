#!/bin/bash
# sdc_fuzz_run.sh — load orchestrator for the 100 core-179 SDC reproducer tests.
#
# Reproduces the verified core-179 SDC trigger conditions (report §11.2 / §13.2):
#   - target core 179 (PkgID 19062, socket 4, cores 144-191) runs the test
#   - same-socket full load (>=47 cores of socket 19062) provides the pressure
#   - single run <= 75s + timeout wrap (avoids the §11.3 system hang)
#
# Two modes:
#   single:  run ONE sdc_fuzz case on 179 under 47-core eigen_sparse load
#   batch:   run ALL 100 sdc_fuzz cases on 179 under 47-core eigen_sparse load
#   subset:  run a subset (comma-separated ids, e.g. 000,001,050)
#
# Usage:
#   ./sdc_fuzz_run.sh single sdc_fuzz_000        [duration_s]
#   ./sdc_fuzz_run.sh batch                       [duration_s_per_case]
#   ./sdc_fuzz_run.sh subset 000,001,050          [duration_s_per_case]
#
# LOAD SOURCE (critical): the trigger is NOT activated by a simple CPU load
# generator (loadgen). It requires the SAME Sparse Cholesky instruction stream
# running on the 47 load cores — i.e. opendcdiag `eigen_sparse` under load
# (report §13.2 standard command). So the load here is opendcdiag running
# `eigen_sparse` on the 47 cores (excluding 179) with the report's fixed seed.
# Requires: builddir_sdc/opendcdiag (built).
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 Intel Corporation.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# PORTING NOTE: scripts live at $REPO/scripts/sdc/ (one level deeper than the
# reference tree's $REPO/scripts/), so REPO needs an extra "..".
REPO="$(cd "$HERE/../.." && pwd)"
BUILD="$REPO/builddir_sdc"
BIN="$BUILD/opendcdiag"

TARGET_CORE=179
# socket 19062 cores 144-191, exclude the target core (179)
LOAD_CORES_LIST="$(seq 144 178 | tr '\n' ','),$(seq 180 191 | tr '\n' ',' | sed 's/,$//')"
# report's fixed failing seed (§13.2)
LOAD_SEED="LCG:323306158"

MODE="${1:-single}"
CASEARG="${2:-sdc_fuzz_000}"
DUR="${3:-60}"   # seconds per case

if [ ! -x "$BIN" ]; then
    echo "ERROR: $BIN not built. Run: ninja -C $BUILD opendcdiag" >&2
    exit 2
fi

# start 47-core same-socket eigen_sparse load (the verified trigger condition).
# This runs the Sparse Cholesky stream on all 47 load cores, which is what
# activates core 179's SDC (a simple loadgen loop does NOT activate it).
LOAD_PID=0
start_load() {
    local loaddur=$((DUR + 15))
    echo "[orchestrator] starting 47-core eigen_sparse load (exclude $TARGET_CORE, seed $LOAD_SEED, ${loaddur}s)..."
    LD_LIBRARY_PATH="${HOME}/rpmroot/sysroot/usr/lib64:${LD_LIBRARY_PATH:-}" \
    timeout "$((loaddur + 10))" "$BIN" --beta -e eigen_sparse \
        --cpuset "$LOAD_CORES_LIST" -s "$LOAD_SEED" -T "${loaddur}s" \
        --output-format=tap -o /dev/null >/dev/null 2>&1 &
    LOAD_PID=$!
    # give the load a moment to spin up
    sleep 4
}

stop_load() {
    if [ "$LOAD_PID" != "0" ]; then
        kill "$LOAD_PID" 2>/dev/null || true
        wait "$LOAD_PID" 2>/dev/null || true
    fi
    # also kill any stragglers (defensive)
    pkill -f "eigen_sparse --cpuset" 2>/dev/null || true
    wait 2>/dev/null || true
}

run_one() {
    local case_id="$1" dur="$2"
    echo "[orchestrator] running $case_id on core $TARGET_CORE under load (${dur}s)..."
    # opendcdiag: -e <test> --cpuset <core> -T <dur> --output-format=tap
    # --cpuset pins the test's execution threads to the target core
    LD_LIBRARY_PATH="${HOME}/rpmroot/sysroot/usr/lib64:${LD_LIBRARY_PATH:-}" \
    timeout "$((dur + 20))" \
    taskset -c "$TARGET_CORE" "$BIN" --beta -e "$case_id" \
        --cpuset "$TARGET_CORE" -T "${dur}s" --output-format=tap \
        -o /dev/null 2>&1 | tail -20 || true
}

trap stop_load EXIT INT TERM

case "$MODE" in
    single)
        start_load
        run_one "$CASEARG" "$DUR"
        stop_load
        ;;
    batch)
        start_load
        for i in $(seq 0 99); do
            id=$(printf "sdc_fuzz_%03d" "$i")
            run_one "$id" "$DUR"
        done
        stop_load
        ;;
    subset)
        IFS=',' read -ra IDS <<< "$CASEARG"
        start_load
        for i in "${IDS[@]}"; do
            id=$(printf "sdc_fuzz_%03d" "$((10#$i))")
            run_one "$id" "$DUR"
        done
        stop_load
        ;;
    *)
        echo "Usage: $0 {single|batch|subset} [case_id|ids] [duration_s]" >&2
        exit 1
        ;;
esac

echo "[orchestrator] done."
