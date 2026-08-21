/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_gemm_float_dynamic_square_dump
 * @parblock
 * Dump-on-failure variant of eigen_gemm_float_dynamic_square. The FMA
 * hot path (x = lhs*rhs) is byte-for-byte identical to the original;
 * only the verification changes: instead of memcmp_or_fail over the whole
 * matrix, a cold element-wise compare loop locates the first mismatch and
 * dumps its row/col, golden, actual, IEEE-754 bit xor, and a per-byte
 * breakdown, so an SDC bit-flip in the product can be located. Mirrors the
 * movbe_dump / mrn_rmw_dump design: no extra hot-path reads or snapshots.
 *
 * @note Although the test should run fine on a single thread, it is
 * only expected to catch defects if run on at least 2 cores.
 * @endparblock
 */

#include <sandstone.h>

#include <Eigen/Core>
#include <cstdlib>
#include <cstdint>
#include <cstring>
using namespace Eigen;

// Matrix dimension. Historically hardcoded to 256. Now configurable via
// the GEMM_K_DIM environment variable (default 256 = original baseline) so
// the fault-propagation-vs-k-dimension hypothesis can be tested without
// touching random-generation / tolerance / report logic. Nothing else
// changes: Mat::Random seeding and report_fail behavior are untouched.
static int gemm_k_dim() {
    static int dim = -1;
    if (dim < 0) {
        int d = 256;
        if (const char *env = getenv("GEMM_K_DIM")) {
            int parsed = atoi(env);
            if (parsed > 0) d = parsed;
        }
        dim = d;
    }
    return dim;
}
#define M_DIM gemm_k_dim()

typedef Matrix < float, Dynamic, Dynamic > Mat;

namespace {
struct eigen_test_data {
    Mat lhs;
    Mat rhs;
    Mat prod;
};
}

#define CAST(_x) static_cast<struct eigen_test_data *>(_x)

static int eigen_gemm_float_dynamic_square_init(struct test *test) {
    test->data = new(eigen_test_data);
    try {
        CAST(test->data)->lhs = Mat::Random(M_DIM, M_DIM);
        CAST(test->data)->rhs = Mat::Random(M_DIM, M_DIM);
        CAST(test->data)->prod = CAST(test->data)->lhs * CAST(test->data)->rhs;
    } catch (...) {
        report_fail_msg("Exception on Eigen code, most probably OOM");
    }
    return EXIT_SUCCESS;
}

static int eigen_gemm_float_dynamic_square_run(struct test *test, int cpu) {
    do {
        auto testdata = CAST(test->data);
        Mat x;
        x = testdata->lhs * testdata->rhs;          // FMA hot path: unchanged

        const float *xd = x.data();
        const float *pd = testdata->prod.data();
        // Element-wise byte-exact compare (same semantics as the original
        // memcmp_or_fail). On the first mismatch, dump golden/actual + the
        // IEEE-754 bit xor and per-byte breakdown, then fail. All of this
        // is cold: the hot path only does the gemm + the compare branch.
        bool ok = true;
        for (int idx = 0; idx < M_DIM * M_DIM; ++idx) {
            if (xd[idx] != pd[idx]) {
                ok = false;
                uint32_t golden_bits, actual_bits;
                memcpy(&golden_bits, &pd[idx], sizeof(uint32_t));
                memcpy(&actual_bits, &xd[idx], sizeof(uint32_t));
                uint32_t xorv = golden_bits ^ actual_bits;
                int row = idx / M_DIM, col = idx % M_DIM;
                unsigned char *gb = (unsigned char *)&golden_bits;
                unsigned char *ab = (unsigned char *)&actual_bits;
                unsigned char *xb = (unsigned char *)&xorv;
                log_error("eigen_gemm_float_dump: miscompare at [%d,%d] (idx=%d): "
                          "golden=%g actual=%g | bits golden=0x%08X actual=0x%08X "
                          "xor=0x%08X (flipped bit(s)) | "
                          "bytes[b3..b0] golden=%02X%02X%02X%02X "
                          "actual=%02X%02X%02X%02X xor=%02X%02X%02X%02X",
                          row, col, idx,
                          (double)pd[idx], (double)xd[idx],
                          golden_bits, actual_bits, xorv,
                          gb[3], gb[2], gb[1], gb[0],
                          ab[3], ab[2], ab[1], ab[0],
                          xb[3], xb[2], xb[1], xb[0]);
                report_fail_msg("eigen_gemm_float_dump: product miscompare at [%d,%d]", row, col);
            }
        }
        (void)ok;
    } while (test_time_condition(test));
    return EXIT_SUCCESS;
}

static int eigen_gemm_float_dynamic_square_finish(struct test *test) {
    delete(CAST(test->data));
    return EXIT_SUCCESS;
}

DECLARE_TEST(eigen_gemm_float_dynamic_square_dump, "Eigen GEMM payload (float, dynamic, square) — dump variant records golden/actual/xor on miscompare")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_gemm_float_dynamic_square_init,
  .test_run = eigen_gemm_float_dynamic_square_run,
  .test_cleanup = eigen_gemm_float_dynamic_square_finish,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
