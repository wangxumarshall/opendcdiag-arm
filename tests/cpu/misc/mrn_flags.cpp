/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_flags
 * @parblock
 * flips a list of numbers while modifying the data and branching based
 * on results. Uses bitwise NOT to flip values, checks if the result is
 * zero, and conditionally modifies the data to a magic value. Combines
 * ALU flag modification with conditional branching logic.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define MRN_FLAGS_COUNT 1024

struct MrnFlagsData {
    uint64_t *src;
    uint64_t *expected;
};

#if defined(__x86_64__) || defined(__i386__)

/*
 * x86 inline asm: bitwise NOT, then check Zero Flag (ZF). If ZF is set,
 * conditionally move a magic value. This tests ALU flags generation and
 * conditional branch/move logic.
 */
static inline uint64_t flip_and_branch(uint64_t val) {
    uint64_t res = val;
    uint64_t magic = 0xDEADBEEFDEADBEEFULL;
    __asm__ volatile (
        "notq %0\n\t"
        "testq %0, %0\n\t"
        "cmovzq %2, %0\n\t"
        : "+r"(res)
        : "r"(magic) // %2
    );
    return res;
}

#elif defined(__aarch64__)

/*
 * ARMv8 inline asm: MVN (bitwise NOT), then CMP to set flags, then CSEL
 * to conditionally select the magic value if the result was zero.
 */
static inline uint64_t flip_and_branch(uint64_t val) {
    uint64_t res = val;
    uint64_t magic = 0xDEADBEEFDEADBEEFULL;
    __asm__ volatile (
        "mvn %0, %0\n\t"
        "cmp %0, #0\n\t"
        "csel %0, %1, %0, eq\n\t"
        : "+r"(res)
        : "r"(magic)
    );
    return res;
}

#endif

static int mrn_flags_init(struct test *test) {
    auto *data = static_cast<MrnFlagsData *>(malloc(sizeof(MrnFlagsData)));
    data->src = static_cast<uint64_t *>(aligned_alloc(64, MRN_FLAGS_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_FLAGS_COUNT * sizeof(uint64_t)));

    memset_random(data->src, MRN_FLAGS_COUNT * sizeof(uint64_t));

    // Compute expected golden values
    for (int i = 0; i < MRN_FLAGS_COUNT; i++) {
        uint64_t flipped = ~data->src[i];
        data->expected[i] = (flipped == 0) ? 0xDEADBEEFDEADBEEFULL : flipped;
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_flags_run(struct test *test, int cpu) {
    auto *data = static_cast<MrnFlagsData *>(test->data);
    uint64_t *dst = static_cast<uint64_t *>(aligned_alloc(64, MRN_FLAGS_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < MRN_FLAGS_COUNT; i++) {
            dst[i] = flip_and_branch(data->src[i]);
        }

        if (memcmp(dst, data->expected, MRN_FLAGS_COUNT * sizeof(uint64_t)) != 0) {
            report_fail_msg("mrn_flags data miscompare");
        }
    }

    free(dst);
    return EXIT_SUCCESS;
}

static int mrn_flags_cleanup(struct test *test) {
    auto *data = static_cast<MrnFlagsData *>(test->data);
    if (data) {
        free(data->src);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_flags, "flips a list of numbers while modifying the data and branching based on results")
    .test_init = mrn_flags_init,
    .test_run = mrn_flags_run,
    .test_cleanup = mrn_flags_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
