/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_a
 * @parblock
 * SDC-trigger probe, variant A: **no store**.
 *
 * Identical to movbe_dump.cpp except the `data->swapped[i] = val` store is
 * removed. The store does not participate in the comparison, but its
 * presence may affect the defect by:
 *   - putting the input cache line into a different state (if the compiler
 *     ever speculated aliasing), or
 *   - occupying the store buffer / LSU so the follow-up reload (the
 *     defect-triggering ldr) is delayed/reshuffled.
 *
 * Compare this variant's trigger rate + XOR pattern against the baseline
 * movbe_dump to decide whether the store side-effect is part of the
 * trigger mechanism.
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
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            // PROBE A: no store. Use a volatile read for the comparison so
            // the compiler cannot prove val == data->input[i] and delete the
            // whole loop body (it would otherwise: with no store there is no
            // aliasing excuse, so bswap(bswap(x)) == x folds the compare to
            // constant-false and the loop vanishes). The volatile reload is
            // the exact same access path as the baseline's reload.
            volatile uint32_t *vinput = &data->input[i];
            uint32_t val = *vinput;
            val = __builtin_bswap32(val);

            // PROBE A: no store here (store side-effect removed).

            val = __builtin_bswap32(val);

            if (val != *vinput) {
                uint32_t inputv = data->input[i];
                uint32_t golden = data->input[i];
                uint32_t actual = val;
                uint32_t xorv = golden ^ actual;
                unsigned char *ib = (unsigned char *)&inputv;
                unsigned char *gb = (unsigned char *)&golden;
                unsigned char *ab = (unsigned char *)&actual;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("MovBE-probeA: Round-trip failed at index %u: "
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

DECLARE_TEST(movbe_dump_probe_a, "movbe_dump probe A: no store (store side-effect removed)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
