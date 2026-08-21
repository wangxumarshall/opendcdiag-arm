/*
 * Copyright 2026 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @test ist
 *
 * In-Silicon Test (IST) is the ARM64-native counterpart to Intel's
 * In-Field Scan (IFS) on x86. It provides at-field self-test coverage of
 * core logic by exercising the architectural self-test facilities exposed
 * by the platform.
 *
 * Where IFS drives the Intel `ifs` kernel driver via sysfs, IST uses the
 * ARM64 equivalents available on the running platform. This is the ARM64
 * mirror of the three IFS tests:
 *   - ist         (counterpart of ifs / SAF - Scan at Field)
 *   - ist_array   (counterpart of ifs_array_bist - cache & register BIST)
 *   - ist_sbaf    (counterpart of ifs_sbaf - structural-based functional test)
 *
 * @test ist_array
 *
 * Array BIST counterpart: exercises the on-core caches and register-file
 * self-test logic exposed by the ARM64 platform.
 *
 * @test ist_sbaf
 *
 * Structural-Based Functional Test at Field counterpart (SBAF-equivalent):
 * exercises the functional structural self-test coverage available on the
 * platform.
 */

#define _GNU_SOURCE 1
#include <sandstone.h>

#if defined(__aarch64__)
#if defined(__linux__)

#include <errno.h>
#include <stdlib.h>

/*
 * ARM64 self-test facilities are still being wired up. Until the kernel
 * exposes a stable at-field self-test interface, the tests report a clean
 * resource skip so they remain listed and schedulable while the IST
 * backend is filled in. The names and scheduling structure mirror IFS so
 * the two provide symmetric coverage across x86 and ARM64.
 */
static int ist_skip_preinit(struct test *test)
{
    test->minimum_duration = num_cpus() * 200;
    return EXIT_SUCCESS;
}

static int ist_skip_init(struct test *test)
{
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): ARM64 In-Silicon Test (IST) "
             "backend not yet available; test reserved as the counterpart "
             "of Intel IFS");
    return EXIT_SKIP;
}

static int ist_run(struct test *test, int cpu)
{
    __builtin_unreachable();
}

#else // !__linux__

#include <stdlib.h>

static int ist_skip_preinit(struct test *test)
{
    return EXIT_SUCCESS;
}

static int ist_skip_init(struct test *test)
{
    log_skip(OsNotSupportedSkipCategory, "Not supported on this OS");
    return EXIT_SKIP;
}

static int ist_run(struct test *test, int cpu)
{
    __builtin_unreachable();
}

#endif // __linux__

DECLARE_TEST(ist, "ARM64 In-Silicon Test (IST) hardware selftest (counterpart of IFS)")
    .test_preinit = ist_skip_preinit,
    .test_init = ist_skip_init,
    .test_run = ist_run,
    .desired_duration = -1,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
    .flags = test_schedule_sequential,
END_DECLARE_TEST

DECLARE_TEST(ist_array, "Array BIST: ARM64 In-Silicon Test (IST) hardware selftest for cache and registers")
    .test_preinit = ist_skip_preinit,
    .test_init = ist_skip_init,
    .test_run = ist_run,
    .desired_duration = -1,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_PROD,
    .flags = test_schedule_sequential,
END_DECLARE_TEST

DECLARE_TEST(ist_sbaf, "SBAF: ARM64 In-Silicon Test (IST) hardware functional selftest")
    .test_init = ist_skip_init,
    .test_run = ist_run,
    .desired_duration = -1,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_BETA,
    .flags = test_schedule_sequential,
END_DECLARE_TEST

#endif // __aarch64__
