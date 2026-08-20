/**
 * @file
 *
 * @copyright
 * Copyright 2026 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b eigen_svd_cdouble_sve
 * @parblock
 * This is the ARM64 SVE-backed counterpart of @ref eigen_svd_cdouble.
 *
 * It runs exactly the same divide-and-conquer bi-diagonalization SVD
 * (Eigen::BDCSVD) on a complex double input matrix, but the Eigen code is
 * instantiated against the SVE packet backend by compiling this translation
 * unit with `-DEIGEN_ARM64_USE_SVE` and a `-march=...+sve` target, and the
 * Eigen namespace is renamed (`-DEigen=EigenSVE`) so it links cleanly
 * beside the NEON baseline build.
 *
 * Because SVE instructions are baked into the object, the test probes for
 * SVE availability at init time using plain C (no SVE instructions) and
 * reports a clean CpuNotSupported skip when the running CPU has no SVE
 * (e.g. Kunpeng 920). On SVE-capable hardware (e.g. Kunpeng 930) the test
 * runs and stresses the SVE FMA/vector units for SDC detection.
 *
 * @note This test requires at least 2 threads to run.
 * @endparblock
 */

#include "sandstone_eigen_common.h"

#if defined(__aarch64__)

#include <sys/auxv.h>
#include <asm/hwcap.h>

using namespace Eigen;        /* renamed to EigenSVE via -DEigen=EigenSVE */
typedef Matrix<std::complex<double>, Dynamic, Dynamic> Mat;
typedef Eigen::BDCSVD<Mat> SVD;

#define M_DIM 300               // matches eigen_svd_cdouble

using eigen_svd_cdouble_sve_test = EigenSVDTest<SVD, M_DIM>;

/*
 * Probe SVE availability using only the vDSO / getauxval, which executes no
 * SVE instructions itself. We must not let any SVE-instrumented Eigen code
 * run on a CPU without SVE (it would SIGILL), so this is done before
 * delegating to the Eigen-backed init.
 */
static int sve_probe_and_init(struct test *test)
{
    unsigned long hwcap = getauxval(AT_HWCAP);
    if ((hwcap & HWCAP_SVE) == 0) {
        log_skip(CpuNotSupportedSkipCategory,
                 "ARM64 SVE not available on this CPU; "
                 "eigen_svd_cdouble_sve requires SVE (e.g. Kunpeng 930)");
        return EXIT_SKIP;
    }
    return eigen_svd_cdouble_sve_test::init(test);
}

DECLARE_TEST(eigen_svd_cdouble_sve, "Eigen SVD complex<double> (ARM64 SVE vector backend); counterpart of eigen_svd_cdouble")
  .groups = DECLARE_TEST_GROUPS(&group_math),
  .test_init = sve_probe_and_init,
  .test_run = eigen_svd_cdouble_sve_test::run,
  .test_cleanup = eigen_svd_cdouble_sve_test::cleanup,
  .minimum_cpu = 0,
  .fracture_loop_count = 5,
  .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST

#endif // __aarch64__
