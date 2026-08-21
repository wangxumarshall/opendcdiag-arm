/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_x
 * @parblock
 * SDC-trigger probe, variant X: **minimal no-op ALU instruction** to isolate
 * "instruction-schedule timing perturbation" from "store footprint" and from
 * "back-to-back store<->reload".
 *
 * CONTEXT (from objdump of baseline + probes H/F/G1/G2, all 5 verified to keep
 * store<->reload back-to-back with 0 instructions between str and the reload
 * ldr):
 *   - baseline: swapped[i]                 (footprint = 64KB, 512 lines) → ~100%
 *   - H  LINES=512: swapped[i & 16383]    (footprint == baseline, mask is a
 *                                          SEMANTIC no-op since i<16384) → ~10%
 *   - F: swapped[i & 15]                   (footprint = 1 line)          → ~0%
 *   - G1/G2: ubfiz-indexed small footprint                                → ~0%
 * The tension: H proves that ONE extra no-op ALU instruction (`and x2,x19,x20`,
 * which computes i&16383 == i) drops the rate 100%→10% even though the store
 * address is byte-identical to baseline. But F/G1/G2 also have one extra ALU
 * instruction AND a shrunk footprint, so their 0% cannot separate "footprint
 * too small" from "ALU instruction suppresses timing".
 *
 * PROBE X resolves this: it is byte-for-byte identical to baseline EXCEPT it
 * inserts ONE extra ALU instruction on the hot path that is a SEMANTIC no-op
 * but carries a real register dependency (mirroring H's `and x2,x19,x20`,
 * which reads x19). The chosen instruction is `val = val ^ 0u` after the first
 * bswap: the compiler lowers this to one `eor` on the value register (a real
 * instruction, not DCE-able since the result is observed by the store), but it
 * does not change val, does not touch the address/index, and does not force
 * any pointer reload. The store stays `swapped[i]` (footprint == baseline ==
 * 64KB). The store<->reload stays back-to-back (the eor is BEFORE the store,
 * not between store and reload — mirroring where H's `and` sits). The ONLY
 * change vs baseline is one extra issue slot per iteration carrying a value
 * dependency.
 *
 * This is done in PURE C (no inline asm). An earlier version used
 * `asm volatile("nop")` / `asm volatile("mov x2,x2")`, but the compiler
 * treated the asm as a memory fence and reloaded the input/swapped pointers
 * inside the loop (`ldp x1,x2,[x20]` every iteration), turning the probe into
 * "no-op + extra memory read" and invalidating it. A pure-C `val ^ 0u` is a
 * normal expression the compiler schedules normally, so it does NOT perturb
 * pointer-lifetime analysis and the loop keeps baseline's structure.
 *
 * Decision tree:
 *   - FAIL (rate > 0) → one extra ALU instruction with a value dependency is
 *     enough to perturb the defect → the mechanism is a per-iteration
 *     instruction-schedule / pipeline-phase race, NOT footprint, NOT
 *     back-to-back. This would mean H's 10% drop is a timing effect, and
 *     F/G1/G2's 0% could be partly timing-suppression too.
 *   - PASS (rate == 0) → a dependency-bearing no-op on the value (not the
 *     index) does NOT perturb → H's drop comes specifically from the `and`
 *     reading the loop INDEX x19 (address-generation dependency), narrowing
 *     the mechanism to the store-address path.
 *
 * objdump MUST be checked after build to confirm: (a) one `eor` (or similar)
 * is present in the loop before the store, (b) store stays
 * `str [.., x19, lsl #2]` == baseline, (c) store<->reload stay back-to-back,
 * (d) NO extra pointer-reload `ldp` appears in the loop.
 * @endparblock
 */

#include "sandstone.h"

#define MOVBE_BUFFER_SIZE (1 << 14)

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_init(struct test *test)
{
    movbe_data *data = (movbe_data *)malloc(sizeof(movbe_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
    data->swapped = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));

    for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movbe_run(struct test *test, int cpu)
{
    movbe_data *data = (movbe_data *)test->data;

    TEST_LOOP(test, 1 << 13) {
        // Byte-for-byte identical to baseline movbe_dump EXCEPT one inline
        // no-op inserted before the store (mirroring where H places its `and`).
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // PROBE X: one extra ALU instruction that is a SEMANTIC no-op but
            // carries a real register dependency on val and survives DCE.
            // Uses `asm("eor %w0,%w0,wzr" : "=r"(val) : "0"(val))`: the eor
            // with wzr (zero register) leaves val unchanged, but because val
            // flows through an asm node as output and is observed by the store
            // below, the compiler cannot DCE it. Critically this uses NO
            // `volatile` and NO `memory` clobber, so the compiler does not
            // treat it as a memory fence and keeps the input/swapped pointers
            // loaded once outside the loop (a `volatile`/`memory` asm made the
            // compiler reload `ldp x1,x2,[x20]` every iteration, invalidating
            // an earlier probe X version). This is the ONLY deviation from
            // baseline — store stays swapped[i], footprint stays 64KB,
            // store<->reload stays back-to-back. objdump MUST confirm: (a)
            // exactly one `eor` in the loop before the store, (b) store stays
            // `str [.., x19, lsl #2]` == baseline, (c) NO `ldp` pointer reload
            // in the loop, (d) store<->reload back-to-back.
            asm("eor %w0, %w0, wzr" : "=r"(val) : "0"(val));

            data->swapped[i] = val;

            val = __builtin_bswap32(val);

            if (val != data->input[i]) {
                uint32_t inputv = data->input[i];
                uint32_t golden = data->input[i];
                uint32_t actual = val;
                uint32_t xorv = golden ^ actual;
                unsigned char *ib = (unsigned char *)&inputv;
                unsigned char *gb = (unsigned char *)&golden;
                unsigned char *ab = (unsigned char *)&actual;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("MovBE-probeX: Round-trip failed at index %u: "
                          "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X | "
                          "bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
                          "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
                          "xor=%02X%02X%02X%02X",
                          (unsigned)i, inputv, golden, actual, xorv,
                          ib[3], ib[2], ib[1], ib[0],
                          gb[3], gb[2], gb[1], gb[0],
                          ab[3], ab[2], ab[1], ab[0],
                          xb[3], xb[2], xb[1], xb[0]);
                report_fail_msg("MovBE: Round-trip failed at index %u", (unsigned)i);
            }
        }
    }

    return EXIT_SUCCESS;
}

static int movbe_cleanup(struct test *test)
{
    movbe_data *data = (movbe_data *)test->data;
    if (data) {
        free(data->input);
        free(data->swapped);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movbe_dump_probe_x, "movbe_dump probe X: minimal no-op ALU instruction before store (isolates timing-phase perturbation from footprint and back-to-back)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
