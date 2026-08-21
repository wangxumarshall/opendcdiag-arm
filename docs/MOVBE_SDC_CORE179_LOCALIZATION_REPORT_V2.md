# Core 179 SDC — Localization Report (v2, corrected)

**Status:** Macroscopic localization complete; micro-architectural mechanism qualitatively pinned, not quantified
**Date:** 2026-08-19
**Supersedes:** `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT.md` (2026-08-18) — that report's "store↔reload back-to-back is a required trigger condition" and "F/G/H methodological confound" conclusions are **OVERTURNED** by the multi-seed campaigns and objdump re-verification in this v2. The old report is retained for history only.
**Target machine:** 192-core aarch64 (8 NUMA nodes × 24 cores), Linux 6.6.0-145, package 19062
**Test harness:** `opendcdiag` (meson+ninja build, `builddir/opendcdiag`), auto-seed, `-t 5m --max-test-loop-count=0`

---

## 1. Executive summary

A silent data corruption (SDC) occurs on **logical core 179** (NUMA node 7, module 23340). It is **single-core** (every recorded failure lands on the exact same bit of the cpu-mask: segment 4, bit 36 → global bit 179) and **probabilistic** — the same seed may fail on one retry iteration and pass on the next.

**The defect is NOT movbe-specific.** Four unrelated workload types all trigger it on core 179:
- `movbe` / `movbe_dump` — byte-swap round-trip (most frequent)
- `mrn_rmw` — integer read-modify-write with store-to-load forwarding
- `eigen_gemm_float_dynamic_square`, `eigen_gemm_cdouble_dynamic_square`, `eigen_gemm_double14` — floating-point GEMM (FMA)

All share the same micro-architectural pattern: **a store immediately followed by a load + a data-dependency chain + a cross-line advancing footprint.** This localizes the defect to core 179's **load/store unit (LSU) or store-buffer forwarding path at a particular instruction-scheduling phase**, not to any instruction's semantics.

**The headline finding of v2 (overturning v1):** the trigger is **not** a clean "address + topology" condition. It is a **timing-phase race**: a single semantically-no-op ALU instruction added to the hot loop drops the trigger rate from ~100% (baseline) to ~10–20% (probes H and X), even when the store address and footprint are byte-identical to baseline. The store↔reload "back-to-back" adjacency — which v1 believed was a required trigger condition — is **not** a discriminator: probe X breaks back-to-back and still triggers at the same rate as probe H which keeps it.

---

## 2. The defect and the flip site

### 2.1 Baseline hot loop (movbe_dump.cpp, objdump @0xb9cf0)

```
ldp  x1, x3, [x20]            ; load input ptr (x1), swapped ptr (x3)
...
ldr  w5, [x1, x19, lsl #2]    ; 1st read of data->input[i]
rev  w2, w5                    ; byte-swap (never feeds the compare)
str  w2, [x3, x19, lsl #2]    ; store to data->swapped[i] (different line)
ldr  w4, [x1, x19, lsl #2]    ; ★ 2nd read (RELOAD) of data->input[i]  <- SDC site
cmp  w4, w5                    ; catches w4 != w5
```

The flip occurs on the **reload `ldr` of `data->input[i]`** (the 4th instruction). A correctly-functioning core returns the same value just read into w5; a defective core 179 intermittently returns a different value. The XOR of (golden ^ actual) differs every fail — the flipped bits are not constant, consistent with a probabilistic corruption rather than a stuck bit.

---

## 3. Locked-down trigger conditions (high confidence)

Established by control probes (A–H) where exactly one variable was changed. All multi-seed re-run under auto-seed (correcting v1's fixed-seed methodology error — see §8).

### 3.1 Required conditions (probes that REMOVE a condition → PASS)

| Probe | What it removes | Result | Conclusion |
|-------|-----------------|--------|------------|
| **A** | the store (no store side-effect) | PASS | A store is **required** |
| **D** | address advancement (fixed-address store) | PASS | The store address must **advance** |
| **E** | same-LLC-domain (swapped on a different NUMA) | PASS | Store & reload must be in the **same LLC domain** |
| **F** | cross-line stepping (store confined to one cache line, i&15) | PASS | The store must **advance across cache lines** |

### 3.2 Conditions that are NOT the discriminator (v2 corrections)

| Probe | What it changes | Trigger rate | Conclusion |
|-------|-----------------|--------------|------------|
| **baseline** | nothing | ~100% (5/5 seeds) | reference |
| **H** (LINES=512) | adds `and x2,x19,x20` — a **semantic no-op** (i&16383==i since i<16384), so store address == baseline, store↔reload stays **back-to-back** (objdump-verified) | ~10% (1/10) | one extra no-op ALU instr drops rate 100%→10% **without** touching address/footprint/back-to-back |
| **X** | adds `eor w?,w?,wzr` — a **semantic no-op**; store footprint == baseline, but store↔reload **NOT back-to-back** (compiler keeps 2nd rev between str and ldr) | ~20% (1/5) | breaking back-to-back does **not** eliminate the trigger; rate is same order as H |

**v2 overturns v1 on two points:**
1. ~~Store↔reload must be back-to-back~~ → **REFUTED**: probe X breaks it and still triggers. (All 5 probes A–H are back-to-back in v1's objdump, so back-to-back was never actually a discriminator — v1 misread the data.)
2. ~~The H probe's `and` mask kills the defect by breaking back-to-back~~ → **REFUTED**: objdump shows H's `and` is at the loop TOP before the first `ldr`, and store↔reload stays back-to-back. H's rate drop is from the **extra instruction itself**, not from breaking adjacency.

### 3.3 What IS the discriminator

**Instruction-scheduling timing phase.** The common factor across H (~10%) and X (~20%) is one extra semantically-no-op ALU instruction on the hot path. Adding it — without changing the store address, footprint, or (for H) back-to-back adjacency — collapses the rate from deterministic (~100%) to probabilistic (~10–20%). The defect is a **per-iteration pipeline-phase race** in core 179's store-buffer/load-buffer, where a 1-issue-slot phase shift probabilistically mis-forwards or mis-reads.

⚠ **Sample limitation:** H is 1/10, X is 1/5 — the difference is not statistically significant at these sample sizes. What IS significant: both are far below baseline's 100% and both are far above 0%. The conclusion is directional (timing-phase is the cause; back-to-back is not), not a precise rate curve.

---

## 4. Cross-workload evidence (defect is on core 179's common execution path)

Historical logs (all in `movbe_log/other_sdc/`) were scanned for failures. Every true SDC lands on core 179.

| Workload | Type | Fails | Source log | Failure recording |
|----------|------|-------|-----------|--------------------|
| `movbe` / `movbe_dump` | byte-swap round-trip | 11 + 2 + 1 + 4 + many | all_tests_2h, all_cores_10m_20260816, opendcdiag-20260818, all_cores_10m_88GB | full input/golden/actual/xor + per-byte dump (custom log_error) |
| `mrn_rmw` | integer RMW + store-to-load forwarding | 2 | all_tests_2h | "data miscompare" only — **no value dump** |
| `eigen_gemm_float_dynamic_square` | float GEMM (FMA) | 1 | all_tests_2h | actual/expected byte dump (framework memcmp_or_fail) |
| `eigen_gemm_cdouble_dynamic_square` | complex-double GEMM (FMA) | 1 | all_tests_2h | actual/expected byte dump |
| `eigen_gemm_double14` | double GEMM (FMA) + copy/verify | 44 | eigen stress 20260812 | actual/expected byte dump (all_fail_blocks.log) |

**Non-SDC failures** (architecture, not hardware): `zero_control_vec_sse_jit` (SSE on aarch64), `movmskpspd` (x86 movmsk) — these fail on **all 128 cores** deterministically with identical error values, not single-core intermittent.

### 4.1 Workload commonality — why unrelated tests trigger the same defect

| Common trait | movbe | mrn_rmw | eigen×3 |
|--------------|-------|---------|---------|
| store immediately followed by load (same/related address) | ✓ (store swapped[i] → reload input[i]) | ✓ (store temp[i] → immediately load temp[i], objdump-confirmed `str`→`ldr` adjacent) | partial (double14's copy→compute→verify has store/load round-trips) |
| ALU/FMA data-dependency chain | ✓ (rev bswap) | ✓ (add/sub/xor/and) | ✓ (FMA) |
| large cross-line advancing footprint | ✓ (64KB) | ✓ (8KB, cross-line) | ✓ (hundreds of KB) |

The defect is in core 179's **common store/load pipeline**, not in any instruction's semantics. movbe triggers most because its "store swapped[i] → immediately reload input[i]" is the tightest, most phase-stable store→load sequence. mrn_rmw triggers because it has an explicit same-address store-to-load forwarding (`str`→`ldr` adjacent, the closest structural analog to movbe). eigen GEMM triggers rarely because its store/load round-trips only occur at result writeback/copy, but double14 (with extra copy→verify round-trips) triggers far more than the other two eigen variants — confirming more store/load round-trips ⇒ higher trigger probability.

---

## 5. Failure-recording capability per test (the value-dump audit)

| Test | Records input value? | Records golden/expected? | Records actual? | Records xor? | Mechanism |
|------|----------------------|-------------------------|-----------------|--------------|-----------|
| `movbe_dump` | **Yes** (`input=`) | **Yes** (`golden=`) | **Yes** (`actual=`) | **Yes** (`xor=`) | custom `log_error` with full per-byte breakdown — **most complete** |
| `mrn_rmw` | **No** | **No** (only `expected[]` array exists, not logged) | **No** | **No** | only `report_fail_msg("mrn_rmw data miscompare")` — logs message + source line, **no values** |
| `eigen_gemm_float_dynamic_square` | No | **Yes** (expected ptr passed) | **Yes** (actual ptr passed) | No (byte diff only) | framework `memcmp_or_fail` → `logging_report_mismatched_data` dumps `actual data:`/`expected data:` byte blocks + mismatch offset |
| `eigen_gemm_cdouble_dynamic_square` | No | Yes | Yes | No | same framework path |
| `eigen_gemm_double14` | No | Yes | Yes | No | same framework path; also `_x/_y/_prod.isApprox` + memcmp checks give 3 failure points |

**Summary:** Only `movbe_dump` records the full input/golden/actual/xor quartet per-failure. `mrn_rmw` records **nothing** about the failing values (a gap — its `expected[]` array exists but is not dumped on miscompare). The eigen variants record actual-vs-expected byte blocks via the framework, which is sufficient to see the corruption but does not separate "input" from "golden" (they are the same buffer read at different times, like movbe).

**Recommendation:** if richer diagnostics are wanted on core 179 across workloads, `mrn_rmw` is the weakest — it should be upgraded to use `memcmp_or_fail(dst, data->expected, ...)` instead of `report_fail_msg`, which would give it the framework's actual/expected byte dump for free.

---

## 6. Final localization statement

**Macroscopic (locked, high confidence):**
- **Core:** logical 179 (NUMA 7, module 23340), bit 179 in every cpu-mask, never drifts.
- **Flip site:** the reload `ldr` of the just-read input word (movbe's 4th hot-loop instruction; structurally analogous reload in the other workloads).
- **Required conditions:** a store exists + the store address advances + the store is in the same LLC domain as the reload + the store advances across cache lines.

**Micro-architectural mechanism (qualitatively pinned, not quantified):**
- **Cause:** instruction-scheduling timing-phase race in core 179's load/store unit or store-buffer forwarding path. One semantically-no-op ALU instruction on the hot path shifts the per-iteration issue phase and collapses deterministic triggering (~100%) to probabilistic (~10–20%).
- **Refuted:** store↔reload back-to-back adjacency is NOT a discriminator (probe X breaks it, still triggers); the H mask does NOT kill the defect by breaking adjacency (objdump: mask is at loop top, adjacency preserved).

**What remains unquantified:** the exact pipeline stage / phase window width, the per-instruction-type sensitivity curve, and a precise trigger-rate-vs-extra-instructions function. These would require larger seed campaigns (≥30 seeds per probe) and PMU/store-buffer event sampling on core 179.

---

## 7. Log archive

All movbe and variant logs, plus the cross-workload SDC logs, are archived under `movbe_log/` with a per-campaign subdirectory structure and a `README.md` index. See `movbe_log/README.md`.

## 8. Methodology notes / corrections from v1

1. **Fixed LCG seed error (v1):** v1 ran single-seed probes with `-s LCG:<fixed>`. A fixed seed is one input pattern; SDC is seed-sensitive; retry reuses the same seed. v1's "H LINES=512 PASS" was pure seed luck (it FAILED under auto-seed). All v2 probes use auto-seed.
2. **objdump address-mapping error (v1, corrected mid-v2):** v1's movbe_run addresses for F/G1/G2 were off by one test struct; re-mapped via unique `MovBE-probeX` string anchors. H's address was already correct. No v1 conclusion relied on the wrong addresses, but they are corrected here.
3. **Infinite-retry script bug (v2):** early v2 campaign scripts let a failing seed retry indefinitely (opendcdiag retries on failure with no hard cap bounded by `-t`). Fixed by wrapping each run in `timeout`.
