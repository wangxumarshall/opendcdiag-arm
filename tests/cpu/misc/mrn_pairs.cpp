/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_pairs
 * @parblock
 * Test that copies an input array from memory to an output array via a
 * temporary buffer with 8 pairs of MRN store-load at a time. Unrolls the
 * store-to-load forwarding loop by 8 to increase concurrency pressure
 * on the memory renaming unit.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define MRN_PAIRS_COUNT 1024

struct MrnPairsData {
    uint64_t *src;
};

#if defined(__x86_64__) || defined(__i386__)

static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("movq %1, %0" : "=m"(*reinterpret_cast<uint64_t*>(addr)) : "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("movq %1, %0" : "=r"(res) : "m"(*reinterpret_cast<const uint64_t*>(addr)) : "memory");
    return res;
}

#elif defined(__aarch64__)

static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int mrn_pairs_init(struct test *test) {
    auto *data = static_cast<MrnPairsData *>(malloc(sizeof(MrnPairsData)));
    data->src = static_cast<uint64_t *>(aligned_alloc(64, MRN_PAIRS_COUNT * sizeof(uint64_t)));
    memset_random(data->src, MRN_PAIRS_COUNT * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_pairs_run(struct test *test, int cpu) {
    auto *data = static_cast<MrnPairsData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_PAIRS_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_PAIRS_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        // Unrolled by 8: 8 stores followed by 8 loads from the same buffer
        for (int i = 0; i < MRN_PAIRS_COUNT; i += 8) {
            store_qword(&temp[i+0], data->src[i+0]);
            store_qword(&temp[i+1], data->src[i+1]);
            store_qword(&temp[i+2], data->src[i+2]);
            store_qword(&temp[i+3], data->src[i+3]);
            store_qword(&temp[i+4], data->src[i+4]);
            store_qword(&temp[i+5], data->src[i+5]);
            store_qword(&temp[i+6], data->src[i+6]);
            store_qword(&temp[i+7], data->src[i+7]);

            store_qword(&dst[i+0], load_qword(&temp[i+0]));
            store_qword(&dst[i+1], load_qword(&temp[i+1]));
            store_qword(&dst[i+2], load_qword(&temp[i+2]));
            store_qword(&dst[i+3], load_qword(&temp[i+3]));
            store_qword(&dst[i+4], load_qword(&temp[i+4]));
            store_qword(&dst[i+5], load_qword(&temp[i+5]));
            store_qword(&dst[i+6], load_qword(&temp[i+6]));
            store_qword(&dst[i+7], load_qword(&temp[i+7]));
        }

        if (memcmp(dst, data->src, MRN_PAIRS_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("mrn_pairs data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int mrn_pairs_cleanup(struct test *test) {
    auto *data = static_cast<MrnPairsData *>(test->data);
    if (data) {
        free(data->src);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_pairs, "Test that copies an input array from memory to an output array via a temporary buffer with 8 pairs of MRN store-load at a time")
    .test_init = mrn_pairs_init,
    .test_run = mrn_pairs_run,
    .test_cleanup = mrn_pairs_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
