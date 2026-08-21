/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b gmp_bigadd
 * @parblock
 * Stress the integer adder / carry chain by repeatedly adding large
 * multi-precision integers via the GMP library.  Two random big
 * integers (4096-bit each) are generated at init by importing random
 * bytes and their sum is stored as a golden result.  During the run
 * the same addition is repeated and every limb is compared against
 * the golden.  A mismatch points at a silent corruption in the add /
 * carry pipeline.
 * @endparblock
 */

#include <sandstone.h>
#include <gmp.h>

#define GMP_BITS 4096
#define GMP_BYTELEN ((GMP_BITS) / 8)
#define GMP_BYTELENLIMBS(l) ((l) * sizeof(mp_limb_t))

namespace {
struct gmp_add_data {
    mpz_t a;
    mpz_t b;
    mpz_t golden;           /* a + b, computed once at init */
    mp_limb_t *golden_buf;
    size_t golden_size;
};
}

#define CAST(_x) static_cast<struct gmp_add_data *>(_x)

static void fill_random_bytes(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i += 4) {
        uint32_t r = random32();
        for (int j = 0; j < 4 && (i + j) < n; ++j)
            buf[i + j] = (uint8_t)(r >> (8 * j));
    }
}

static int gmp_bigadd_init(struct test *test)
{
    gmp_add_data *data = new gmp_add_data;
    mpz_init(data->a);
    mpz_init(data->b);
    mpz_init(data->golden);
    data->golden_buf = nullptr;
    data->golden_size = 0;

    uint8_t buf[GMP_BYTELEN];
    fill_random_bytes(buf, GMP_BYTELEN);
    mpz_import(data->a, GMP_BYTELEN, 1, 1, 1, 0, buf);
    fill_random_bytes(buf, GMP_BYTELEN);
    mpz_import(data->b, GMP_BYTELEN, 1, 1, 1, 0, buf);

    mpz_add(data->golden, data->a, data->b);
    data->golden_size = mpz_size(data->golden);
    data->golden_buf = (mp_limb_t *)aligned_alloc_safe(64,
                          GMP_BYTELENLIMBS(data->golden_size));
    for (size_t i = 0; i < data->golden_size; ++i)
        data->golden_buf[i] = mpz_getlimbn(data->golden, i);

    test->data = data;
    return EXIT_SUCCESS;
}

static int gmp_bigadd_run(struct test *test, int cpu)
{
    auto data = CAST(test->data);
    mpz_t result;
    mpz_init(result);

    do {
        mpz_add(result, data->a, data->b);
        size_t rsize = mpz_size(result);
        if (rsize != data->golden_size) {
            mpz_clear(result);
            report_fail_msg("GMP add produced unexpected size %zu vs %zu",
                            rsize, data->golden_size);
        }
        for (size_t i = 0; i < rsize; ++i) {
            mp_limb_t got = mpz_getlimbn(result, i);
            if (got != data->golden_buf[i]) {
                mpz_clear(result);
                report_fail_msg("GMP big-add limb %zu mismatch: 0x%llx vs 0x%llx",
                                i, (unsigned long long)got,
                                (unsigned long long)data->golden_buf[i]);
            }
        }
    } while (test_time_condition(test));

    mpz_clear(result);
    return EXIT_SUCCESS;
}

static int gmp_bigadd_cleanup(struct test *test)
{
    gmp_add_data *data = CAST(test->data);
    mpz_clear(data->a);
    mpz_clear(data->b);
    mpz_clear(data->golden);
    free(data->golden_buf);
    delete data;
    return EXIT_SUCCESS;
}

DECLARE_TEST(gmp_bigadd, "GMP large integer addition (4096-bit) vs golden result")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = gmp_bigadd_init,
  .test_run = gmp_bigadd_run,
  .test_cleanup = gmp_bigadd_cleanup,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
