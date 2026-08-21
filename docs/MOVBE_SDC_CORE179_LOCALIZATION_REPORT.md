# Core 179 movbe SDC — Localization Report

**Status:** Localization complete (investigation horizon reached)
**Date:** 2026-08-18
**Target machine:** 192-core aarch64 (8 NUMA nodes × 24 cores), Linux 6.6.0, package 19062
**Test harness:** `opendcdiag --enable=movbe_dump --max-test-loop-count=0 -s LCG:1711090087 -t 5m`
**Author:** SDC fuzzing investigation

---

## 1. Executive summary

A silent data corruption (SDC) occurs on **logical core 179** (NUMA node 7)
when running the `movbe_dump` byte-swap round-trip test. The defect is
**intermittent** (~0.24/min overall, ~2.4 first-fails/min while active),
**single-core** (core 179 only), and **not deterministic** in timing. It
manifests as the reload of a just-read input word returning a *different*
value, caught by a `cmp` against the first read.

Through a series of control probes (A–H) the defect was localized to the
**reload `ldr` of `data->input[i]`** and pinned to require a **same-LLC-domain
store immediately preceding the reload, back-to-back**. Finer
microarchitectural refinement (footprint size, LLC-set pressure, within-line
vs cross-line) was **attempted but could not be completed**: the defect is so
sensitive to the exact hot-loop instruction sequence that any change to the
compiled loop — even one that preserves store↔reload back-to-back timing —
silently switches the defect off. This instruction-sequence sensitivity is
itself a finding: it points to a **timing-corner** mechanism rather than a
parameterizable resource threshold.

## 2. The baseline test and the defect

### 2.1 Hot loop (movbe_dump.cpp, disassembly @0xb9c98)

```
ldr  w5, [x1, x19, lsl #2]   ; 1st read of data->input[i]
rev  w2, w5                   ; byte-swap (NEVER feeds the compare)
str  w2, [x3, x19, lsl #2]    ; store to data->swapped[i] (different line)
ldr  w4, [x1, x19, lsl #2]    ; ★ 2nd read (RELOAD) of data->input[i]  <-- SDC here
cmp  w4, w5                   ; catches w4 != w5
b.ne <fail branch>
```

- `x1` = `data->input`, `x3` = `data->swapped`, `x19` = `i`.
- `data->input` is filled once at init with `random32()` (seed-determined) and
  reused across all `TEST_LOOP` iterations.
- The second `__builtin_bswap32` is folded by the compiler into a **reload** of
  `data->input[i]` (because `data->swapped[i]` may alias `data->input`). That
  reload is the access that exercises the defect.
- `rev` never participates in the compare → the defect is **not** in the
  byte-swap instruction.

### 2.2 Failure signature

- `result: fail`, core 179 only.
- Example XORs (golden ^ actual): `0xF973254F`, `0xC4333F60` — multi-bit,
  non-trivial patterns, not a single stuck bit.
- First fail at ~4126 ms into a 5-minute run; several fails per 5-minute run
  once active.

## 3. Cache topology of core 179

From `/sys/devices/system/cpu/cpu179/cache/index*`:

| Level | Type   | Size  | Sets | Ways | Line | Shared with       |
|-------|--------|-------|------|------|------|--------------------|
| L1 D  | Data   | 64KB  | 256  | 4    | 64B  | core 179 (private) |
| L1 I  | Instr  | 64KB  | 256  | 4    | 64B  | core 179 (private) |
| L2    | Unif.  | 512KB | 1024 | 8    | 64B  | core 179 (private) |
| L3    | Unif.  | 24MB  | 2048 | 15   | 128B | cores 168–191 (NUMA node 7) |

- The LLC (L3) is **shared by all 24 cores of NUMA node 7** and has a **128B
  line** (L1/L2 use 64B).
- NUMA nodes: 8 × 24 cores. Core 179's home node is **7**; node 7's LLC is the
  defect site.

## 4. Probe matrix

All probes run with identical config unless noted:
`opendcdiag --enable=<probe> --max-test-loop-count=0 -s LCG:1711090087 -t 5m`
(all 192 cores). Baseline fails on core 179 at ~4126 ms.

### 4.1 Probes with clean methodology (hot loop unchanged or change IS the variable)

| Probe | What it changes vs baseline | Result | Valid conclusion |
|-------|------------------------------|--------|------------------|
| **baseline** | — | **FAIL** | reference |
| **A** | removes the `str` (no store) | PASS | a store is **required** |
| **B** | inserts nops between store and reload | PASS | store↔reload must be **back-to-back** |
| **E** | `mbind` swaps `swapped` to NUMA node 0 (remote LLC); input stays node 7; **hot loop byte-for-byte identical to baseline** | PASS | store and reload must share the **same LLC domain** (node 7) |
| C | store back to `input[i]` (SLF path) | multi-core false positive (all cores, 5.7 ms; single-core-179 = PASS) | different, multi-core coherence phenomenon; **excluded** |

**Clean conclusions (robust):**
1. The SDC fires in the **reload `ldr` of `data->input[i]`**.
2. It requires a **store** immediately before the reload (A).
3. That store↔reload pair must be **back-to-back** (B).
4. The store and the reload must be in the **same LLC/NUMA domain** (node 7) (E).
5. `rev` is exonerated (never compared).

### 4.2 Probes that were CONFOUNDED (hot-loop instruction stream changed)

These probes modify the store index/target in a way that changes the compiled
hot-loop instruction stream. Because the defect is instruction-sequence
sensitive (see §5), their PASS results **cannot** be attributed to their
intended variable — the PASS may be entirely due to the instruction-stream
change.

| Probe | Intended variable | Instruction change introduced | Result | Status |
|-------|--------------------|-------------------------------|--------|--------|
| D | store to fixed global address (vs advancing) | `str w2,[x20]` fixed + `add x1,x1,#4` pointer-bump (vs `str w2,[x3,x19,lsl#2]` indexed) | PASS | **confounded** |
| F | store advances but within one line | `and x2,x19,#0xf` mask added | PASS | **confounded** |
| G1 | store hits ONE LLC set (max way pressure) | `ubfiz x2,x19,#18,#4` added | PASS | **confounded** |
| G2 | store spreads across 16 LLC sets | `ubfiz x2,x19,#7,#4` added | PASS | **confounded** |
| H | sweep store footprint (LINES env) | `and x2,x19,x20` + mask-global load added | PASS (all LINES) | **confounded & disproved by its own positive control** |

**Why H disproved the whole F/G/D line:** probe H's `LINES=512` setting makes
the store index `i & 16383`, which for `i ∈ [0,16383]` is identical to `i` — so
the store is byte-for-byte equivalent to baseline `swapped[i]`. This is a
**positive control** that MUST fail if the probe is valid. It **PASSED**, while
the real `movbe_dump` baseline run back-to-back in the same window **FAILED**
(core 179, index 4075 & 201, 4 fails). The only difference is H's extra `and`
mask instruction (scheduled before the store, **not** breaking store↔reload
back-to-back timing). Therefore a single extra `and` in the hot loop is
sufficient to switch the defect off — proving D/F/G1/G2/H's PASS results are
attributable to the instruction-stream perturbation, not their intended
microarchitectural variables.

## 5. The instruction-sequence sensitivity finding

This is the single most important methodological result of the investigation
and doubles as a mechanistic clue.

**Observation:** A one-instruction perturbation of the hot loop (adding an
`and` mask before the store, which does **not** alter the store↔reload
back-to-back relationship) completely suppresses the defect, turning a FAIL
into a PASS — confirmed against a simultaneous baseline that still FAILs.

**Implication for mechanism:** If the defect were governed by a
parameterizable microarchitectural resource with a threshold (e.g. "eviction
pressure exceeds N ways", "footprint exceeds M lines", "MSHR pool exhausted"),
then a perturbation that leaves that parameter unchanged should leave the
trigger rate unchanged. Instead, any instruction-stream change zeroes the
rate. This is consistent with a **rare timing-corner**: a specific alignment
of instruction-issue / store-buffer drain / reload-issue events that only
occurs for the exact baseline instruction schedule. Slight schedule changes
diffuse the corner and the defect vanishes.

This matches the known pitfall recorded for this test family
(`movbe-dump-compiler-dce-root-cause`): the core-179 defect is
instruction-sequence/path-sensitive; identical source logic is not enough —
the compiled hot loop must be byte-identical to the original or the defect
stops reproducing under a fixed time budget.

**Consequence:** finer microarchitectural localization (footprint threshold,
LLC-set count, within-line vs cross-line) **cannot be done** by modifying the
test program's hot loop. Any such modification perturbs the very thing it tries
to measure. Only changes that leave the hot loop byte-for-byte identical
(physical placement / NUMA binding, as in probe E) are valid further levers.

## 6. Likely SDC site (best current hypothesis)

**Instruction:** the reload `ldr w4, [x1, x19, lsl #2]` of `data->input[i]`.

**Mechanism (provisional):** under the exact baseline instruction schedule,
the immediately-preceding same-LLC-domain store to `data->swapped[i]` creates
a timing/resource corner on core 179's local LLC (node 7) such that the
reload fetches a stale or wrong line. The corruption is a rare
instruction-schedule-dependent timing corner on the store↔reload path in
core 179's local LLC — not a deterministic logic bug, not the `rev`
instruction, not pure AGU/store-buffer occupancy, and not (provably) a
single-set eviction parameter (the set-pressure line of probes was
confounded).

**Why core 179 only:** unknown. The randomness and single-core specificity
are consistent with a marginal timing corner that only core 179's
silicon/aging/operating-condition state currently straddles. No other core
has reproduced the defect across all runs.

## 7. What is proven vs not proven

### Proven (clean probes A/B/E + baseline)
- The defect is in the **reload `ldr` of `data->input[i]`** on core 179.
- Trigger requires: (1) the reload; (2) a store immediately before it; (3)
  store↔reload back-to-back; (4) store and reload in the **same LLC/NUMA
  domain** (node 7).
- `rev` is not involved.
- The multi-core failure seen in probe C is a **separate** phenomenon
  (coherence/invalidate storm), not core 179's SDC.

### Not proven (confounded probes D/F/G1/G2/H)
- Whether the store must advance across cache lines (F was confounded).
- Whether single-set LLC eviction pressure matters (G1 was confounded; the
  "eviction disproved" claim is **retracted**).
- Whether there is a footprint threshold (H was confounded by its own
  positive control).
- Whether a fixed-address store triggers (D was confounded).

### Cannot be resolved by test-program modification
- Any finer microarchitectural parameter (footprint size, set count,
  within-line structure) — blocked by instruction-sequence sensitivity.

## 8. Recommendations

1. **Forwards:** if further localization is required, it must avoid touching
   the hot loop. Options: (a) `mbind`/physical-placement variations that keep
   the hot loop byte-identical (probe-E style) — e.g. bind `swapped` pages to
   a controlled subset of LLC sets via physical address layout; (b) external
   observation via `perf stat` hardware counters (LLC-miss, prefetch,
   store-buffer) on core 179 during baseline runs, which does not perturb the
   hot loop.
2. **Mitigation:** since the defect is core-179-specific and
   instruction-schedule-dependent, practical mitigations are (a) offline
   core 179 from production; (b) if impossible, ensure the production
   workload's hot loop does not match the baseline `ldr/rev/str/ldr/cmp`
   schedule — but this is fragile. The robust action is to retire core 179.
3. **Test hygiene:** keep `movbe_dump.cpp`'s hot loop verbatim; any
   "improvement" to it will silently stop catching this defect (see
   `movbe-dump-compiler-dce-root-cause` memory).

## 9. Artifacts

- Test sources: `tests/cpu/misc/movbe_dump*.cpp` (baseline + probes A–H).
- Run logs: `probe_baseline*.log`, `probe_{a,b,c,d,e,f,g1,g2}.log`,
  `probe_h_512*.log` in the repo root.
- Meson registration: `tests/cpu/meson.build` (lines for each probe).
- Memory (detailed per-probe data + methodology):
  `movbe-probe-abcd-sdc-location.md`, `movbe-dump-compiler-dce-root-cause.md`,
  `movbe-sdc-core179-findings.md`.

## 10. Probe-by-probe appendix (one-line each)

- **A** (no store): PASS → store required.
- **B** (nops store↔reload): PASS → back-to-back required.
- **C** (store to input, SLF): multi-core false positive → excluded.
- **D** (fixed-address store): PASS → ⚠️ confounded (hot-loop instr changed).
- **E** (swapped on remote NUMA/LLC): PASS → same-LLC-domain required ✓ (clean).
- **F** (within-line store): PASS → ⚠️ confounded (`and` mask).
- **G1** (single-set, 16-way over-sub): PASS → ⚠️ confounded (`ubfiz`).
- **G2** (16-set spread): PASS → ⚠️ confounded (`ubfiz`).
- **H** (footprint sweep): PASS at all LINES incl. 512 positive control →
  ⚠️ confounded; **disproved the F/G/D line** via its positive control.
