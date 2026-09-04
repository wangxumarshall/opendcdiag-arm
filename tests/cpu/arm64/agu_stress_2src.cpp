/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b agu_stress_2src
 * @parblock
 * AGU throughput stress: 2 source loads + data-ALU + store/reload/store chain,
 * maximizing address-generation pressure (3 ldr + 2 str per element).
 * Rotation uses {add, eor, and, orr} (proposal variant; differs from
 * mrn_nuke_2src_alu's {add,sub,eor,and} — tests rotation-composition
 * robustness). Skeleton identical to mrn_nuke_2src_alu.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define AGU_STRESS_2SRC_COUNT 1024

struct AguStress2srcData {
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

static int agu_stress_2src_init(struct test *test) {
    auto *data = static_cast<AguStress2srcData *>(malloc(sizeof(AguStress2srcData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t));

    for (int i = 0; i < AGU_STRESS_2SRC_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] | data->srcB[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int agu_stress_2src_run(struct test *test, int cpu) {
    auto *data = static_cast<AguStress2srcData *>(test->data);

    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < AGU_STRESS_2SRC_COUNT; i++) {
            uint64_t a = data->srcA[i];
            uint64_t b = data->srcB[i];
            uint64_t res;

            switch (i % 4) {
                case 0: res = a + b; break;
                case 1: res = a ^ b; break;
                case 2: res = a & b; break;
                case 3: res = a | b; break;
            }

            store_qword(&temp[i], res);
            store_qword(&dst[i], load_qword(&temp[i]));
        }

        if (memcmp(dst, data->expected, AGU_STRESS_2SRC_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("agu_stress_2src data miscompare");
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int agu_stress_2src_cleanup(struct test *test) {
    auto *data = static_cast<AguStress2srcData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(agu_stress_2src, "AGU stress: 2-source load + rotating ALU {add,eor,and,orr} + store/reload/store")
    .test_init    = agu_stress_2src_init,
    .test_run     = agu_stress_2src_run,
    .test_cleanup = agu_stress_2src_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
