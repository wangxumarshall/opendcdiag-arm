/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b atomic_seq_cst
 * @parblock
 * atomic_seq_cst verifies the correctness of C++11 memory_order_seq_cst
 * operations on ARMv8. It relies on the compiler to generate appropriate
 * LDAR/STLR pairs and barriers.
 * @endparblock
 */

#include "sandstone.h"
#include <atomic>
#include <cstring> // for memset, free

#define BUFFER_SIZE (1 << 12)

struct atomic_seq_cst_data {
    std::atomic<uint64_t> *shared;
    uint64_t *golden;
};

static int atomic_seq_cst_init(struct test *test)
{
    atomic_seq_cst_data *data = (atomic_seq_cst_data *)malloc(sizeof(atomic_seq_cst_data));
    // aligned_alloc_safe is provided by the framework
    data->golden = (uint64_t *)aligned_alloc_safe(64, BUFFER_SIZE * sizeof(uint64_t));
    
    // Use aligned_alloc for std::atomic buffer, then use placement new
    void *raw_mem = aligned_alloc(64, BUFFER_SIZE * sizeof(std::atomic<uint64_t>));
    data->shared = new (raw_mem) std::atomic<uint64_t>[BUFFER_SIZE];

    // Initialize golden values with random data
    for (size_t i = 0; i < BUFFER_SIZE; ++i) {
        data->golden[i] = random64();
        data->shared[i].store(0, std::memory_order_relaxed);
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int atomic_seq_cst_run(struct test *test, int cpu)
{
    atomic_seq_cst_data *data = (atomic_seq_cst_data *)test->data;
    
    TEST_LOOP(test, 1 << 15) {
        // Pick a random index
        uint32_t idx = random32() & (BUFFER_SIZE - 1);
        uint64_t expected = data->golden[idx];

        // Perform store with seq_cst (Maps to STLR on ARMv8)
        data->shared[idx].store(expected, std::memory_order_seq_cst);

        // Perform load with seq_cst (Maps to LDAR on ARMv8) and verify
        uint64_t actual = data->shared[idx].load(std::memory_order_seq_cst);

        if (actual != expected) {
            report_fail_msg("Seq-cst mismatch at index %u: expected 0x%llx, got 0x%llx", 
                            idx, (unsigned long long)expected, (unsigned long long)actual);
        }
    }

    return EXIT_SUCCESS;
}

static int atomic_seq_cst_cleanup(struct test *test)
{
    atomic_seq_cst_data *data = (atomic_seq_cst_data *)test->data;
    if (data) {
        // Explicitly call destructor for array of atomics
        using atomic_type = std::atomic<uint64_t>;
        data->shared->~atomic_type(); 
        free(data->shared);
        free(data->golden);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(atomic_seq_cst, "C++11 memory_order_seq_cst test")
    .test_init = atomic_seq_cst_init,
    .test_run = atomic_seq_cst_run,
    .test_cleanup = atomic_seq_cst_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
