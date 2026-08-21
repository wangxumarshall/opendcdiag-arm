/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movq2dq
 * @parblock
 * Does a bunch of MMX -> XMM moves and checks values. On x86, this tests
 * the MOVQ2DQ instruction which moves 64 bits from an MMX register to the
 * low 64 bits of an XMM register, zeroing the upper 64 bits. On ARMv8, the
 * equivalent FMOV (scalar) instruction is used to move 64 bits from a
 * general-purpose register to the low 64 bits of a SIMD register, which
 * also zeroes the upper 64 bits.
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>

#define MOVQ2DQ_COUNT 256

struct Movq2dqData {
    uint64_t *values;
};

/* ------------------------------------------------------------------ */
/* Platform-specific helpers                                          */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>

/*
 * MOVQ2DQ: move 64 bits from MMX (mm0) to low 64 of XMM, zero upper 64.
 * We use EMMS afterwards to restore the x87 FPU tag word to a clean state.
 * All x87 registers are listed as clobbers because MMX aliases them.
 */
static inline void do_movq2dq(uint64_t src, uint64_t *lo, uint64_t *hi)
{
    __m128i result;
    __asm__ volatile (
        "movq    %1, %%mm0\n\t"
        "movq2dq %%mm0, %0\n\t"
        "emms"
        : "=x"(result)
        : "m"(src)
        : "mm0",
          "st", "st(1)", "st(2)", "st(3)",
          "st(4)", "st(5)", "st(6)", "st(7)",
          "memory"
    );
    /* Extract low and high 64-bit halves */
    alignas(16) uint64_t buf[2];
    _mm_store_si128(reinterpret_cast<__m128i*>(buf), result);
    *lo = buf[0];
    *hi = buf[1];
}

#elif defined(__aarch64__)

#include <arm_neon.h>

/*
 * ARM equivalent: FMOV Dd, Rn moves 64 bits from a general-purpose
 * register into the D (low 64-bit) sub-register of a V register, and
 * zeroes the upper 64 bits — behaviour identical to MOVQ2DQ.
 */
static inline void do_movq2dq(uint64_t src, uint64_t *lo, uint64_t *hi)
{
    uint64x2_t result;
    __asm__ volatile (
        "fmov %d0, %1"
        : "=w"(result)
        : "r"(src)
    );
    *lo = vgetq_lane_u64(result, 0);
    *hi = vgetq_lane_u64(result, 1);
}

#endif /* platform */

/* ------------------------------------------------------------------ */
/* Standard test callbacks                                            */
/* ------------------------------------------------------------------ */

static int movq2dq_init(struct test *test)
{
    auto *data = static_cast<Movq2dqData *>(malloc(sizeof(Movq2dqData)));
    data->values = static_cast<uint64_t *>(malloc(MOVQ2DQ_COUNT * sizeof(uint64_t)));
    memset_random(data->values, MOVQ2DQ_COUNT * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}

static int movq2dq_run(struct test *test, int cpu)
{
    auto *data = static_cast<Movq2dqData *>(test->data);

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < MOVQ2DQ_COUNT; i++) {
            uint64_t lo, hi;
            do_movq2dq(data->values[i], &lo, &hi);

            if (lo != data->values[i]) {
                report_fail_msg(
                    "movq2dq low 64-bit mismatch at index %d: "
                    "expected 0x%llx got 0x%llx",
                    i,
                    static_cast<unsigned long long>(data->values[i]),
                    static_cast<unsigned long long>(lo));
            }
            if (hi != 0) {
                report_fail_msg(
                    "movq2dq high 64-bit not zeroed at index %d: "
                    "expected 0x0 got 0x%llx",
                    i, static_cast<unsigned long long>(hi));
            }
        }
    }
    return EXIT_SUCCESS;
}

static int movq2dq_cleanup(struct test *test)
{
    auto *data = static_cast<Movq2dqData *>(test->data);
    if (data) {
        free(data->values);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movq2dq, "Does a bunch of MMX -> XMM moves and checks values")
    .test_init    = movq2dq_init,
    .test_run     = movq2dq_run,
    .test_cleanup = movq2dq_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
