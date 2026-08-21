/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_g1
 * @parblock
 * SDC-trigger probe, variant G1: **store stream hits ONE LLC set repeatedly
 * (max way-replacement pressure)**, vs probe G2 which spreads the same number
 * of stores across many sets. Together G1/G2 test the LLC-set eviction
 * hypothesis that probes A-F converged on.
 *
 * Core-179 LLC geometry (from /sys/.../cpu179/cache/index3):
 *   24MB, 2048 sets, 15-way, 128B line, shared by cores 168-191 (NUMA node 7).
 * Addresses mapping to the SAME LLC set differ by a multiple of
 *   2048 sets * 128B = 262144B = 256KB.
 *
 * Probe G1 keeps the baseline hot loop byte-for-byte (same aligned_alloc
 * first-touch allocation so input AND swapped land on node 7 = core 179's
 * LLC domain; same reload of input[i] with full `i`), and changes ONLY the
 * store index: `swapped[i]` -> `swapped[(i & 15) * (SET_STRIDE/4)]` where
 * SET_STRIDE = 256KB. So the 16 distinct store addresses are each 256KB apart
 * -> they ALL map to the SAME LLC set, occupying 16 distinct ways. Since the
 * LLC is 15-way, this over-subscribes one set by 1 and forces an eviction on
 * every store, maximizing per-set replacement pressure. The store address
 * still advances every iteration (AGU + store-buffer activity identical in
 * kind to baseline), still crosses cache lines (in fact crosses 128B LLC
 * lines), still back-to-back with the reload, still in the same LLC domain.
 *
 * Probe G2 (companion) does the same but with stride 64B (one LLC line) so
 * the 16 stores hit 16 DIFFERENT sets, one way each -- minimal per-set
 * pressure. Comparing G1 (1 set, max pressure) vs G2 (16 sets, min pressure):
 *   - G1 FAILS at higher rate than G2 / baseline -> eviction pressure is the
 *     mechanism (confirms LLC-set eviction hazard).
 *   - G1 ~= G2 -> set concentration does not matter; mechanism is elsewhere.
 *
 * NOTE: the `and` mask (i & 15) and the `* (SET_STRIDE/4)` scale schedule
 * before the 1st ldr (verified by objdump), so store<->reload stay
 * back-to-back. swapped buffer is sized 16*256KB = 4MB to hold the sparse
 * addresses. input stays 16384 words = 64KB (identical to baseline) so the
 * reload traverses 512 LLC sets as in baseline.
 * @endparblock
 */

#include "sandstone.h"

#define MOVBE_BUFFER_SIZE (1 << 14)          /* input words, identical to baseline */
#define PROBE_G_SET_STRIDE (256 * 1024)      /* 2048 sets * 128B = LLC set span */
#define PROBE_G_SLOTS 16                     /* distinct store addresses */
#define PROBE_G_SWAP_WORDS (PROBE_G_SLOTS * (PROBE_G_SET_STRIDE / 4)) /* 16*65536 */

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_init(struct test *test)
{
    movbe_data *data = (movbe_data *)malloc(sizeof(movbe_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
    /* swapped must be large enough to hold 16 addresses 256KB apart = 4MB.
       64-byte-aligned so the first store line is clean. */
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
    const size_t slot_scale = PROBE_G_SET_STRIDE / 4;  /* word stride between slots */

    TEST_LOOP(test, 1 << 13) {
        // Hot loop byte-for-byte identical to baseline movbe_dump EXCEPT
        // the store index: swapped[(i & 15) * slot_scale]. The 16 store
        // addresses are each 256KB apart -> all map to ONE LLC set, 16 ways
        // (over-subscribing the 15-way LLC -> forced eviction every store).
        // The reload `data->input[i]` below uses the full `i` (identical to
        // baseline) and is the SDC site.
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // PROBE G1: store to one LLC set (16 addresses, 256KB stride).
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
                log_error("MovBE-probeG1: Round-trip failed at index %u: "
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

DECLARE_TEST(movbe_dump_probe_g1, "movbe_dump probe G1: store stream hits ONE LLC set (256KB stride, 16 ways vs 15-way -> max eviction pressure)")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
