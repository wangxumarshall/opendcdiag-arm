/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mite
 * @parblock
 * MITE switch workload adapted for ARMv8. Stresses decode and branch prediction.
 * Uses W0 and W1 registers explicitly.
 * Fixed: Uses register operands for large constants due to ARM64 immediate limitations.
 * @endparblock
 */

#include "sandstone.h"

#define MITE_ITERATIONS (1 << 9)

struct mite_data {
    uint32_t *input;
    uint64_t golden_checksum;
};

// Helper logic: (x + A) ^ B - C
// Note: Since constants don't fit in ARM immediate fields, we load them into W1.
// Define constants
#define CONST1 0xDEADBEEF
#define CONST2 0xC0FFEE
#define CONST3 0xF00D

static inline uint32_t process_value(uint32_t w0_val)
{
    // Force W0 to hold the value
    register uint32_t w0 asm("w0") = w0_val;
    
    // Force W1 to hold constants (Compiler will generate MOVZ/MOVK sequences or LDR)
    register uint32_t w1 asm("w1") = CONST1; 
    
    asm volatile (
        "add %w0, %w0, %w1;"
        : "+r"(w0) // Output in W0
        : "r"(w1)  // Input from W1
        : "cc"
    );

    w1 = CONST2;
    asm volatile (
        "eor %w0, %w0, %w1;"
        : "+r"(w0)
        : "r"(w1)
        : "cc"
    );

    w1 = CONST3;
    asm volatile (
        "sub %w0, %w0, %w1;"
        : "+r"(w0)
        : "r"(w1)
        : "cc"
    );
    
    w0_val = w0;

    // Branching logic
    if (w0_val & 1) {
        w0_val = w0_val * 13;
    } else {
        w0_val = w0_val * 17;
    }

    if ((w0_val >> 16) & 1) {
         w0_val += 1;
    }

    return w0_val;
}

static int mite_init(struct test *test)
{
    mite_data *data = (mite_data *)malloc(sizeof(mite_data));
    data->input = (uint32_t *)aligned_alloc_safe(64, MITE_ITERATIONS * sizeof(uint32_t));
    
    uint64_t sum = 0;
    for (size_t i = 0; i < MITE_ITERATIONS; ++i) {
        data->input[i] = random32();
        sum += process_value(data->input[i]);
    }
    data->golden_checksum = sum;

    test->data = data;
    return EXIT_SUCCESS;
}

static int mite_run(struct test *test, int cpu)
{
    mite_data *data = (mite_data *)test->data;

    TEST_LOOP(test, 1 << 10) {
        uint64_t checksum = 0;
        for (size_t i = 0; i < MITE_ITERATIONS; ++i) {
            uint32_t val = data->input[i];

            register uint32_t w0 asm("w0") = val;
            register uint32_t w1 asm("w1") = CONST1;
            
            asm volatile (
                "add %w0, %w0, %w1;"
                : "+r"(w0) : "r"(w1) : "cc"
            );

            w1 = CONST2;
            asm volatile (
                "eor %w0, %w0, %w1;"
                : "+r"(w0) : "r"(w1) : "cc"
            );

            w1 = CONST3;
            asm volatile (
                "sub %w0, %w0, %w1;"
                : "+r"(w0) : "r"(w1) : "cc"
            );
            
            val = w0;

            // Branching logic
            if (val & 1) {
                val = val * 13;
            } else {
                val = val * 17;
            }

            if ((val >> 16) & 1) {
                 val += 1;
            }

            checksum += val;
        }

        if (checksum != data->golden_checksum) {
            report_fail_msg("MITE: Checksum mismatch. Expected 0x%llx, got 0x%llx", 
                            (unsigned long long)data->golden_checksum, (unsigned long long)checksum);
        }
    }

    return EXIT_SUCCESS;
}

static int mite_cleanup(struct test *test)
{
    mite_data *data = (mite_data *)test->data;
    if (data) {
        free(data->input);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mite, "MITE switch workload - Add/sub in eax with memory barriers and jumps")
    .test_init = mite_init,
    .test_run = mite_run,
    .test_cleanup = mite_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
