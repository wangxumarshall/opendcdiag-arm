/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b modified_sort
 * @parblock
 * Uses modified versions of various sort algorithms to target memory renaming.
 * Fixed version: Uses local buffers to ensure thread-safety and correctness.
 * @endparblock
 */

#include "sandstone.h"
#include <cstring>
#include <algorithm>

#define ARRAY_SIZE (1 << 8)

// No shared data structure needed in init to avoid race conditions.
// But we can keep init empty or minimal.
static int modified_sort_init(struct test *test)
{
    // Nothing to initialize globally. 
    // We will allocate buffers in run() to ensure thread isolation.
    return EXIT_SUCCESS;
}

static int modified_sort_run(struct test *test, int cpu)
{
    // 1. Allocate thread-local buffers
    // aligned_alloc_safe is provided by the framework and zeroes memory
    uint64_t *data_src = (uint64_t *)aligned_alloc_safe(64, ARRAY_SIZE * sizeof(uint64_t));
    uint64_t *data_dst = (uint64_t *)aligned_alloc_safe(64, ARRAY_SIZE * sizeof(uint64_t));

    TEST_LOOP(test, 1 << 5) {
        // 2. Initialize source buffer with random data
        for (size_t i = 0; i < ARRAY_SIZE; ++i) {
            data_src[i] = random64();
        }

        // 3. Copy to destination buffer to serve as "Golden Reference"
        memcpy(data_dst, data_src, ARRAY_SIZE * sizeof(uint64_t));

        // 4. Sort the "Golden Reference" using standard library
        std::sort(data_dst, data_dst + ARRAY_SIZE);

        // 5. Perform "Modified Sort" (Bubble Sort variant) on source buffer
        // This stresses load/store units and memory renaming logic
        for (size_t i = 0; i < ARRAY_SIZE - 1; i++) {
            for (size_t j = 0; j < ARRAY_SIZE - i - 1; j++) {
                // Swap if elements are in the wrong order (Ascending)
                if (data_src[j] > data_src[j + 1]) {
                    uint64_t temp = data_src[j];
                    data_src[j] = data_src[j + 1];
                    data_src[j + 1] = temp;
                }
            }
        }

        // 6. Verify: Compare our manually sorted result with the library sorted result
        if (memcmp(data_src, data_dst, ARRAY_SIZE * sizeof(uint64_t)) != 0) {
            report_fail_msg("Modified Sort: Result does not match golden reference (Thread %d)", cpu);
        }
    }

    // Cleanup thread-local memory
    free(data_src);
    free(data_dst);

    return EXIT_SUCCESS;
}

// Cleanup is not strictly needed since we didn't allocate anything in init,
// but keeping the function signature consistent is good practice.
static int modified_sort_cleanup(struct test *test)
{
    return EXIT_SUCCESS;
}

DECLARE_TEST(modified_sort, "Uses modified versions of various sort algorithms to target memory renaming")
    .test_init = modified_sort_init,
    .test_run = modified_sort_run,
    .test_cleanup = modified_sort_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
