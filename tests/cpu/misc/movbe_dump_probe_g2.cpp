/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_g2
 * @parblock
 * SDC-trigger probe, variant G2: **store stream spreads across 16 DIFFERENT
 * LLC sets (minimal per-set pressure)** -- the companion control for probe
 * G1. Same number of distinct store addresses, same AGU/store-buffer
 * activity, same cross-line stepping, same LLC domain, same back-to-back
 * timing; the ONLY variable vs G1 is how concentrated the stores are onto
 * LLC sets.
 *
 * Core-179 LLC: 2048 sets, 15-way, 128B line. G2 uses stride = 128B (one LLC
 * line) so the 16 store addresses hit 16 CONSECUTIVE LLC sets, one way each
 * -- each set sees only 1 store, no over-subscription, minimal replacement
 * pressure. G1 (stride 256KB) hits the SAME set 16 times -> max pressure.
 *
 * Compare:
 *   - G1 FAILS more than G2 -> per-set eviction pressure is the mechanism.
 *   - G1 ~= G2 -> set concentration irrelevant; mechanism elsewhere.
 *
 * The hot loop is byte-for-byte identical to baseline except the store
 * index: `swapped[i]` -> `swapped[(i & 15) * (LINE/4)]` where LINE=128B.
 * The mask+scale schedule before the 1st ldr (verified by objdump) so
 * store<->reload stay back-to-back. swapped buffer sized 16*128B = 2KB,
 * easily holds the 16 addresses; input stays 16384 words = 64KB identical to
 * baseline so the reload traverses 512 LLC sets as in baseline.
 * @endparblock
 */

#include "sandstone.h"

#define MOVBE_BUFFER_SIZE (1 << 14)          /* input words, identical to baseline */
#define PROBE_G_LINE 128                    /* LLC line size on core 179 */
#define PROBE_G_SLOTS 16                    /* distinct store addresses */
#define PROBE_G_SWAP_WORDS (PROBE_G_SLOTS * (PROBE_G_LINE / 4))  /* 16*32 = 512 words */

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_init(struct test *test)
{
    movbe_data *data = (movbe_data *)malloc(sizeof(movbe_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
    data->swapped = (uint32_t *)aligned_alloc_safe(64, PROBE_G_SWAP_WORDS * sizeof(uint32_t));

    for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movbe_run(struct test *test, int cpu)
{
    movbe_data *data = (movbe_data *)test->data;
    const size_t slot_scale = PROBE_G_LINE / 4;  /* word stride = 32 -> 128B, one LLC line apart */

    TEST_LOOP(test, 1 << 13) {
        // Hot loop byte-for-byte identical to baseline EXCEPT store index:
        // swapped[(i & 15) * slot_scale], stride 128B = one LLC line.
        // 16 store addresses hit 16 CONSECUTIVE LLC sets, 1 way each.
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // PROBE G2: store across 16 different LLC sets (128B stride).
            data->swapped[(i & 15) * slot_scale] = val;

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
                log_error("MovBE-probeG2: Round-trip failed at index %u: "
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

DECLARE_TEST(movbe_dump_probe_g2, "movbe_dump probe G2: store stream spreads across 16 LLC sets (128B stride, 1 way each -> min eviction pressure; control for G1)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
