/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movmskpspd
 * @parblock
 * Tests packed single and double movmsk instructions. On x86, this exercises
 * the MOVMSKPS and MOVMSKPD instructions which extract the sign bits from
 * packed single- and double-precision floating-point values in an XMM
 * register and pack them into a general-purpose register mask. On ARMv8,
 * the equivalent functionality is tested using the USHR NEON instruction
 * to isolate the sign bit directly, matching x86 behavior for -0.0 and NaNs.
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>
#include <cstring>

#define MOVMSK_COUNT 256

struct MovmskpspdData {
    float    *fvalues;    /* MOVMSK_COUNT * 4 floats, 16-byte aligned */
    double   *dvalues;    /* MOVMSK_COUNT * 2 doubles, 16-byte aligned */
    uint32_t *fexpected;  /* expected masks for single-precision */
    uint32_t *dexpected;  /* expected masks for double-precision */
};

/* ------------------------------------------------------------------ */
/* Platform-specific helpers                                          */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>

/* MOVMSKPS: extract 4 sign bits from packed singles → GP register */
static inline uint32_t do_movmskps(const float *p)
{
    uint32_t mask;
    __m128 v = _mm_load_ps(p);
    __asm__ volatile ("movmskps %1, %0" : "=r"(mask) : "x"(v));
    return mask;
}

/* MOVMSKPD: extract 2 sign bits from packed doubles → GP register */
static inline uint32_t do_movmskpd(const double *p)
{
    uint32_t mask;
    __m128d v = _mm_load_pd(p);
    __asm__ volatile ("movmskpd %1, %0" : "=r"(mask) : "x"(v));
    return mask;
}

#elif defined(__aarch64__)

/*
 * ARM equivalent of MOVMSKPS: use USHR to logically shift each 32-bit
 * lane right by 31 bits, isolating the sign bit at bit 0. Then extract
 * each lane and pack into a 4-bit mask.
 */
static inline uint32_t do_movmskps(const float *p)
{
    uint32_t r0, r1, r2, r3;
    __asm__ volatile (
        "ldr q0, [%4]\n\t"             /* Load 128 bits into v0 */
        "ushr v0.4s, v0.4s, #31\n\t"  /* Shift each 32-bit lane right by 31 */
        "umov %w0, v0.s[0]\n\t"       /* Extract lane 0 */
        "umov %w1, v0.s[1]\n\t"       /* Extract lane 1 */
        "umov %w2, v0.s[2]\n\t"       /* Extract lane 2 */
        "umov %w3, v0.s[3]\n\t"       /* Extract lane 3 */
        : "=r"(r0), "=r"(r1), "=r"(r2), "=r"(r3)
        : "r"(p)
        : "memory", "v0"
    );
    return r0 | (r1 << 1) | (r2 << 2) | (r3 << 3);
}

/*
 * ARM equivalent of MOVMSKPD: use USHR to logically shift each 64-bit
 * lane right by 63 bits, isolating the sign bit at bit 0. Then extract
 * each lane and pack into a 2-bit mask.
 */
static inline uint32_t do_movmskpd(const double *p)
{
    uint64_t r0, r1;
    __asm__ volatile (
        "ldr q0, [%2]\n\t"             /* Load 128 bits into v0 */
        "ushr v0.2d, v0.2d, #63\n\t"  /* Shift each 64-bit lane right by 63 */
        "umov %0, v0.d[0]\n\t"        /* Extract lane 0 */
        "umov %1, v0.d[1]\n\t"        /* Extract lane 1 */
        : "=r"(r0), "=r"(r1)
        : "r"(p)
        : "memory", "v0"
    );
    return static_cast<uint32_t>(r0 | (r1 << 1));
}

#endif /* platform */

/* ------------------------------------------------------------------ */
/* Standard test callbacks                                            */
/* ------------------------------------------------------------------ */

static int movmskpspd_init(struct test *test)
{
    auto *data = static_cast<MovmskpspdData *>(malloc(sizeof(MovmskpspdData)));

    data->fvalues  = static_cast<float *>(aligned_alloc(16, MOVMSK_COUNT * 4 * sizeof(float)));
    data->dvalues  = static_cast<double *>(aligned_alloc(16, MOVMSK_COUNT * 2 * sizeof(double)));
    data->fexpected = static_cast<uint32_t *>(malloc(MOVMSK_COUNT * sizeof(uint32_t)));
    data->dexpected = static_cast<uint32_t *>(malloc(MOVMSK_COUNT * sizeof(uint32_t)));

    memset_random(data->fvalues, MOVMSK_COUNT * 4 * sizeof(float));
    memset_random(data->dvalues, MOVMSK_COUNT * 2 * sizeof(double));

    /*
     * Compute golden masks by examining the sign bit of each float/double
     * directly, without using the instruction under test.
     */
    for (int i = 0; i < MOVMSK_COUNT; i++) {
        uint32_t mask = 0;
        for (int j = 0; j < 4; j++) {
            uint32_t bits;
            std::memcpy(&bits, &data->fvalues[i * 4 + j], sizeof(bits));
            if (bits & 0x80000000u)
                mask |= (1u << j);
        }
        data->fexpected[i] = mask;

        mask = 0;
        for (int j = 0; j < 2; j++) {
            uint64_t bits;
            std::memcpy(&bits, &data->dvalues[i * 2 + j], sizeof(bits));
            if (bits & 0x8000000000000000ULL)
                mask |= (1u << j);
        }
        data->dexpected[i] = mask;
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movmskpspd_run(struct test *test, int cpu)
{
    auto *data = static_cast<MovmskpspdData *>(test->data);

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < MOVMSK_COUNT; i++) {
            uint32_t mask = do_movmskps(&data->fvalues[i * 4]);
            if (mask != data->fexpected[i]) {
                report_fail_msg(
                    "movmskps mismatch at index %d: expected 0x%x got 0x%x",
                    i, data->fexpected[i], mask);
            }

            uint32_t dmask = do_movmskpd(&data->dvalues[i * 2]);
            if (dmask != data->dexpected[i]) {
                report_fail_msg(
                    "movmskpd mismatch at index %d: expected 0x%x got 0x%x",
                    i, data->dexpected[i], dmask);
            }
        }
    }
    return EXIT_SUCCESS;
}

static int movmskpspd_cleanup(struct test *test)
{
    auto *data = static_cast<MovmskpspdData *>(test->data);
    if (data) {
        free(data->dexpected);
        free(data->fexpected);
        free(data->dvalues);
        free(data->fvalues);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movmskpspd, "Tests packed single and double movmsk instructions")
    .test_init    = movmskpspd_init,
    .test_run     = movmskpspd_run,
    .test_cleanup = movmskpspd_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
