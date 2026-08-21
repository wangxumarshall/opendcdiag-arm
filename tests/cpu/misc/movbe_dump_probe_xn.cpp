/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movbe_dump_probe_xn
 * @parblock
 * SDC-trigger probe, variant XN: **parametrized extra-NOP count** to draw the
 * timing-phase sensitivity curve quantitatively. This is the V3 quantitative
 * instrument (see docs/MOVBE_SDC_CORE179_LOCALIZATION_REPORT_V2.md §3.3).
 *
 * V2 established (qualitatively) that one extra semantically-no-op ALU instr
 * collapses baseline's ~100% trigger to ~10–20% (probe H `and`, probe X `eor`).
 * XN parametrizes the extra-instruction count via the PROBE_XN_NOPS env var
 * (default 0 == baseline), inserting N independent `eor w?,w?,wzr` no-ops on the
 * value register between the first bswap and the store. objdump MUST confirm:
 * (a) exactly N `eor` instructions in the loop before the store, (b) store
 * stays `str [.., x19, lsl #2]` == baseline, (c) NO pointer-reload `ldp` in
 * the loop, (d) store<->reload relationship matches probeX (NOT back-to-back
 * when N>0, because eor breaks the 2nd-bswap-fold-to-reload). For N=0 the loop
 * is byte-identical to baseline (back-to-back), serving as the 100% control.
 *
 * The curve of trigger-rate-vs-N answers: is the phase window narrow (rate→0
 * at small N) or wide (rate persists)? A logistic/step shape pins the phase
 * width; a flat-low shape means the window is narrower than 1 issue slot.
 *
 * Implementation: each NOP is `asm("eor %w0, %w0, wzr" : "=r"(val) : "0"(val));`
 * in a loop run N times. Plain C `val^0u` is DCE'd; asm with IO operands is not.
 * No volatile/memory clobber (that made the compiler reload pointers).
 * @endparblock
 */

#include "sandstone.h"

#define MOVBE_BUFFER_SIZE (1 << 14)

static int g_probe_xn_nops = 0;   /* extra no-op ALU instructions, from PROBE_XN_NOPS */

struct movbe_data {
    uint32_t *input;
    uint32_t *swapped;
};

static int movbe_init(struct test *test)
{
    if (const char *env = getenv("PROBE_XN_NOPS")) {
        long v = strtol(env, nullptr, 10);
        if (v >= 0 && v <= 16) g_probe_xn_nops = (int)v;
    }

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
    const int nops = g_probe_xn_nops;

    TEST_LOOP(test, 1 << 13) {
        for (size_t i = 0; i < MOVBE_BUFFER_SIZE; ++i) {
            uint32_t val = data->input[i];
            val = __builtin_bswap32(val);

            // PROBE XN: insert N semantically-no-op ALU instructions (eor with
            // wzr) on val. asm IO operands prevent DCE; no volatile/memory so
            // the compiler keeps pointers loaded outside the loop. N=0 == baseline.
            for (int n = 0; n < nops; ++n)
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
                log_error("MovBE-probeXN(nops=%u): Round-trip failed at index %u: "
                          "input=0x%08X golden=0x%08X actual=0x%08X xor=0x%08X | "
                          "bytes[b3 b2 b1 b0] input=%02X%02X%02X%02X "
                          "golden=%02X%02X%02X%02X actual=%02X%02X%02X%02X "
                          "xor=%02X%02X%02X%02X",
                          (unsigned)nops, (unsigned)i, inputv, golden, actual, xorv,
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

DECLARE_TEST(movbe_dump_probe_xn, "movbe_dump probe XN: parametrized extra-NOP count (PROBE_XN_NOPS env) for timing-phase sensitivity curve")
    .test_init = movbe_init,
    .test_run = movbe_run,
    .test_cleanup = movbe_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
