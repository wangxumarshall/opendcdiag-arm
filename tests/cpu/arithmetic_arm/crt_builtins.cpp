/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b crt_builtins
 * @parblock
 * Stress the integer ALU by repeatedly executing the LLVM
 * compiler-rt soft-float builtins (__addsf3 / __mulsf3 / __divsf3)
 * which implement IEEE-754 single-precision arithmetic purely in
 * integer operations.  A table of random float operands is generated
 * at init and the hardware-computed result (using the native FPU) is
 * stored as the golden.  During the run the same operations are
 * re-evaluated through the compiler-rt integer implementations and
 * compared bit-exactly against the golden.  Because the soft-float
 * routines touch many integer ALU resources (shifts, adds, compares)
 * while producing a deterministic result, any mismatch indicates a
 * silent corruption in the integer execution path.
 * @endparblock
 */

#include <sandstone.h>

/* compiler-rt soft-float builtins: IEEE-754 implemented in integer ALU.
 * Declared extern "C" so the linker binds to libclang_rt.builtins. */
extern "C" {
    float __addsf3(float, float);
    float __subsf3(float, float);
    float __mulsf3(float, float);
    float __divsf3(float, float);
}

#define CRT_COUNT (1 << 12)   /* 4096 operand pairs */

namespace {
struct crt_data {
    float *a;
    float *b;
    float *golden_add;
    float *golden_mul;
    float *golden_div;
};
}

#define CAST(_x) static_cast<struct crt_data *>(_x)

static int crt_builtins_init(struct test *test)
{
    crt_data *data = new crt_data;
    data->a          = (float *)aligned_alloc_safe(64, CRT_COUNT * sizeof(float));
    data->b          = (float *)aligned_alloc_safe(64, CRT_COUNT * sizeof(float));
    data->golden_add = (float *)aligned_alloc_safe(64, CRT_COUNT * sizeof(float));
    data->golden_mul = (float *)aligned_alloc_safe(64, CRT_COUNT * sizeof(float));
    data->golden_div = (float *)aligned_alloc_safe(64, CRT_COUNT * sizeof(float));

    for (size_t i = 0; i < CRT_COUNT; ++i) {
        /* avoid divisors near zero: pick b from a safe non-zero range */
        data->a[i] = (float)((int32_t)random32()) / (float)(1u << 24);
        float bv;
        do { bv = (float)((int32_t)random32()) / (float)(1u << 24); }
        while (bv > -0.5f && bv < 0.5f);
        data->b[i] = bv;
        /* golden computed once on the hardware FPU */
        data->golden_add[i] = data->a[i] + data->b[i];
        data->golden_mul[i] = data->a[i] * data->b[i];
        data->golden_div[i] = data->a[i] / data->b[i];
    }
    test->data = data;
    return EXIT_SUCCESS;
}

static int crt_builtins_run(struct test *test, int cpu)
{
    auto data = CAST(test->data);
    do {
        for (size_t i = 0; i < CRT_COUNT; ++i) {
            float add = __addsf3(data->a[i], data->b[i]);
            float mul = __mulsf3(data->a[i], data->b[i]);
            float div = __divsf3(data->a[i], data->b[i]);
            if (memcmp(&add, &data->golden_add[i], sizeof(float)) != 0) {
                report_fail_msg("crt __addsf3 mismatch at %zu: 0x%08x vs 0x%08x",
                                i, *(unsigned int *)&add,
                                *(unsigned int *)&data->golden_add[i]);
            }
            if (memcmp(&mul, &data->golden_mul[i], sizeof(float)) != 0) {
                report_fail_msg("crt __mulsf3 mismatch at %zu: 0x%08x vs 0x%08x",
                                i, *(unsigned int *)&mul,
                                *(unsigned int *)&data->golden_mul[i]);
            }
            if (memcmp(&div, &data->golden_div[i], sizeof(float)) != 0) {
                report_fail_msg("crt __divsf3 mismatch at %zu: 0x%08x vs 0x%08x",
                                i, *(unsigned int *)&div,
                                *(unsigned int *)&data->golden_div[i]);
            }
        }
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int crt_builtins_cleanup(struct test *test)
{
    crt_data *data = CAST(test->data);
    free(data->a);
    free(data->b);
    free(data->golden_add);
    free(data->golden_mul);
    free(data->golden_div);
    delete data;
    return EXIT_SUCCESS;
}

DECLARE_TEST(crt_builtins, "compiler-rt soft-float builtins (__addsf3/__mulsf3/__divsf3) vs golden")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = crt_builtins_init,
  .test_run = crt_builtins_run,
  .test_cleanup = crt_builtins_cleanup,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
