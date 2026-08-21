/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_h
 * @parblock
 * SDC-trigger probe, variant H: **sweep the store-side distinct-line footprint**
 * to find the threshold at which core-179's defect turns on. Distinguishes a
 * fixed-size resource (MSHR / prefetch / write-combine entries -> threshold
 * near a power of two) from an open-ended footprint effect.
 *
 * Probes A-G established the trigger needs a store stream with a LARGE
 * number of DISTINCT, CONTIGUOUSLY-ADVANCING lines, back-to-back with the
 * reload, in the same LLC domain. Crucially G1 (16 lines, all in ONE LLC
 * set, 16-way over-subscription) PASSED -- disproving single-set eviction.
 * So the surviving variable is the store-side distinct-LINE count. Probe H
 * sweeps it: store to `swapped[(i % LINES) * (LINE/4)]`, where LINES is set
 * by the PROBE_H_LINES env var (default 512 = baseline-equivalent). The
 * stride is one LLC line (128B), so the LINES stores hit LINES distinct
 * consecutive sets (1 way each -- no set over-subscription, unlike G1),
 * and the store address advances into a NEW line every iteration.
 *
 * Sweep points: 32, 64, 128, 256, 512 (512 == baseline footprint, must FAIL).
 * 16 (==probe G2) is known PASS. The threshold line count where the defect
 * turns on pins the resource: e.g. a turn-on near 128/256 distinct lines
 * points at a bounded MSHR/prefetcher pool of that size.
 *
 * Reload of `data->input[i]` uses the full `i` (512 LLC lines, identical to
 * baseline) and is the SDC site. Verified by objdump that the reload is
 * byte-identical to baseline and store<->reload stay back-to-back (the
 * modulo+scale schedule before the 1st ldr, not between store and reload).
 *
 * NOTE: LINES must be a power of two in [1..512]; the store index uses
 * `i & (LINES*32 - 1)` (NOT a runtime modulo) so no `udiv`/`msub` lands on
 * the hot path -- the inner-loop timing stays as close to baseline as
 * possible (only a single `and` mask, scheduled before the 1st ldr). This
 * keeps the store<->reload back-to-back window intact AND avoids the
 * multi-cycle division perturbing per-iteration throughput. init() rejects
 * non-power-of-two LINES values so the mask is always valid.
 *
 * CRITICAL: the stride is 4B (baseline-style) NOT 128B. The store writes
 * 32 words per LLC line before advancing, exactly like baseline. So:
 *   LINES=512 -> mask=16383 -> swapped[i & 16383] == swapped[i] (i in 0..16383)
 *                == BASELINE byte-for-byte. This is the positive control and
 *                MUST fail on core 179; if it doesn't, the probe is invalid.
 * Smaller LINES shrink the distinct-line footprint while keeping the 4B
 * stride and per-line fill pattern identical to baseline.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdlib>

#define MOVBE_BUFFER_SIZE (1 << 14)          /* input words, identical to baseline */
#define PROBE_H_LINE 128                    /* LLC line size on core 179 */
/* swapped holds up to 512 lines * 128B = 64KB (== baseline footprint max). */
#define PROBE_H_MAX_LINES 512
#define PROBE_H_SWAP_WORDS (PROBE_H_MAX_LINES * (PROBE_H_LINE / 4))  /* 512*32 = 16384 */

static uint32_t g_probe_h_mask = PROBE_H_MAX_LINES * (PROBE_H_LINE / 4) - 1;  /* i & mask; default 16383 = baseline */
static uint32_t g_probe_h_lines = PROBE_H_MAX_LINES;      /* for logging only */

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_init(struct test *test)
{
    /* Allow overriding the distinct-line count at runtime. Must be a power
     * of two in [1..512] so the store index `i & (LINES*32 - 1)` is a simple
     * mask (no division on the hot path, preserving baseline 4B-stride timing). */
    if (const char *env = getenv("PROBE_H_LINES")) {
        long v = strtol(env, nullptr, 10);
        if (v >= 1 && v <= PROBE_H_MAX_LINES && (v & (v - 1)) == 0) {
            g_probe_h_lines = (uint32_t)v;
            g_probe_h_mask = (uint32_t)v * (PROBE_H_LINE / 4) - 1;
        }
    }

    movbe_data *data = (movbe_data *)malloc(sizeof(movbe_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MOVBE_BUFFER_SIZE * sizeof(uint32_t));
    data->swapped = (uint32_t *)aligned_alloc_safe(64, PROBE_H_SWAP_WORDS * sizeof(uint32_t));

    for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
        data->input[i] = random32();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movbe_run(struct test *test, int cpu)
{
    movbe_data *data = (movbe_data *)test->data;
    const size_t lines = g_probe_h_lines;          /* distinct store lines (logging) */
    const size_t mask = g_probe_h_mask;            /* i & mask = i & (lines*32 - 1) */

    TEST_LOOP(test, 1 << 13) {
        // Hot loop: store index = swapped[i & mask], stride 4B (baseline-style).
        // For LINES=512, mask=16383 and i in 0..16383, so this is EXACTLY
        // swapped[i] == baseline. For smaller LINES the store cycles a smaller
        // region of `lines` distinct LLC lines but keeps the 4B stride and
        // 32-words-per-line fill pattern identical to baseline. Reload of
        // input[i] uses the full `i` (512 LLC lines, identical to baseline)
        // and is the SDC site. The `and` mask schedules before the 1st ldr
        // (verified by objdump) so store<->reload stay back-to-back, and
        // there is NO division on the hot path.
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // PROBE H: store footprint = `lines` distinct LLC lines, 4B stride.
            data->swapped[i & mask] = val;

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
                log_error("MovBE-probeH(lines=%u): Round-trip failed at index %u: "
                          "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X | "
                          "bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
                          "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
                          "xor=%02X%02X%02X%02X",
                          (unsigned)lines, (unsigned)i, inputv, golden, actual, xorv,
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

DECLARE_TEST(movbe_dump_probe_h, "movbe_dump probe H: sweep store-side distinct LLC-line footprint (PROBE_H_LINES env; stride=128B=1 line/set, 1 way each) to find defect-on threshold")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
