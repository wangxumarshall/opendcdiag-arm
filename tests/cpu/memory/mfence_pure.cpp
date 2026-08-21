/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mfence_pure
 * @parblock
 * mfence_pure tests synchronization using purely a full memory barrier (DMB SY on ARMv8).
 * @endparblock
 */

#include "sandstone.h"
#include <cstring> // for memset, free

#define BUFFER_SIZE (1 << 12)

struct mfence_pure_data {
    volatile uint64_t *data_src;
    volatile uint64_t *data_dst;
    uint64_t *golden_src;
    uint64_t *golden_dst;
};

static int mfence_pure_init(struct test *test)
{
    mfence_pure_data *data = (mfence_pure_data *)malloc(sizeof(mfence_pure_data));
    
    data->golden_src = (uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    data->golden_dst = (uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    data->data_src = (volatile uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    data->data_dst = (volatile uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));

    // Initialize source with random data
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        data->golden_src[i] = random64();
        data->data_src[i] = data->golden_src[i];
        data->golden_dst[i] = random64();
        data->data_dst[i] = 0; // Destination starts at 0
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mfence_pure_run(struct test *test, int cpu)
{
    mfence_pure_data *data = (mfence_pure_data *)test->data;

    TEST_LOOP(test, 1 << 14) {
        uint32_t idx = random32() & (BUFFER_SIZE - 1);
        
        // Load from source
        uint64_t val = data->data_src[idx];

        // Full Memory Barrier: On ARMv8 this compiles to DMB SY
        __sync_synchronize();

        // Store to destination
        data->data_dst[idx] = data->golden_dst[idx];

        // Verify loaded value matches golden source
        if (val != data->golden_src[idx]) {
            report_fail_msg("MFENCE_PURE: Load mismatch at index %u", idx);
        }
        
        // Verify stored value matches golden destination
        if (data->data_dst[idx] != data->golden_dst[idx]) {
            report_fail_msg("MFENCE_PURE: Store mismatch at index %u", idx);
        }
    }

    return EXIT_SUCCESS;
}

static int mfence_pure_cleanup(struct test *test)
{
    mfence_pure_data *data = (mfence_pure_data *)test->data;
    if (data) {
        free((void *)data->data_src);
        free((void *)data->data_dst);
        free(data->golden_src);
        free(data->golden_dst);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mfence_pure, "synchronization using purely mfence")
    .test_init = mfence_pure_init,
    .test_run = mfence_pure_run,
    .test_cleanup = mfence_pure_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
