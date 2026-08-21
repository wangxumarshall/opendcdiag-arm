/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_d
 * @parblock
 * SDC-trigger probe, variant D: **store to a compile-time-constant global
 * address** (decouples store-LFU-occupancy from any cache-line relationship
 * with the input buffer).
 *
 * The baseline movbe_dump hot loop is:
 *   ldr input[i] -> rev -> str swapped[i] -> ldr input[i](reload) -> cmp
 * Probes A (no store) and B (nops after store) both PASS, proving the store
 * and its tight timing to the reload are trigger conditions. But in the
 * baseline the store writes to `swapped[i]`, a different cache line from
 * input[i] -- so the store does NOT dirty the input line. The open question
 * is whether the trigger is:
 *   (a) the store's LSU / store-buffer *occupancy/timing* (address-
 *       independent), or
 *   (b) some *cache-line* effect of the store (address-dependent).
 *
 * Probe D isolates (a): the store target is a single compile-time-constant
 * global `probe_d_sink` whose address has no relationship to data->input.
 * This keeps the store instruction and its LSU/store-buffer timing
 * identical to the baseline, while removing any cache-line coupling between
 * the store and the reloaded input address. The reload of data->input[i]
 * still happens (forced via volatile), so if the defect is in the reload's
 * LSU-contention window, probe D should STILL trigger like the baseline.
 * If probe D passes, the store's address/cache-line matters and the
 * "contention window" theory is wrong.
 *
 * NOTE: every core writes the same global sink, so there is cross-core
 * coherence traffic on that one line. That is the *point* here -- it keeps
 * store-buffer/LSU activity present and address-decoupled from input. The
 * expected healthy behavior is still reload == 1st-read on every core.
 * @endparblock
 */

#include "sandstone.h"

#define MOVBE_BUFFER_SIZE (1 << 14)

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

// Compile-time-constant store target: decoupled from data->input cache line.
// volatile so the store is never dead-code-eliminated.
static volatile uint32_t probe_d_sink = 0;

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
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            // PROBE D: store to a compile-time-constant global (not input,
            // not swapped[i]). Keeps the store's LSU/store-buffer timing
            // identical to the baseline but removes cache-line coupling to
            // the reloaded input address. The reload of data->input[i] is
            // forced via volatile so the defect-triggering reload is
            // preserved exactly.
            volatile uint32_t *vinput = &data->input[i];
            uint32_t val = *vinput;
            val = __builtin_bswap32(val);

            probe_d_sink = val;          // store to constant global address

            val = __builtin_bswap32(val);

            uint32_t input_now = *vinput;
            if (val != input_now) {
                uint32_t inputv = *vinput;
                uint32_t golden = val;
                uint32_t actual = input_now;
                uint32_t xorv = golden ^ actual;
                unsigned char *ib = (unsigned char *)&inputv;
                unsigned char *gb = (unsigned char *)&golden;
                unsigned char *ab = (unsigned char *)&actual;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("MovBE-probeD: Round-trip failed at index %u: "
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

DECLARE_TEST(movbe_dump_probe_d, "movbe_dump probe D: store to constant global (LSU timing kept, cache-line coupling removed)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
