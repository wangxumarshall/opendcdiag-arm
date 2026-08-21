/**
 * @file
 *
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b smi_count
 * This test is not really a test in the sense that it will never fail.  It is intended to simply
 * print differences in the number of SMI (System Management Interrupts) that have occurred since
 * either the beginning of the sandstone run or the last time this test itself has run
 */

#include "sandstone_p.h"

#if defined(__x86_64__)
#include "interrupt_monitor.hpp"

#include <cinttypes>
#include <vector>

namespace {
// we can use global variable, since write to this vector is done by single thread (initialize_smi_counts),
// and in smi_count_run it's read-only + each thread reads different memory location.
std::vector<uint64_t> smi_counts_start;

int initialize_smi_counts(struct test*)
{
    std::optional<uint64_t> v = InterruptMonitor::count_smi_events(device_info[0].cpu_number);
    if (!v) {
        log_skip(RuntimeSkipCategory, "Could not read msr");
        return EXIT_SKIP;
    }
    smi_counts_start.resize(thread_count());
    smi_counts_start[0] = *v;
    for (int i = 1; i < thread_count(); i++) {
        smi_counts_start[i] = InterruptMonitor::count_smi_events(device_info[i].cpu_number).value_or(0);
    }
    return EXIT_SUCCESS;
}

int smi_count_run(struct test *test, int thread)
{
    (void) test;

    if (int(smi_counts_start.size()) > thread) {
        int real_cpu_number = device_info[thread].cpu_number;
        auto initial_count = smi_counts_start[thread];
        auto current_count = InterruptMonitor::count_smi_events(real_cpu_number);
        if (current_count) {
            uint64_t difference = *current_count - initial_count;

            if (difference) {
                log_platform_message(SANDSTONE_LOG_INFO "SMI count difference detected: %" PRIu64 " new SMI detected on thread %d cpu_number %d\n",
                                     difference, thread, real_cpu_number);
            }
        }
    }
    return EXIT_SUCCESS;
}
} // end anonymous namespace

DECLARE_TEST(smi_count, "Counts SMI events")
    .test_preinit = initialize_smi_counts,
    .test_init = [](struct test *t) { return EXIT_SUCCESS; },
    .test_run = smi_count_run,
    .test_cleanup = initialize_smi_counts,
    .desired_duration = -1,
    .fracture_loop_count = -1,
    .quality_level = InterruptMonitor::InterruptMonitorWorks ? TEST_QUALITY_PROD : TEST_QUALITY_SKIP,
END_DECLARE_TEST

#else // !__x86_64__

/*
 * On x86-64 this test reads the IA32_SMI_COUNT MSR (0x34) via /dev/cpu/<n>/msr
 * to count System Management Interrupts during a run. ARM64 has no MSR
 * interface and no direct SMI equivalent; its analogues are the firmware/EL3
 * initiated interrupts and RAS events (SError, Fault, FIQ), which are not
 * exposed through a single per-CPU counter the way x86 exposes MSR 0x34.
 *
 * The test is therefore kept as a placeholder on non-x86 so it stays listed
 * and schedulable. It reports a clean skip ("to be implemented (placeholder)")
 * so the result is marked ignored rather than spuriously passing. When a
 * portable per-CPU firmware/RAS-interrupt counter becomes available (e.g.
 * via perf_event_open on the ARM PMU, or a sysfs attribute under
 * /sys/devices/system/cpu/...), fill in the three functions below following
 * the x86 structure above: a baseline snapshot in preinit, a per-thread
 * delta reported via log_platform_message() in run, and a re-snapshot in
 * cleanup.
 */

static int smi_count_preinit(struct test *test)
{
    (void) test;
    return EXIT_SUCCESS;
}

static int smi_count_init(struct test *test)
{
    (void) test;
    log_skip(TestResourceIssueSkipCategory,
             "to be implemented (placeholder): SMI counting requires a "
             "per-CPU firmware/RAS-interrupt counter not available on this "
             "architecture");
    return EXIT_SKIP;
}

static int smi_count_run(struct test *test, int thread)
{
    (void) test;
    (void) thread;
    __builtin_unreachable();    // init reports EXIT_SKIP, run never executes
}

static int smi_count_cleanup(struct test *test)
{
    (void) test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(smi_count, "Counts SMI events")
    .test_preinit = smi_count_preinit,
    .test_init = smi_count_init,
    .test_run = smi_count_run,
    .test_cleanup = smi_count_cleanup,
    .desired_duration = -1,
    .fracture_loop_count = -1,
    .quality_level = TEST_QUALITY_SKIP,
END_DECLARE_TEST

#endif // __x86_64__
