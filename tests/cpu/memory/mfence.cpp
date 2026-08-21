/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mfence
 * @parblock
 * mfence tests the functionality of a full memory barrier. On ARMv8,
 * __sync_synchronize() generates a DMB SY instruction to serialize
 * all memory accesses.
 * @endparblock
 */

#include "sandstone.h"
#include <cstring> // for memset, free

#define BUFFER_SIZE (1 << 12)

struct mfence_data {
    volatile uint64_t *store_a;
    volatile uint64_t *store_b;
    uint64_t *golden_a;
    uint64_t *golden_b;
};

static int mfence_init(struct test *test)
{
    mfence_data *data = (mfence_data *)malloc(sizeof(mfence_data));
    
    data->golden_a = (uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    data->golden_b = (uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    data->store_a = (volatile uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    data->store_b = (volatile uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));

    memset((void *)data->store_a, 0, BUFFER_SIZE * sizeof(uint64_t));
    memset((void *)data->store_b, 0, BUFFER_SIZE * sizeof(uint64_t));

    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        data->golden_a[i] = random64();
        data->golden_b[i] = random64();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mfence_run(struct test *test, int cpu)
{
    mfence_data *data = (mfence_data *)test->data;

    TEST_LOOP(test, 1 << 14) {
        uint32_t idx = random32() & (BUFFER_SIZE - 1);

        // Store to A
        data->store_a[idx] = data->golden_a[idx];

        // Full Memory Barrier: On ARMv8 this maps to DMB SY
        // Ensures previous stores are globally visible before proceeding
        __sync_synchronize();

        // Store to B
        data->store_b[idx] = data->golden_b[idx];

        // Verify both values
        if (data->store_a[idx] != data->golden_a[idx]) {
            report_fail_msg("MFENCE: Store A mismatch at index %u", idx);
        }
        if (data->store_b[idx] != data->golden_b[idx]) {
            report_fail_msg("MFENCE: Store B mismatch at index %u", idx);
        }
    }

    return EXIT_SUCCESS;
}

static int mfence_cleanup(struct test *test)
{
    mfence_data *data = (mfence_data *)test->data;
    if (data) {
        free((void *)data->store_a);
        free((void *)data->store_b);
        free(data->golden_a);
        free(data->golden_b);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mfence, "MFENCE test (previously used C++11 memory_order_seq_cst)")
    .test_init = mfence_init,
    .test_run = mfence_run,
    .test_cleanup = mfence_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
