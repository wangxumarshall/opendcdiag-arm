/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b movdq2q
 * @parblock
 * ARM version of movdq2q. Extracts low 64-bit from NEON V0 register to X0.
 * Explicitly forces register usage to stress specific data paths.
 * Fixed: Corrected assembly syntax for moving vector element to GPR.
 * @endparblock
 */

#include "sandstone.h"
#include <arm_neon.h>

#define MOV_COUNT (1 << 10)

struct movdq2q_data {
    uint64_t *high_parts;
    uint64_t *low_parts;
};

static int movdq2q_init(struct test *test)
{
    movdq2q_data *data = (movdq2q_data *)malloc(sizeof(movdq2q_data));
    data->high_parts = (uint64_t *)aligned_alloc_safe(64, MOV_COUNT * sizeof(uint64_t));
    data->low_parts = (uint64_t *)aligned_alloc_safe(64, MOV_COUNT * sizeof(uint64_t));

    for (size_t i = 0; i < MOV_COUNT; ++i) {
        data->high_parts[i] = random64();
        data->low_parts[i] = random64();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int movdq2q_run(struct test *test, int cpu)
{
    movdq2q_data *data = (movdq2q_data *)test->data;

    TEST_LOOP(test, 1 << 10) {
        for (size_t i = 0; i < MOV_COUNT; ++i) {
            // Prepare the data in a standard vector variable
            uint64x2_t src_vec = vcombine_u64(vld1_u64(&data->low_parts[i]), vld1_u64(&data->high_parts[i]));

            // Force the source vector into V0
            register uint64x2_t v0 asm("v0") = src_vec;
            
            // Prepare the destination variable and force it into X0
            register uint64_t x0 asm("x0");

            // Correct ARM64 assembly syntax for extracting vector element to GPR
            // Instruction: mov x0, v0.d[0]
            // Syntax: mov <Xd>, <Vm>.<Ts>[<index>]
            __asm__ __volatile__(
                "mov %0, %1.d[0]"
                : "=r"(x0)   // Output: X0 (General Purpose Register)
                : "w"(v0)    // Input: V0 (SIMD/FP register class)
                : "memory"
            );

            uint64_t result = x0;

            if (result != data->low_parts[i]) {
                report_fail_msg("MovDQ2Q: Low 64-bit mismatch at index %lu", i);
            }
        }
    }

    return EXIT_SUCCESS;
}

static int movdq2q_cleanup(struct test *test)
{
    movdq2q_data *data = (movdq2q_data *)test->data;
    if (data) {
        free(data->high_parts);
        free(data->low_parts);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(movdq2q, "Does a bunch of MMX moves from xmm registers and checks values")
    .test_init = movdq2q_init,
    .test_run = movdq2q_run,
    .test_cleanup = movdq2q_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
