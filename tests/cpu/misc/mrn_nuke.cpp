/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_nuke
 * @parblock
 * Simple test that copies an input array from memory to an output array
 * via a temporary buffer and induces mrn nukes. Performs a tight sequence
 * of store-to-temp then load-from-temp operations to stress the CPU's
 * memory renaming logic and potentially trigger nukes (pipeline resets)
 * in the load/store queue.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define MRN_NUKE_COUNT 1024

struct MrnNukeData {
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

static int mrn_nuke_init(struct test *test) {
    auto *data = static_cast<MrnNukeData *>(malloc(sizeof(MrnNukeData)));
    data->src = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_COUNT * sizeof(uint64_t)));
    memset_random(data->src, MRN_NUKE_COUNT * sizeof(uint64_t));
    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_nuke_run(struct test *test, int cpu) {
    auto *data = static_cast<MrnNukeData *>(test->data);
    
    // Per-thread buffers to avoid data races
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_NUKE_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < MRN_NUKE_COUNT; i++) {
            // Store to temp buffer, then immediately load from it to dst.
            // This store-to-load forwarding dependency stresses MRN.
            store_qword(&temp[i], data->src[i]);
            uint64_t val = load_qword(&temp[i]);
            store_qword(&dst[i], val);
        }

        // Validate the entire array at the end of the loop iteration
        if (memcmp(dst, data->src, MRN_NUKE_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("mrn_nuke data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int mrn_nuke_cleanup(struct test *test) {
    auto *data = static_cast<MrnNukeData *>(test->data);
    if (data) {
        free(data->src);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_nuke, "Simple test that copies an input array from memory to an output array via a temporary buffer and induces mrn nukes")
    .test_init = mrn_nuke_init,
    .test_run = mrn_nuke_run,
    .test_cleanup = mrn_nuke_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
