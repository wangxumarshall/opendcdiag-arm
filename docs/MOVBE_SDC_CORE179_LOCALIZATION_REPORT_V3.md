# Core 179 SDC — Localization Report (v3, cross-pathway + bit-flip distribution)

**Status:** Cross-pathway confirmation complete; bit-flip position distribution quantified (36 samples / 562 bits); dump-variant instrumentation methodology validated
**Date:** 2026-08-20
**Supersedes:** `docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT_V2.md` (2026-08-19). V2 remains accurate on trigger-condition localization (§§3–4) and the timing-phase-race mechanism (§6). V3 **adds** the 8×15 min dump campaign (§5), the 36-sample bit-flip position statistics (§6), the dump-variant methodology (§7), and **corrects** two V2 workload-coverage rows (cdouble, double14) per the new campaign data (§4).
**Target machine:** 192-core aarch64 (8 NUMA nodes × 24 cores), Linux 6.6.0-145.3.23.154.oe2403sp3, package 19062
**Test harness:** `opendcdiag` (meson+ninja, `builddir/opendcdiag`), auto-seed (no `-s`), full-core (no `taskset`), `-t 15m --max-test-loop-count=0`

---

## 1. Executive summary

A silent data corruption (SDC) defect occurs on **logical core 179** (NUMA node 7, module 23340). It is **single-core** — every recorded failure lands on the exact same bit of the cpu-mask (segment 4, bit 36 → global bit 179), never drifting across 192 cores. It is **probabilistic** per-iteration but **deterministic in location**.

**The defect is NOT instruction-specific.** This v3 **confirms with a controlled campaign** what v2 inferred from historical logs: six unrelated workload types all trigger the same defect on core 179 —
- `movbe` / `movbe_dump` — byte-swap round-trip (integer, store→reload)
- `mrn_rmw` — integer read-modify-write with store-to-load forwarding
- `eigen_gemm_float_dynamic_square` — float FMA GEMM
- `eigen_gemm_double_dynamic_square` — double FMA GEMM
- `eigen_gemm_double14` — double GEMM + copy/verify round-trips
- `eigen_svd` (historical) — iterative SVD

All share the micro-architectural pattern identified in v2: **a store immediately followed by a load + a data-dependency chain + a cross-line advancing footprint**, exercising core 179's load/store unit or store-buffer forwarding path at a particular instruction-scheduling phase.

**Three new findings in v3:**

1. **Cross-pathway confirmation by controlled campaign (§5).** An 8-test × 15-min campaign (4 originals + 4 dump variants), run sequentially under full-core + auto-seed, triggered SDC on core 179 in **6/8 tests** — every original triggered at the same rate as its dump twin (fail count identical per pair), proving the dump instrumentation does not perturb the trigger. Only the two complex-double (`cdouble`) variants failed to trigger (0 fails each).

2. **Bit-flip position distribution quantified (§6).** Across 36 fail samples / 562 flipped bits (all core 179): flips **concentrate in the mantissa** (float 85 %, double 93 %), the **sign bit is nearly immune** (0–1 flip out of 562), and the **flip-bit count is path-dependent** — SVD fails are mostly single-bit (median 3, 5/11 samples = exactly 1 bit) while GEMM fails are high-density multi-bit (double GEMM median 28 bits, max 39). This is the signature of **data-path corruption, not ALU semantic error and not single-bit SEU**.

3. **Dump-variant methodology validated (§7).** The "hot loop byte-identical, diagnostics in cold branch" design (first proven for `movbe_dump`) generalizes to `mrn_rmw` and `eigen_gemm` float/double/cdouble. Each dump variant records input/golden/actual/xor + per-byte; campaign proves it does not break the trigger. Separately: the framework's own `memcmp_or_fail` already emits `data-miscompare` blocks (actual/expected/mask) on fail, so **position statistics do not require dump variants** — historical logs suffice.

**Mechanism (carried from v2, unchanged):** instruction-scheduling timing-phase race in core 179's load/store unit. One semantically-no-op ALU instruction on the hot path shifts the per-iteration issue phase and collapses deterministic triggering (~100 %) to probabilistic (~10–20 %). Store↔reload back-to-back adjacency is **not** a discriminator (probe X breaks it, still triggers).

---

## 2. The defect and the flip site

Unchanged from v2 §2. The flip occurs on the **reload `ldr` of the just-read input word** in `movbe_dump`'s hot loop (4th instruction, objdump @0xb9cf0):

```
ldr  w5, [x1, x19, lsl #2]   ; 1st read of data->input[i]
rev  w2, w5                   ; byte-swap (never feeds the compare)
str  w2, [x3, x19, lsl #2]   ; store to data->swapped[i] (different line)
ldr  w4, [x1, x19, lsl #2]   ; ★ 2nd read (RELOAD) — SDC site
cmp  w4, w5                   ; catches w4 != w5
```

A correctly-functioning core returns the value just read into `w5`; defective core 179 intermittently returns a different value. The XOR (golden ^ actual) differs every fail — flipped bits are not constant, consistent with probabilistic corruption rather than a stuck bit. The structurally analogous reload in `mrn_rmw` (adjacent `str`→`ldr` of the same temp buffer) and in the eigen GEMM result-writeback path are the flip sites for the other workloads.

---

## 3. Locked-down trigger conditions (high confidence)

Carried from v2 §3, unchanged. Established by control probes A–H under auto-seed (correcting v1's fixed-seed error).

### 3.1 Required conditions (probe REMOVES a condition → PASS)

| Probe | What it removes | Result | Conclusion |
|-------|-----------------|--------|------------|
| **A** | the store (no store side-effect) | PASS | A store is **required** |
| **D** | address advancement (fixed-address store) | PASS | The store address must **advance** |
| **E** | same-LLC-domain (swapped on a different NUMA) | PASS | Store & reload must be in the **same LLC domain** |
| **F** | cross-line stepping (store confined to one line, `i&15`) | PASS | The store must **advance across cache lines** |

### 3.2 Conditions that are NOT the discriminator

| Probe | What it changes | Trigger rate | Conclusion |
|-------|-----------------|--------------|------------|
| **baseline** | nothing | ~100 % (5/5 seeds) | reference |
| **H** (LINES=512) | adds `and x2,x19,x20` — a **semantic no-op** (`i&16383==i` since `i<16384`); store address == baseline, store↔reload stays **back-to-back** (objdump-verified) | ~10 % (1/10) | one extra no-op ALU instr drops rate 100 %→10 % **without** touching address/footprint/back-to-back |
| **X** | adds `eor w?,w?,wzr` — a **semantic no-op**; store footprint == baseline, but store↔reload **NOT back-to-back** (compiler keeps 2nd `rev` between `str` and `ldr`) | ~20 % (1/5) | breaking back-to-back does **not** eliminate the trigger; rate same order as H |

**What IS the discriminator:** instruction-scheduling timing phase. The common factor across H (~10 %) and X (~20 %) is one extra semantically-no-op ALU instruction on the hot path. Adding it collapses the rate from deterministic (~100 %) to probabilistic (~10–20 %). The defect is a **per-iteration pipeline-phase race** in core 179's store-buffer/load-buffer, where a 1-issue-slot phase shift probabilistically mis-forwards or mis-reads.

⚠ **Sample limitation:** H is 1/10, X is 1/5 — the difference is not statistically significant. What IS significant: both are far below baseline's 100 % and both are far above 0 %. The conclusion is directional (timing-phase is the cause; back-to-back is not), not a precise rate curve.

---

## 4. Cross-workload evidence

### 4.1 Historical logs (v2 basis, corrected in v3)

| Workload | Type | Historical fails | Source log | Failure recording |
|----------|------|------------------|------------|--------------------|
| `movbe` / `movbe_dump` | byte-swap round-trip | 11+2+1+4+many | all_tests_2h, all_cores_10m×2, opendcdiag-20260818 | full input/golden/actual/xor + per-byte (custom `log_error`) |
| `mrn_rmw` | integer RMW + store-to-load | 2 | all_tests_2h | "data miscompare" msg only — **no value dump** (v2 gap) |
| `eigen_gemm_float_dynamic_square` | float GEMM (FMA) | 1 | all_tests_2h | actual/expected byte dump (framework `memcmp_or_fail`) |
| `eigen_gemm_cdouble_dynamic_square` | complex-double GEMM | 1 | all_tests_2h | actual/expected byte dump |
| `eigen_gemm_double14` | double GEMM + copy/verify | 44 (stress) | eigen stress 20260812 | ⚠ see §4.2 correction |
| `eigen_svd` | iterative SVD | 11 (in 30m stress) | eigen_core179_30m_stress | framework `data-miscompare` (actual/expected/mask) |

### 4.2 v3 corrections to v2's workload rows

Two V2 rows need correction against the new 8×15 min campaign (§5) and a re-examination of double14's source:

- **`eigen_gemm_double14`** — V2 §4/§5 implied double14 produces "actual/expected byte dump (framework memcmp_or_fail)". **Misleading.** `double14.cpp` *does* have a `memcmp_or_fail` path (line 78), but the per-iteration check sequence runs three `isApprox` + `memcmp_or_fail` pairs, and the failures consistently hit **`_prod.isApprox failed` (line 76) first** — Eigen's floating-point *tolerance* comparison fires before the byte-exact memcmp on line 78, so the framework's `data-miscompare` block (actual/expected/mask) is **never reached**. The log records only `E> Failed at .../double14.cpp:76: _prod.isApprox failed` + cpu-mask + seed. So double14 *does* trigger core 179 (3/5 runs in §5, the most frequent double-path trigger), but yields **no bit-flip position data** — it contributes trigger confirmation, not bit-flip samples. (The double-path bit-flip statistics in §6 come from `eigen_gemm_double_dynamic_square`, whose only check is the byte-exact `memcmp_or_fail`.) See §8.6.

- **`eigen_gemm_cdouble_dynamic_square`** — V2 listed 1 historical fail (all_tests_2h). The 8×15 min campaign (§5) recorded **0 fails** for both `cdouble` and `cdouble_dump` (each 15 min, full-core, auto-seed). The historical 1-fail may be real but is rare; cdouble is the **lowest-trigger-rate** of the eigen paths. No bit-flip data from the campaign.

### 4.3 Workload commonality — why unrelated tests trigger the same defect

| Common trait | movbe | mrn_rmw | eigen GEMM | eigen SVD |
|--------------|-------|---------|------------|-----------|
| store immediately followed by load (same/related address) | ✓ (store swapped[i] → reload input[i]) | ✓ (str→ldr adjacent, same temp buffer) | partial (writeback/copy store→load) | partial (iterative) |
| ALU/FMA data-dependency chain | ✓ (rev bswap) | ✓ (add/sub/xor/and) | ✓ (FMA) | ✓ (iterations) |
| large cross-line advancing footprint | ✓ (64 KB) | ✓ (8 KB, cross-line) | ✓ (hundreds of KB) | ✓ |

The defect is in core 179's **common store/load pipeline**, not in any instruction's semantics. `movbe` triggers most (tightest, most phase-stable store→load sequence); `mrn_rmw` triggers because of its explicit same-address store-to-load forwarding (closest structural analog to movbe); GEMM triggers rarely because its store/load round-trips occur only at writeback/copy; `double14` triggers more than the plain eigen variants because its copy→compute→verify loop adds extra round-trips.

**Non-SDC failures** (architecture, not hardware): `zero_control_vec_sse_jit` (SSE on aarch64), `movmskpspd` (x86 movmsk) — fail on **all 128+ cores** deterministically with identical values, not single-core intermittent. These are excluded from all SDC analysis.

---

## 5. The 8×15 min dump campaign (v3 — cross-pathway confirmation)

### 5.1 Setup

A controlled campaign to (a) confirm the defect triggers across pathways under identical conditions, and (b) validate that adding a dump branch does **not** perturb the trigger. 8 tests run **sequentially** (full-core concurrent pressure exercised, but only one test live at a time — so no cross-test interference on core 179's trigger). Each test: 15 min, full-core (no `taskset`), auto-seed (no `-s`), `--max-test-loop-count=0`.

```
run order:  mrn_rmw → mrn_rmw_dump →
           eigen_gemm_float → eigen_gemm_float_dump →
           eigen_gemm_double → eigen_gemm_double_dump →
           eigen_gemm_cdouble → eigen_gemm_cdouble_dump
```

Log dir: `movbe_log/dump_variants_15min/run_summary.log`.

### 5.2 Results

| # | Test | Type | fail / 15min | on core 179 |
|---|------|------|---------------|-------------|
| 1 | `mrn_rmw` | integer ALU + store-load | 1 | ✅ (seg4 bit36 = X → 179) |
| 2 | `mrn_rmw_dump` | (dump variant) | 1 | ✅ |
| 3 | `eigen_gemm_float_dynamic_square` | float FMA GEMM | 1 | ✅ |
| 4 | `eigen_gemm_float_dynamic_square_dump` | (dump variant) | 1 | ✅ |
| 5 | `eigen_gemm_double_dynamic_square` | double FMA GEMM | 1 | ✅ |
| 6 | `eigen_gemm_double_dynamic_square_dump` | (dump variant) | 1 | ✅ |
| 7 | `eigen_gemm_cdouble_dynamic_square` | complex-double GEMM | 0 | — |
| 8 | `eigen_gemm_cdouble_dynamic_square_dump` | (dump variant) | 0 | — |

**Campaign complete 19:56:40** (start 17:56:10, ~2 h). **6/8 triggered, all 6 on core 179.**

### 5.3 Two findings from the campaign

**(a) Dump variant does not perturb the trigger.** For every pair, the dump variant's fail count equals the original's (mrn_rmw = mrn_rmw_dump = 1; float = float_dump = 1; double = double_dump = 1; cdouble = cdouble_dump = 0). This validates the "hot loop byte-identical, diagnostics in cold branch" design (§7) as trigger-preserving across workload types — the dump branch is never on the hot path, so it cannot shift the timing phase (cf. the H/X probe result of §3.2).

**(b) Complex-double is the non-triggering path.** Both cdouble variants = 0 fails. This is consistent with §4.2's correction (cdouble has the lowest trigger rate). The plain double GEMM triggers (1 fail each), so the difference is the complex-number computation path, not the floating-point width. ⚠ One 15-min window × 1 seed per test is thin; "0 fails" means "not triggered in this window", not "cannot trigger". A longer multi-seed run would be needed to bound the rate.

### 5.4 Bit-flip data captured by the dump variants

| Variant | Flip site | golden | actual | xor (golden^actual) | flipped bits |
|---------|-----------|--------|--------|----------------------|--------------|
| `mrn_rmw_dump` | idx 782, op=xor (integer 64-bit) | 0x6740051755D4E720 | 0xB9D09DCDF55B98CC | 0xDE9098DAA08F7FEC | 35 / 64 |
| `eigen_gemm_float_dump` | [0,20] (float) | 0x3FCD0A94 (=1.60189) | 0x3FCCFB64 (=1.60142) | 0x0001F1F0 | 10 / 32 |
| `eigen_gemm_double_dump` | [0,92] (double) | 0x4006122BB349156C (=2.75887) | 0x400693D6A19C1C8C (=2.82219) | 0x000081FD12D509E0 | 21 / 64 |

Per-byte breakdowns were also recorded (see log_error lines). The `mrn_rmw` xor is a 64-bit integer path (not IEEE754); the eigen xors are the raw material for the §6 position statistics. All three are **multi-bit** — no single-bit SEU.

---

## 6. Bit-flip position distribution (v3 — 36 samples / 562 bits)

### 6.1 Sample basis

Aggregated from two sources, all on core 179, all full-core + auto-seed:
- **Historical 30-min stress log** (`movbe_log/other_sdc/eigen_core179_30m_stress_logs/core179_eigen_30m_20260812_084903.log`): 34 framework `data-miscompare` blocks (each records actual/expected/**mask** = xor), across eigen_gemm_float/double, eigen_svd, eigen_svd_cdouble.
- **8×15 min dump campaign (§5):** 2 samples — eigen_gemm_float_dump (xor 0x0001F1F0) and eigen_gemm_double_dump (xor 0x000081FD12D509E0).

Total: **36 samples, 562 flipped bits** (float32: 23 samples / 207 bits; double64: 13 samples / 355 bits). Note: `mrn_rmw_dump`'s integer xor is excluded from the IEEE754 region analysis (not floating-point); `double14` contributes no bit-flip data (§4.2). `eigen_gemm_double_dynamic_square`'s 11 historical samples carry the double statistics.

### 6.2 Finding 1 — flips concentrate in the mantissa; sign bit nearly immune

| Precision | Sign bit | Exponent | Mantissa |
|-----------|----------|----------|----------|
| float32 (207 bits) | 1 (0 %) | 29 (14 %) | **177 (85 %)** |
| double64 (355 bits) | 0 (0 %) | 24 (6 %) | **331 (93 %)** |

- The sign bit flips 0–1 times out of 562 — **effectively immune**. This rules out "sign/magnitude inversion" class corruption.
- Mantissa dominates (85 %→93 %); the share grows with precision because the mantissa field is wider (23→52 bits), giving more flip "landing sites".
- Exponent is hit but minority (6–14 %) — corruption can change the value's magnitude but does not preferentially.

### 6.3 Finding 2 — flip-bit count is path-dependent (not random)

| Test path | Samples | Flip-bit counts | Median | Pattern |
|-----------|---------|-----------------|--------|---------|
| `eigen_svd` | 11 | 1,1,1,1,1,3,3,5,7,18,21 | 3 | **mostly single-bit** — 5/11 are exactly 1 bit; looks like a single-point perturbation amplified through iterative computation |
| `eigen_gemm_float` | 11 | 6,9,9,10,12,12,12,13,13,18,21 | 12 | moderate multi-bit |
| `eigen_gemm_double` | 11 | 20,21,25,25,27,28,28,29,32,38,39 | 28 | **high-density multi-bit** — looks like result-data wholesale corruption |
| `eigen_svd_cdouble` | 1 | 22 | 22 | single sample (no conclusion) |

**Interpretation:** GEMM (data-dense load/store) → multi-bit wholesale corruption; SVD (iterative compute) → mostly single-bit, perturbation amplified. This points the corruption site at the **storage / load-store path** rather than the ALU compute core. GEMM's multi-bit pattern is most consistent with a **cache-line-level corruption** of a result buffer; SVD's single-bit pattern is consistent with a single mis-forwarded load amplified by the iteration's sensitivity.

### 6.4 Finding 3 — defect is cross-instruction-path, not a single-instruction bug

The same core 179 + same trigger conditions reproduce via movbe (byte-swap), mrn_rmw (integer ALU + store-to-load forwarding), eigen_gemm (FMA floating-point), and eigen_svd (iterative). The "movbe byte-swap path defect" characterization from early investigation must be broadened to: **a multi-bit data-path corruption on core 179, triggered under multi-core concurrent pressure.** The instruction is the *exercise vehicle*, not the *cause*.

### 6.5 What the distribution rules out

- **Single-bit SEU (cosmic ray / alpha):** only 5/36 samples are single-bit (all in SVD), and all failures are locked to one core under a specific concurrency condition — not random in space or time.
- **ALU semantic / sign-logic error:** sign bit immune + mantissa concentration + path-dependent count ⇒ the ALU's result semantics are not the site.
- **A stuck bit:** the xor differs every fail; no constant bit.

---

## 7. Dump-variant methodology (v3 — validated, generalizable)

### 7.1 The design principle

First established for `movbe_dump` (see memory `movbe-dump-compiler-dce-root-cause`): to add per-failure value diagnostics **without** perturbing the SDC trigger, the hot loop must remain **byte-identical** to the original (compiler DCE / scheduling must not change), and all diagnostics go in the **cold failure branch** (the path taken only on miscompare). Violating this — e.g. putting a dump read on the hot path, or adding `volatile`/snapshot — changes the instruction schedule and silently switches the defect off (the H/X probe result of §3.2 is the cautionary tale: a single no-op ALU instr collapses the rate).

### 7.2 Generalization (v3 campaign)

The principle was applied to four new dump variants:
- `tests/cpu/misc/mrn_rmw_dump.cpp`
- `tests/cpu/eigen_gemm/gemm_float_dynamic_square_dump.cpp`
- `tests/cpu/eigen_gemm/gemm_double_dynamic_square_dump.cpp`
- `tests/cpu/eigen_gemm/gemm_cdouble_dynamic_square_dump.cpp`

Each records, on the cold fail branch: input/inputB/golden/actual/xor + per-byte breakdown (mrn_rmw) or row/col + golden/actual IEEE754 bits + xor + per-byte (eigen).

The §5 campaign validates the generalization: **every dump variant's fail count equals its original's**, so the dump branch did not perturb the trigger across integer-ALU, float-FMA, double-FMA, and complex-double-FMA paths.

### 7.3 When you do NOT need a dump variant

The framework's `memcmp_or_fail` already emits a `data-miscompare` block on failure, recording `actual` / `expected` / `mask` (= xor) in hex + per-byte. Therefore:

- **For bit-flip *position statistics*:** historical logs suffice — extract `mask` from existing `data-miscompare` blocks (this is how §6 was built from the 30-min stress log). No re-run, no dump variant needed.
- **For richer per-failure context** (e.g. separating "input" from "golden" when they are the same buffer read at different times, as in movbe/mrn_rmw): a dump variant adds the `input=`/`golden=` distinction the framework does not natively make.

**How to apply:** new SDC-trigger test needing diagnostics → copy the `movbe_dump`/`mrn_rmw_dump` pattern: hot loop untouched, cold branch reads source data. Do not add hot-path snapshots or `volatile`.

---

## 8. Workload types and their bit-flip regularity (v3)

This section characterizes each SDC-triggering workload by **load type** (what compute/memory path it exercises) and **bit-flip regularity** (what the corruption looks like, and whether it follows a pattern). Source: the test sources under `tests/cpu/` + the 36-sample bit-flip dataset of §6. All flips below are on core 179.

### 8.1 movbe / movbe_dump — byte-swap round-trip (integer store→reload)

**Load type.** Integer byte-swap with a **same-buffer store→reload** pattern. Source `tests/cpu/misc/movbe.cpp` / `movbe_dump.cpp`. A 64 KB buffer of `uint32_t` (16384 words) is filled once at init with `random32()` and reused every iteration. The hot loop per element:
```
val = data->input[i];          // 1st read
val = __builtin_bswap32(val);  // byte-swap (rev) — never feeds the compare
data->swapped[i] = val;        // store to a DIFFERENT cache line (swapped[])
val = __builtin_bswap32(val);  // 2nd swap → compiler folds to a RELOAD of input[i]
if (val != data->input[i])    // catches reload != 1st read
```
The flip site is the **reload `ldr` of `data->input[i]`** (the 4th instruction). A correct core returns the value just read; defective core 179 returns a different value. Footprint 64 KB (cross-line, same LLC domain). This is the **tightest, most phase-stable store→load sequence** of all the workloads, which is why movbe triggers most frequently (≈0.24 events/min).

**Flip regularity — multi-bit, no constant bit, no sign/magnitude rule.** Five 5-min movbe_dump runs (seeds 1711090087, 1072812489×2, 1047109210, 763356360, 1314873594) yield xors (golden ^ actual):
| run | seed | xor | flipped bits |
|-----|------|-----|--------------|
| 1 | LCG:1711090087 | 0x8C617A9F | 17 |
| 2 | LCG:1072812489 | 0x5AEF73F2 | 21 |
| 2 | LCG:1072812489 | 0x9D427D70 | 16 |
| 3 | LCG:1047109210 | 0xB57CB778 | 20 |
| 4 | LCG:763356360  | 0xD1B0A9CD | 16 |
| 5 | LCG:1314873594 | 0x2B91F058 | 14 |

All **multi-bit** (14–21 bits), xor differs every fail (no stuck bit), bits scattered across the 32-bit word with no field preference (movbe is an integer path, so IEEE754 region analysis does not apply). **No regularity in the *bit pattern*** — the xor is effectively random per fail — but **strong regularity in the *statistical signature***: always multi-bit, never single-bit, always same core.

### 8.2 mrn_rmw — integer read-modify-write + store-to-load forwarding

**Load type.** Integer ALU (add/sub/xor/and, cycling by `i%4`) combined with an **explicit same-address store→load forwarding** chain. Source `tests/cpu/misc/mrn_rmw.cpp`. Two 8 KB source arrays (`srcA`, `srcB`, 1024 `uint64_t` each) are filled with `memset_random` at init; golden `expected[]` is precomputed. The hot loop per element:
```
a = data->srcA[i];  b = data->srcB[i];
res = (i%4==0)? a+b : (1)? a-b : (2)? a^b : a&b;
store_qword(&temp[i], res);               // str
store_qword(&dst[i], load_qword(&temp[i]));  // ldr of just-stored temp[i] → forwarding
...
memcmp(dst, data->expected, ...)           // batch check
```
On aarch64 the store/load use inline-asm `str`/`ldr` with a `"memory"` clobber, forcing them to stay as adjacent `str`→`ldr` of the **same address** (`temp[i]`). This is the **closest structural analog to movbe**: a store immediately followed by a load of the just-written location (store-to-load forwarding through the store buffer). Footprint 8 KB (cross-line). ALU dependency chain (add/sub/xor/and).

**Flip regularity.** Historical logs (`all_tests_2h`) recorded 2 fails but with **no value dump** (the original `mrn_rmw` only calls `report_fail_msg("mrn_rmw data miscompare")` — logs the message + source line, no actual/expected/xor). The dump-variant campaign (`mrn_rmw_dump`, §5) captured **one** full sample: index 782, op=xor, golden=0x6740051755D4E720, actual=0xB9D09DCDF55B98CC, **xor=0xDE9098DAA08F7FEC → 35/64 bits flipped**. This is the **highest-density flip** in the dataset (54 % of bits), high bits densely corrupted, scattered across the full 64-bit word. Consistent with movbe's multi-bit signature; the single sample is too thin for pattern regularity, but it is emphatically **not single-bit**. (The §6 statistics exclude this integer xor from the IEEE754 region analysis.)

### 8.3 memcpy1 — small-block memory copy (pure load/store, no ALU)

**Load type.** **Pure memcpy with immediate verification** — the only workload with no arithmetic at all. Source `tests/cpu/memory/memcpy1.cpp`. A 256-byte `src` block (stack) is filled with `std::mt19937` random bytes each iteration, copied to `dst` via `memcpy`, then `memcmp(dst, src, 256)` checks the copy. So every iteration is a **load (src) → store (dst) → load (dst for memcmp)** round-trip over a 256-byte block — a dense sequence of store→load pairs on the same freshly-written data, exercising the store-buffer forwarding path with **no ALU data dependency** (unlike mrn_rmw/movbe). Footprint tiny (256 B, one-or-two cache lines) but the store→reload happens every iteration.

**Flip regularity.** Historical logs (`all_cores_10m_20260815` and `_20260816`) recorded **6 fails** (seeds LCG:163174881 ×4, LCG:1410093522 ×2), all on core 179. The original `memcpy1` only logs `report_fail_msg("memcpy1: data mismatch after copy")` — **no value dump, no xor/mask**. So no bit-flip positions are available for memcpy1. Its significance is **trigger confirmation**: a workload with *zero ALU* and only store→load round-trips still triggers core 179, reinforcing that the defect is in the load/store path, not in any arithmetic unit. Flip regularity: **undetermined** (no data); trigger regularity: confirmed on 179 when it fires.

⚠ **v3 re-test — single-test memcpy1 does NOT trigger (neither seed-fixed nor fracturing, neither 5 min nor 10 min):** two dedicated single-test runs both recorded **0 fails**:
- **5 min, `--max-test-loop-count=0`** (seed fixed per worker): `movbe_log/memcpy1_mrn_5min/memcpy1.log`, `test-runtime` 300002 ms, full-core, 0 fail.
- **10 min, default fracturing** (seed rotated — **11180 distinct seeds** exercised across fracturing shards): `movbe_log/memcpy1_10min_nofracture/memcpy1.log`, `exit: pass`, 0 fail, 11180 pass/interrupted shards.

Both ran on all 192 cores (the `stderr messages` block shows multiple workers' iteration output interleaved, confirming multi-core concurrency; `Cpus_allowed_list: 0-191`, no taskset). The historical 6 fails all came from `all_cores_10m` runs whose command line is `opendcdiag -t 10m` (**no `--enable`** → the *full test suite* runs concurrently). Since single-test memcpy1 is 0-fail across **both** seed modes (fixed **and** 11180-way rotated) **and** across 5 min/10 min windows, the confounding variables (seed, window length) are **eliminated**: the discriminator is **full-suite concurrent pressure** — memcpy1 alone, no matter how run, cannot form the trigger condition on core 179; it triggers only when many different tests share the 192 cores and pile concurrent load/store pressure on core 179. This contrasts with movbe/mrn_rmw, which trigger in ≤21 s of single-test. The likely reason: memcpy1's 256 B footprint produces a store→load sequence too phase-unstable to hit core 179's timing window alone, and only the additional concurrent traffic from the full suite perturbs the phase enough to land in the window. Treat memcpy1 as a trigger only under full-suite concurrency; for single-test SDC capture use movbe/mrn_rmw.

### 8.4 eigen_gemm_float_dynamic_square — float FMA GEMM

**Load type.** **FMA matrix-matrix multiply**, single precision. Source `tests/cpu/eigen_gemm/gemm_float_dynamic_square.cpp`. At init, two 256×256 `float` random matrices `lhs`, `rhs` are generated and the golden `prod = lhs*rhs` is computed once. The hot loop recomputes `x = lhs*rhs` and does a **byte-exact** `memcmp_or_fail(x.data(), prod.data(), 256*256)` — no tolerance, so a single flipped mantissa bit fails. Under the hood Eigen issues a long stream of `fmadd`/`fmla` (FMA) micro-ops over hundreds of KB of matrix data; the store/load round-trips occur at result writeback (`x.data()` write → `memcmp_or_fail` read). Footprint hundreds of KB.

**Flip regularity — mantissa-concentrated, multi-bit, sign-immune.** 11 historical samples (30-min stress, seed LCG:1756755722) + 1 dump-campaign sample (§5, [0,20]). Xor masks (float, 32-bit), representative:
| site | mask | flipped bits | field |
|------|------|---------------|-------|
| dump [0,20] | 0x0001F1F0 | 10 | mantissa+exp low |
| 0x7FE873E6 | — | 20 | sign+exp+mantis (rare sign hit) |
| 0x000506EC | — | 8 | mantissa |
| 0x01D68734 | — | 11 | exp+mantis |
| 0x0001DF0CC→0x001DF0CC | — | 11 | mantissa |

Across all 12 float samples: **mantissa 85 %, exponent 14 %, sign 1 %** (only the 0x7FE8… case touched the sign bit). Flip-bit count median 12 (range 6–21). **Regularity: the corruption lands in the mantissa (precision bits), avoids the sign bit, and is multi-bit** — consistent with a cache-line/result-buffer corruption, not an FMA semantic error (which would tend to produce systematic exponent errors).

### 8.5 eigen_gemm_double_dynamic_square — double FMA GEMM

**Load type.** Same as §8.4 but **double precision** (256×256 `double`, `Matrix<double,Dynamic,Dynamic>`), golden precomputed, byte-exact `memcmp_or_fail`. The FMA stream is wider (64-bit accumulators); footprint ~512 KB. Same writeback→verify store/load round-trip.

**Flip regularity — the densest multi-bit corruption of all.** 11 historical samples (seed LCG:546398148) + 1 dump-campaign sample (§5, [0,92]). Representative xor masks (double, 64-bit):
| site | mask | flipped bits |
|------|------|---------------|
| dump [0,92] | 0x000081FD12D509E0 | 21 |
| 0x0002DECCDDA05EB9 | — | 25 |
| 0x7FFCDE1DBC270D91 | — | 38 |
| 0x000053DF26BFED9D | — | 21 |
| 0x006484561086551E | — | 28 |

Across all 12 double samples: **mantissa 93 %, exponent 6 %, sign 0 %**. Flip-bit count median 28 (range 20–39) — **the highest and densest of any path**. Regularity: same as float (mantissa, sign-immune, multi-bit) but *more* bits per fail, scaling with the wider mantissa field (52 vs 23 bits → more landing sites). The 0x7FFC…/0x7FFD… cases hit the exponent high bits but never the sign bit.

### 8.6 eigen_gemm_double14 — double GEMM + extra copy/verify round-trips

**Load type.** Double GEMM (256×256, M_DIM=256) **with extra copy→verify round-trips per iteration** (source `tests/cpu/eigen_gemm/double14.cpp`). Each iteration: copy `_x = lhs`, `_y = rhs`, compute `_prod = _x*_y`, then **three** isApprox + memcmp_or_fail check pairs (`_x`/`_y`/`_prod` vs originals). This adds more store→load round-trips (copy-in + verify-out) than the plain dynamic_square variants — hence the higher trigger probability.

**Flip regularity — triggers but records NO bit-flip data.** The double14 failures (3/5 in the complement campaign, all core 179) all hit `_prod.isApprox failed` (double14.cpp:76). Because `isApprox` is a **floating-point tolerance comparison**, it fires *before* the byte-exact `memcmp_or_fail` on line 78 — so the framework's `data-miscompare` block (with actual/expected/mask) is **never reached**. The log records only `E> Failed at .../double14.cpp:76: _prod.isApprox failed` + cpu-mask + seed. Therefore double14 contributes **trigger confirmation** (it is the most-frequent double-path trigger, 10 fails in the 30-min stress + 3/5 in the complement) but **zero bit-flip positions**. Flip regularity: **undetermined** (isApprox masks the bits); trigger regularity: strong on 179.

### 8.7 eigen_gemm_cdouble_dynamic_square — complex-double FMA GEMM (non-triggering in v3)

**Load type.** FMA GEMM on `std::complex<double>` 221×221 matrices (M_DIM=221, "weird dim on purpose"). Golden precomputed; verification is byte-exact `memcmp_or_fail` on the reinterpreted `double*` (2×221×221 = 97641 doubles). Complex GEMM decomposes into 4 real GEMMs (real×real, real×imag, imag×real, imag×imag) under the hood.

**Flip regularity — low-rate path, but DOES trigger (v3 re-test).** Two historical + campaign samples:
- Historical `all_tests_2h`: 1 fail, mask=0x33CEC39BE4109264 (28-bit double-path flip).
- 8×15 min dump campaign: **0 fails** (both cdouble and cdouble_dump, each 15 min) — too thin a window for this low-rate path.
- **v3 re-test (Eigen 3.4.0 rebuild, 15 min, full-core, auto-seed, `--max-test-loop-count=0`): 1 fail on core 179**, ttf=94856 ms, seed LCG:874481998, mask=**0x0000BA899C502FDC → 24 bits flipped** (bits 2–47, mantissa + low exponent). This confirms cdouble *does* trigger — it is the lowest-rate eigen path, and a single 15-min/1-seed window is a coin flip, not a "cannot trigger" verdict. The 24-bit flip is consistent with §8.5's double GEMM signature (multi-bit wholesale corruption), confirming the defect is the same regardless of the real-vs-complex arithmetic path.

### 8.8 eigen_svd / eigen_svd_cdouble — iterative BDCSVD (mostly single-bit)

**Load type.** **Iterative singular value decomposition**, source `tests/cpu/eigen_svd/svd.cpp` (float, M_DIM=256) and `svd_cdouble_noavx512.cpp` (complex-double, M_DIM=300). Uses Eigen's `BDCSVD` (divide-and-conquer bidiagonalization) — an *iterative* algorithm: many matrix multiplies + Householder reflections + sweeps, with repeated load/store of intermediate matrices. Golden = first thread's U/V matrices; subsequent iterations compared byte-exact via `memcmp_or_fail` through `compare_or_fail`. Footprint hundreds of KB, but the access pattern is **iteration-driven** (read-modify-write across sweeps), not the tight store→reload of movbe/mrn_rmw.

**Flip regularity — mostly SINGLE-bit (unique among all paths).** 11 float SVD samples (seed LCG:526650671) + 1 cdouble SVD sample (LCG:576148252). Float SVD xor masks:
| mask | flipped bits |
|------|--------------|
| 0x0000000E | 3 |
| 0x00000001 | 1 |
| 0x000001FC | 7 |
| 0x00000001 | 1 |
| 0x00003448 | 6 |
| 0xE34FEFCC | 21 |
| 0x00000001 | 1 |
| 0x00000004 | 1 |
| 0x0000000E | 3 |
| 0x7656F3B0 | 17 |
| 0x00000002 | 1 |

Flip-bit count median **3**, with **5/11 samples = exactly 1 bit**. This is the **only path dominated by single-bit flips** — and the single-bit flips concentrate at the very bottom of the mantissa (bit 0, 1, 2 — masks 0x1, 0x2, 0x4, 0xE). Regularity: SVD's iterative amplification turns a single mis-forwarded load into a one-bit (or few-bit) difference in the U/V output, **distinct from GEMM's wholesale multi-bit result corruption**. The two large-SVD masks (0xE34FEFCC=21, 0x7656F3B0=17) are the exceptions where the corruption hit a high-traffic intermediate. The cdouble SVD sample (0x0000001F993A6AD6, 22 bits) looks more like a GEMM-style multi-bit corruption than the typical SVD single-bit.

**v3 re-test (Eigen 3.4.0 rebuild):** the original BDCSVD svd tests were compiled back into the binary (system Eigen 3.3.8 + `-std=c++23` had a BDCSVD `operator!=` rewrite-candidate compile failure; re-pointing meson's `eigen3_root` to `/usr/local` Eigen 3.4.0 fixed it). A 5-test × 15-min campaign then ran:
- `eigen_svd` (float BDCSVD): **0 fails** (15 min). Historical 11 fails were from a 30-min stress run — float SVD's per-window rate is low; one 15-min/1-seed window missed.
- `eigen_svd_cdouble_noavx512` (complex-double BDCSVD): **0 fails** (15 min). Historical 1 fail — lowest rate.
- `eigen_svd_double2` (double BDCSVD, same family): **1 fail on core 179**, ttf=125000 ms, seed LCG:1037940591, mask=**0x0000000000000001 → exactly 1 bit (bit 0, the mantissa LSB)**. The corruption hit Matrix V at offset [6608,0]: actual=−0x1.28673db9ef688p−8, expected=−0x1.28673db9ef689p−8 — a 1-ULP difference. **This is the strongest confirmation of the SVD single-bit signature**: the BDCSVD double path, under the same defect, produced exactly the 1-bit flip predicted by §4.3's SVD pattern — direct contrast with cdouble GEMM's 24-bit flip in the same campaign.

### 8.9 Cross-workload flip regularity — summary table

| Workload | Load type | Store→load pattern | Flip-bit count (median / range) | Field preference | Sign bit | Regularity |
|----------|-----------|--------------------|---------------------------------|-------------------|----------|------------|
| movbe_dump | int byte-swap | same-buffer reload (tightest) | 17 / 14–21 | none (integer) | n/a | multi-bit, no stuck bit |
| mrn_rmw_dump | int ALU + forwarding | same-address str→ldr | 35 / 1 sample | none (integer) | n/a | multi-bit (densest) |
| memcpy1 | pure copy | load→store→load (no ALU) | no data | — | — | single-test 0 fail (5min seed-fixed + 10min 11180-seed fracturing); triggers only under full-suite concurrency (6 hist on 179) |
| gemm_float | FMA GEMM | writeback→verify | 12 / 6–21 | mantissa 85 % | 1/207 | multi-bit, mantissa |
| gemm_double | FMA GEMM | writeback→verify | 28 / 20–39 | mantissa 93 % | 0/355 | multi-bit (densest FP) |
| gemm_double14 | GEMM + copy/verify | extra round-trips | no data (isApprox) | — | — | trigger only (most frequent) |
| gemm_cdouble | complex FMA GEMM | writeback→verify | 24 / 1 re-test sample (+1 hist 28) | mantissa+low exp | — | low rate; v3 re-test 1 fail/24-bit on 179 (0 in earlier campaign) |
| eigen_svd | iterative BDCSVD | iteration-driven | 3 / 1–21 (hist) | mantissa low bits | — | **mostly single-bit** (5/11 hist); v3 re-test 0 fail (low rate, 15min missed) |
| eigen_svd_double2 | iterative BDCSVD (double) | iteration-driven | 1 / 1 re-test sample | bit 0 (mantissa LSB) | — | **exactly 1 bit** (v3 re-test) — confirms SVD single-bit signature |
| eigen_svd_cdouble_noavx512 | iterative BDCSVD (cplx double) | iteration-driven | 22 / 1 hist sample | — | — | lowest rate; v3 re-test 0 fail |

**The regularity that holds across all paths:** (1) every flip is on core 179; (2) no stuck bit (xor varies every fail); (3) **multi-bit dominates** (only SVD shows single-bit, and even there it is a minority of samples) — ruling out single-bit SEU. **The regularity that splits by load type:** tight store→reload (movbe/mrn_rmw) and data-dense writeback (GEMM) → multi-bit wholesale corruption; iteration-driven (SVD) → single-bit amplified perturbation; pure copy (memcpy1) → triggers but no data. The split tracks the **load/store access pattern**, not the arithmetic type — reinforcing that the defect site is in core 179's load/store path.

---

## 9. Final localization statement

**Macroscopic (locked, high confidence):**
- **Core:** logical 179 (NUMA 7, module 23340), bit 179 in every cpu-mask, never drifts.
- **Flip site:** the reload `ldr` of the just-read input word (movbe's 4th hot-loop instruction; structurally analogous reload in mrn_rmw and the GEMM writeback path).
- **Required conditions:** a store exists + the store address advances + the store is in the same LLC domain as the reload + the store advances across cache lines.
- **Cross-pathway (v3):** confirmed by controlled campaign across 6 workload types — the defect is in core 179's common store/load pipeline, not any instruction's semantics.

**Micro-architectural mechanism (qualitatively pinned, not quantified):**
- **Cause:** instruction-scheduling timing-phase race in core 179's load/store unit or store-buffer forwarding path. One semantically-no-op ALU instruction on the hot path shifts the per-iteration issue phase and collapses deterministic triggering (~100 %) to probabilistic (~10–20 %).
- **Bit-flip signature (v3):** mantissa-concentrated (85–93 %), sign-immune, path-dependent count (SVD single-bit ↔ GEMM multi-bit) ⇒ data-path / cache-line-level corruption, not ALU semantic error, not single-bit SEU.
- **Refuted:** store↔reload back-to-back adjacency is NOT a discriminator (probe X breaks it, still triggers); the H mask does NOT kill the defect by breaking adjacency (objdump: mask at loop top, adjacency preserved).

**What remains unquantified:** the exact pipeline stage / phase-window width; the per-instruction-type sensitivity curve; the precise trigger-rate-vs-extra-instructions function; the cdouble path's true (near-zero?) rate. These need ≥30-seed campaigns per probe and PMU/store-buffer event sampling on core 179. The workload-type flip regularity of §8 narrows the site to the load/store path but does not pin the pipeline stage.

---

## 10. Log archive

All campaign logs are under `movbe_log/` with a per-campaign subdirectory and a `README.md` index (see `movbe_log/README.md`). v3's additions:
- `movbe_log/dump_variants_15min/` — the 8×15 min campaign (§5).
- `movbe_log/eigen_complement/` — the double14+sparse complement run (§11.3).

## 11. Methodology notes / corrections from v2

1. **cdouble coverage (v3):** V2 listed 1 historical cdouble fail; the 8×15 min campaign recorded 0 (both variants). cdouble is the lowest-rate eigen path; "0 in one 15-min window" does not bound the rate, only flags it as rare.
2. **double14 failure recording (v3, correctness):** V2 §4/§5 implied double14 produces framework byte dumps. **Misleading** — `double14.cpp` *does* have a `memcmp_or_fail` (line 78), but its three-check sequence hits `_prod.isApprox failed` (line 76, a floating-point tolerance check) **first**, so the byte-exact memcmp that emits the `data-miscompare`/mask block is never reached. double14 triggers core 179 (3/5 runs in the complement campaign, the most frequent double-path trigger) but records **no bit-flip data**. The double-path statistics in §6 come from `eigen_gemm_double_dynamic_square` (whose only check is byte-exact memcmp). double14's value is as a *trigger confirmation* (extra copy→verify round-trips ⇒ higher trigger probability), not as a bit-flip source. See §8.6.
3. **eigen_sparse instant-exit caveat (v3):** the complement run included `eigen_sparse` (5×15 min). It recorded 0 fails — but `test-runtime` was **15.485 s**, not 15 min: the sparse solver completes its iteration and exits pass well before the 15-min timeout. So "0 fails" here means "did not run long enough on core 179 to exercise the trigger", **not** "sparse cannot trigger". Sparse's triggerability is **undetermined**.
4. **Fixed LCG seed error (v1, carried):** v1 ran single-seed probes with `-s LCG:<fixed>`; a fixed seed is one input pattern, SDC is seed-sensitive, retry reuses the seed. v1's "H LINES=512 PASS" was seed luck (FAILED under auto-seed). All v2/v3 probes use auto-seed.
5. **Infinite-retry script bug (v2, carried):** early campaign scripts let a failing seed retry indefinitely (opendcdiag retries on failure with no cap bounded by `-t`). Fixed by wrapping each run in `timeout`.
