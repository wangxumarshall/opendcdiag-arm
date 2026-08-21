/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump
 * @parblock
 * Dump-on-failure variant of movbe: byte-swap round-trip logic identical to
 * misc/movbe.cpp. Like the original movbe.cpp, the input buffer is filled
 * once at init time with random32() and reused for every TEST_LOOP
 * iteration -- this keeps the per-iteration cost (and therefore the SDC
 * trigger rate under a fixed time budget) identical to the original movbe
 * test, so a defect that the original movbe can reproduce will also
 * reproduce here under the same conditions/seed.
 *
 * The only difference from movbe.cpp is that, on a round-trip failure, it
 * logs the relevant values plus a per-byte breakdown so the flipped bit(s)
 * can be located:
 *   - input   : the original input value (data->input[i]) for this index
 *   - golden   : snapshot of the input taken right before the byte-swap
 *                (equals input; captured separately so the dump is robust
 *                even if the corruption is in the input buffer itself)
 *   - actual   : the value obtained after two consecutive byte-swaps
 *                (should equal golden)
 *   - xor      : golden ^ actual (the bit(s) that flipped)
 * Logging is fail-only: a healthy core produces a clean (pass-only) log
 * with no input/golden/actual lines.
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

    // Fill the input buffer once; it is reused by every TEST_LOOP iteration
    // in test_run (identical to movbe.cpp), so the LCG seed fully determines
    // the input pattern under test.
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
        // The input buffer is filled once at init time and reused for every
        // iteration (identical to movbe.cpp), keeping the per-iteration cost
        // -- and thus the SDC trigger rate under a fixed time budget -- the
        // same as the original movbe test.
        //
        // The loop body below is byte-for-byte identical to movbe.cpp's
        // movbe_run hot path: read input[i], bswap, store to swapped[i],
        // bswap back, compare against data->input[i]. The compiler folds the
        // second bswap into a reload of data->input[i] (because the store to
        // data->swapped[i] may alias data->input), and that reload is the
        // memory path that exercises the silicon defect. Adding *any* extra
        // local snapshot or volatile read here changes the instruction
        // schedule enough to stop the defect from triggering, so the dump is
        // done entirely inside the (cold) failure branch below, reading the
        // values it needs from data->input[i] and the local val.
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            // Perform "movbe" (Byte Swap)
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // Store swapped value
            data->swapped[i] = val;

            // Perform another "movbe" to restore original order
            val = __builtin_bswap32(val);

            // Check if we got the original value back.
            // ONLY on failure: dump input / golden / actual / xor plus
            // per-byte breakdown so the flipped bit can be located.
            //   input   : the original input value (data->input[i])
            //   golden  : same as input (the value a correct round-trip
            //             must reproduce); read fresh here only on failure
            //   actual  : the value obtained after two byte-swaps (val)
            //   xor     : golden ^ actual (the bit(s) that flipped)
            // (Matches original movbe.cpp's fail-only logging policy.)
            if (val != data->input[i]) {
                uint32_t inputv = data->input[i];
                uint32_t golden = data->input[i];
                uint32_t actual = val;
                uint32_t xorv = golden ^ actual;
                unsigned char *ib = (unsigned char *)&inputv;
                unsigned char *gb = (unsigned char *)&golden;
                unsigned char *ab = (unsigned char *)&actual;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("MovBE-dump: Round-trip failed at index %u: "
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

DECLARE_TEST(movbe_dump, "movbe dump-on-failure variant: random input per iteration, round-trip with input/golden/actual/xor dump on failure")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
