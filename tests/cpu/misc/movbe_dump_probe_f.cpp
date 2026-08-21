/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_f
 * @parblock
 * SDC-trigger probe, variant F: **store address advances but NEVER crosses a
 * cache line** (isolates "cross-line stepping" from "advance per se", holding
 * the same-LLC-domain condition constant).
 *
 * Baseline movbe_dump hot loop:
 *   ldr input[i] -> rev -> str swapped[i] -> ldr input[i](reload) -> cmp
 * Probe E proved the store must stay in the SAME LLC domain as the reload
 * (swapped on a remote NUMA node -> PASS). Probe D proved a FIXED-address
 * store -> PASS, but probe D's store target was a static global whose
 * physical NUMA node was uncontrolled (likely off-domain), so D confounds
 * "fixed address" with "off-domain". The open question: does the store need
 * to advance ACROSS cache lines, or is any advancing address enough?
 *
 * Probe F keeps the baseline structure byte-for-byte (same allocation via
 * aligned_alloc = first-touch, so core 179's input AND swapped land on node 7,
 * the SAME LLC domain as baseline; same reload of input[i]) and changes
 * EXACTLY ONE source line: the store target goes from `swapped[i]` to
 * `swapped[i & 15]`. Now the store address still advances every iteration
 * (0,4,8,...,60,0,4,... -- 16 distinct word addresses) but is confined to the
 * FIRST 64-byte cache line of swapped. It NEVER crosses into a new line. The
 * reload `data->input[i]` is untouched (full i, advancing across all 16384
 * words = 1024 input lines, exactly as baseline), and is the SDC site.
 *
 * Decision tree:
 *   - PASS -> crossing cache lines is REQUIRED (within-line advance is
 *     insufficient). The advancing store must step across distinct lines /
 *     distinct LLC sets. Strongly supports an LLC-set / store-buffer eviction
 *     interaction as the mechanism (one line = one/few LLC sets = no
 *     cross-set eviction pressure).
 *   - FAIL -> within-line advance is sufficient; cross-line is NOT required.
 *     Points to AGU / store-buffer-replay activity rather than LLC-set
 *     eviction.
 *
 * NOTE: `swapped[i & 15]` forces the compiler to keep the reload of
 * `data->input[i]` (it cannot prove swapped[0..15] does not alias input[i],
 * since both are opaque uint32_t* from the same struct). Verified by objdump
 * that the reload remains `ldr w4, [x1, x19, lsl #2]` and that store<->reload
 * are still back-to-back (the `and` mask schedules before the store, not
 * between store and reload).
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
        // Hot loop is byte-for-byte identical to baseline movbe_dump EXCEPT
        // the store index: `swapped[i & 15]` confines the store to ONE cache
        // line (bytes 0..60 of swapped). The store address still advances
        // every iteration (16 distinct word slots) but never crosses a line.
        // The reload `data->input[i]` below uses the full `i` and is the
        // SDC site -- kept identical to baseline.
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // PROBE F: store within one cache line (i & 15), not swapped[i].
            // Address advances 0->4->...->60->0->... but stays in line 0.
            data->swapped[i & 15] = val;

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
                log_error("MovBE-probeF: Round-trip failed at index %u: "
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

DECLARE_TEST(movbe_dump_probe_f, "movbe_dump probe F: store advances within ONE cache line (cross-line stepping isolated; same-LLC-domain held constant)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
