/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_cdouble
 * @parblock
 * This piece of code aims to stress test the FMA execution units of
 * the CPU, among others, by repetitively solving the singular value
 * decomposition problem on given input matrices, which involve a lot
 * of matrix multiplication operations underneath.
 *
 * The logic comes from the 3rd party library Eigen. The first thread
 * that gets to run computes a "golden value" of the results, those
 * being output matrices "U" and "V". Subsequent runs have results
 * contrasted to those golden values and, whenever they differ, an
 * error is flagged.
 *
 * This particular version of the Eigen SVD tests go for double
 * precision, complex numbers input matrices and the (divide and
 * conquer) bi-diagonalization SVD algorithm.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"

using namespace Eigen;

typedef Matrix < std::complex <double >, Dynamic, Dynamic > Mat;
typedef Eigen::BDCSVD < Mat > SVD;

#define M_DIM 300               // weird dim on purpose

using eigen_svd_cdouble_test = EigenSVDTest<SVD, M_DIM>;

/* On x86-64 the test is gated on AVX-512 (Eigen selects the AVX-512 packet
 * path); on other architectures (e.g. ARM64) there is no equivalent feature
 * flag, and the framework's base build already selects the native vector
 * backend (NEON), so the test runs unconditionally there. */
#if defined(__x86_64__)
#  define SVD_CDOUBLE_MINIMUM_CPU  cpu_skylake_avx512
#else
#  define SVD_CDOUBLE_MINIMUM_CPU  0
#endif

DECLARE_TEST(eigen_svd_cdouble, "Eigen SVD (Singular Value Decomposition) solving payload, which issues a bunch of matrix multiplies underneath, now operating on std::complex<double>")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = eigen_svd_cdouble_test::init,
  .test_run = eigen_svd_cdouble_test::run,
  .test_cleanup = eigen_svd_cdouble_test::cleanup,
  .minimum_cpu = SVD_CDOUBLE_MINIMUM_CPU,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

