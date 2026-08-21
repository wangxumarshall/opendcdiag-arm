/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw
 * @parblock
 * Simple test that computes add/sub/xor/and of two source arrays and
 * copies it to a destination array using multiple pairs of mrn store-load.
 * Combines ALU read-modify-write operations with store-to-load forwarding
 * pressure.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define MRN_RMW_COUNT 1024

struct MrnRmwData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
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

static int mrn_rmw_init(struct test *test) {
    auto *data = static_cast<MrnRmwData *>(malloc(sizeof(MrnRmwData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, MRN_RMW_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, MRN_RMW_COUNT * sizeof(uint64_t));

    // Compute expected golden values
    for (int i = 0; i < MRN_RMW_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_rmw_run(struct test *test, int cpu) {
    auto *data = static_cast<MrnRmwData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < MRN_RMW_COUNT; i++) {
            uint64_t a = data->srcA[i];
            uint64_t b = data->srcB[i];
            uint64_t res;

            switch (i % 4) {
                case 0: res = a + b; break;
                case 1: res = a - b; break;
                case 2: res = a ^ b; break;
                case 3: res = a & b; break;
            }

            // Store RMW result to temp, then load to dst
            store_qword(&temp[i], res);
            store_qword(&dst[i], load_qword(&temp[i]));
        }

        if (memcmp(dst, data->expected, MRN_RMW_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("mrn_rmw data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int mrn_rmw_cleanup(struct test *test) {
    auto *data = static_cast<MrnRmwData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_rmw, "Simple test that computes add/sub/xor/and of two source arrays and copies it to a destination array using multiple pairs of mrn store-load")
    .test_init = mrn_rmw_init,
    .test_run = mrn_rmw_run,
    .test_cleanup = mrn_rmw_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
