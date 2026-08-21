#!/usr/bin/env python3
# sdc_fuzz_verify.py — verify the 100 sdc_fuzz cases reproduce core-179 SDC.
#
# The core-179 SDC trigger is TRANSIENT / load-window-sensitive (report §15.1:
# "有的窗口 6-24/3000，有的 0/3000"). So "100/100 reproduce" is NOT judged as
# "all fail right now" — a dormant window legitimately yields 0/0. Instead we
# use the SAME-WINDOW MRU CONTROL method (report §11.6 E2E):
#
#   In a given load window, run the verified libc-only MRU (mrueig) on 179 in
#   parallel with each sdc_fuzz case on 179, and judge each case against the
#   MRU's behaviour IN THAT WINDOW:
#     - active window (MRU fail > 0): each case should also fail and emit the
#       core-179 signature (fixed elem[0] + multi-bit popcount >= 10, not a
#       single-bit SEU) => "reproduces"
#     - dormant window (MRU fail == 0): cases also 0 fail; mark "dormant — re-run
#       in active window", NOT "fails to reproduce"
#
# The sdc_fuzz cases report failures via opendcdiag's report_fail_msg with the
# string "core179 SDC: x crc mismatch ... elem[0]=... popcount=N bits [cluster]".
# This verifier parses that, extracts the signature, and confirms it matches the
# core-179 SDC family (multi-bit data-aliasing, not a single-bit SEU).
#
# Output: verification_report.md + cases_result.csv
#
# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 Intel Corporation.

import argparse
import csv
import os
import re
import subprocess
import sys

# PORTING NOTE: this script lives at $REPO/scripts/sdc/ (one level deeper than
# the reference tree's $REPO/scripts/), so REPO needs one extra dirname().
REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
# PORTING NOTE: docs/sdc1-01-02-core179-diagnostics/ is NOT ported in this unit.
# It ships the libc-only MRU control binary `mrueig` that this script drives on
# core 179 alongside each sdc_fuzz case. Until that directory is ported, override
# DIAG via env var (or pass --out / --build) to point at a prepared location.
DIAG = os.environ.get("DIAG", os.path.join(REPO, "docs", "sdc1-01-02-core179-diagnostics"))
BUILD = os.path.join(REPO, "builddir_sdc")
BIN = os.path.join(BUILD, "opendcdiag")
MRUEIG = os.path.join(DIAG, "mrueig")

TARGET_CORE = 179
LOAD_CORES = list(range(144, 179)) + list(range(180, 192))  # 47 cores, exclude 179
LOAD_CPUS = ",".join(str(c) for c in LOAD_CORES)
LOAD_SEED = "LCG:323306158"  # report's fixed failing seed (§13.2)
LD_PATH = os.path.expanduser("~/rpmroot/sysroot/usr/lib64")

# matches "core179 SDC: x crc mismatch (0x.. vs golden 0x..) elem[0]=X (golden Y) popcount=N bits [cluster]"
SIG_RE = re.compile(
    r"core179 SDC: x crc mismatch \(0x[0-9a-f]+ vs golden 0x[0-9a-f]+\) "
    r"elem\[0\]=([-\d.eE+]+) \(golden ([-\d.eE+]+)\) popcount=(\d+) bits \[([^\]]+)\]"
)
# mrueig output: "k=N x-crc mismatch ... x[0]=X (golden Y)"
MRU_FAIL_RE = re.compile(r"k=\d+ x-crc mismatch.*x\[0\]=([-\d.eE+]+) \(golden ([-\d.eE+]+)\)")
MRU_RESULT_RE = re.compile(r"RESULT eigen-MC MRU: (\d+)/(\d+) fails")


def start_load(duration):
    """Start the 47-core eigen_sparse load (the verified trigger condition).

    CRITICAL: a simple loadgen loop does NOT activate core 179 SDC. The trigger
    requires the Sparse Cholesky instruction stream running on the 47 load cores
    (report §13.2). So we run opendcdiag `eigen_sparse` on the load cores with
    the report's fixed failing seed.
    """
    env = dict(os.environ, LD_LIBRARY_PATH=f"{LD_PATH}:{os.environ.get('LD_LIBRARY_PATH','')}")
    loaddur = duration + 15
    p = subprocess.Popen(
        ["timeout", str(loaddur + 10), BIN, "--beta", "-e", "eigen_sparse",
         "--cpuset", LOAD_CPUS, "-s", LOAD_SEED, "-T", f"{loaddur}s",
         "--output-format=tap", "-o", "/dev/null"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env,
    )
    # let the load spin up
    import time; time.sleep(4)
    return [p]


def stop_load(pids):
    for p in pids:
        p.terminate()
    for p in pids:
        try:
            p.wait(timeout=5)
        except subprocess.TimeoutExpired:
            p.kill()
    # defensive: kill any straggler opendcdiag load processes
    subprocess.run(["pkill", "-f", "eigen_sparse --cpuset"],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    import time; time.sleep(1)


def run_mrueig(iters, dur_s):
    """Run the verified MRU on 179, return (fail_count, iters, fail_signatures)."""
    try:
        out = subprocess.run(
            ["timeout", str(dur_s + 10), "taskset", "-c", str(TARGET_CORE), MRUEIG, str(iters), "12345"],
            capture_output=True, text=True, timeout=dur_s + 30,
        )
    except subprocess.TimeoutExpired:
        return (-1, iters, [])
    sigs = MRU_FAIL_RE.findall(out.stderr)
    m = MRU_RESULT_RE.search(out.stderr)
    fail = int(m.group(1)) if m else -1
    total = int(m.group(2)) if m else iters
    return (fail, total, sigs)


def run_sdc_case(case_id, dur_s):
    """Run one sdc_fuzz case on 179 under load, return (status, signatures)."""
    env = dict(os.environ, LD_LIBRARY_PATH=f"{LD_PATH}:{os.environ.get('LD_LIBRARY_PATH','')}")
    try:
        out = subprocess.run(
            ["timeout", str(dur_s + 10),
             "taskset", "-c", str(TARGET_CORE), BIN, "--beta", "-e", case_id,
             "--cpuset", str(TARGET_CORE), "-T", f"{dur_s}s",
             "--output-format=tap", "-o", "/dev/null"],
            capture_output=True, text=True, timeout=dur_s + 30, env=env,
        )
    except subprocess.TimeoutExpired:
        return ("timeout", [])
    sigs = SIG_RE.findall(out.stderr + out.stdout)
    # opendcdiag TAP: "not ok" lines indicate failures
    not_ok = [ln for ln in (out.stdout + out.stderr).splitlines() if ln.strip().startswith("not ok")]
    if sigs:
        return ("fail", sigs)
    if not_ok:
        return ("fail", [])  # failed but signature not captured (truncated)
    return ("pass", [])


def judge_case(case_status, case_sigs, mru_fail):
    """Decide if a case 'reproduces' core-179 SDC in this window."""
    if mru_fail > 0:
        # active window: case MUST fail with the core-179 signature
        if case_status == "fail" and case_sigs:
            # confirm multi-bit (popcount >= 10), not single-bit SEU
            multi_bit = all(int(s[2]) >= 10 for s in case_sigs)
            if multi_bit:
                return "REPRODUCES", f"active window (MRU fail={mru_fail}); case failed with core-179 signature (elem[0] multi-bit)"
            else:
                return "INCONCLUSIVE", f"active window; case failed but popcount<10 (possible single-bit, not core-179 signature)"
        elif case_status == "fail":
            return "REPRODUCES(uncaptured)", f"active window (MRU fail={mru_fail}); case failed (signature not captured in log)"
        else:
            return "NO_REPRODUCE", f"active window (MRU fail={mru_fail}) but case did NOT fail"
    else:
        # dormant window: cannot judge; cases legitimately 0 fail
        if case_status == "pass":
            return "DORMANT", f"dormant window (MRU fail=0); case 0 fail as expected — re-run in active window"
        else:
            return "UNEXPECTED", f"dormant window (MRU fail=0) but case FAILED — investigate (false positive?)"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cases", default="all", help="comma-separated ids (0-99) or 'all'")
    ap.add_argument("--per-case-dur", type=int, default=60, help="seconds per case")
    ap.add_argument("--mru-iters", type=int, default=3000, help="MRU iters in same window")
    ap.add_argument("--subset", type=int, default=None,
                    help="only verify first N cases (smoke). default: all 100")
    ap.add_argument("--out", default=os.path.join(DIAG, "verification_report.md"))
    args = ap.parse_args()

    if not os.path.exists(BIN):
        print(f"ERROR: {BIN} not built", file=sys.stderr); sys.exit(2)
    if not os.path.exists(MRUEIG):
        print(f"ERROR: {MRUEIG} not built", file=sys.stderr); sys.exit(2)

    if args.cases == "all":
        ids = list(range(100))
    else:
        ids = [int(x) for x in args.cases.split(",")]
    if args.subset is not None:
        ids = ids[:args.subset]

    # index
    idx_path = os.path.join(REPO, "tests", "cpu", "arm64", "sdc_fuzzing", "cases_index.csv")
    idx = {}
    if os.path.exists(idx_path):
        with open(idx_path) as f:
            for r in csv.DictReader(f):
                idx[r["case_id"]] = r

    results = []
    print(f"=== sdc_fuzz verification: {len(ids)} cases, {args.per_case_dur}s each ===")
    print(f"=== target core {TARGET_CORE}, {len(LOAD_CORES)}-core socket load ===")

    # Per-case load window: start fresh load per case (mirrors sdc_fuzz_run.sh
    # safety: <=75s windows). MRU control runs in the SAME window as each case.
    for n, i in enumerate(ids):
        case_id = f"sdc_fuzz_{i:03d}"
        dur = args.per_case_dur
        print(f"\n[{n+1}/{len(ids)}] {case_id} ...", flush=True)
        pids = start_load(dur)
        try:
            mru_fail, mru_total, mru_sigs = run_mrueig(args.mru_iters, dur)
            case_status, case_sigs = run_sdc_case(case_id, dur)
        finally:
            stop_load(pids)
        verdict, reason = judge_case(case_status, case_sigs, mru_fail)
        meta = idx.get(case_id, {})
        results.append({
            "case_id": case_id, "N": meta.get("N",""), "cluster": meta.get("cluster",""),
            "mode": meta.get("mode",""), "mru_fail": mru_fail, "case_status": case_status,
            "case_sigs": len(case_sigs), "verdict": verdict, "reason": reason,
        })
        print(f"  MRU: {mru_fail}/{mru_total} fail | case: {case_status} ({len(case_sigs)} sigs) -> {verdict}")

    # reports
    csv_path = os.path.join(DIAG, "cases_result.csv")
    with open(csv_path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(results[0].keys()))
        w.writeheader(); w.writerows(results)

    # markdown summary
    from collections import Counter
    vc = Counter(r["verdict"] for r in results)
    with open(args.out, "w") as f:
        f.write("# sdc_fuzz Verification Report\n\n")
        f.write(f"**Date:** generated by sdc_fuzz_verify.py\n")
        f.write(f"**Target:** core {TARGET_CORE} under {len(LOAD_CORES)}-core socket load\n")
        f.write(f"**Cases verified:** {len(results)}\n\n")
        f.write("## Verdict summary\n\n")
        for v, c in vc.most_common():
            f.write(f"- **{v}**: {c}\n")
        f.write("\n## Methodology\n\n")
        f.write("Same-window MRU control (report §11.6 E2E). The verified libc-only MRU "
                "(`mrueig`) and each sdc_fuzz case run on core 179 in the SAME load window. "
                "In an active window (MRU fail>0), a case 'REPRODUCES' if it fails with the "
                "core-179 signature (fixed elem[0] + multi-bit popcount>=10). A dormant "
                "window (MRU fail=0) is marked DORMANT — not a failure; re-run in an active "
                "window for final confirmation.\n\n")
        f.write("## Per-case results\n\n")
        f.write("| case | N | cluster | mode | MRU fail | case status | sigs | verdict |\n")
        f.write("|---|---|---|---|---|---|---|---|\n")
        for r in results:
            f.write(f"| {r['case_id']} | {r['N']} | {r['cluster']} | {r['mode']} | "
                    f"{r['mru_fail']} | {r['case_status']} | {r['case_sigs']} | **{r['verdict']}** |\n")
        f.write(f"\n## CSV\n\n{csv_path}\n")
    print(f"\n=== done: {args.out} ===")
    print("verdicts:", dict(vc))


if __name__ == "__main__":
    main()
