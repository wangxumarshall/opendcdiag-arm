/*
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include "sandstone_kvm.h"

#ifdef __linux__

int kvm_generic_run(struct test *test, int cpu)
{
    return EXIT_FAILURE;
}

int kvm_generic_init(struct test *t)
{
    /* This generic path is used on architectures where the framework has no
     * KVM guest backend (e.g. AArch64). The x86-64 backend in
     * sysdeps/linux/kvm.c drives a small in-process KVM guest; an AArch64
     * counterpart would need the ARM KVM API (KVM_ARM_* vcpu/MPIDR/regs
     * setup) and is not yet implemented.
     *
     * Distinguish "the framework lacks an ARM KVM guest" from "this host
     * has no KVM at all" so the skip reason is accurate. */
    bool host_has_kvm = (access("/dev/kvm", F_OK) == 0);
    if (host_has_kvm) {
        log_skip(DeviceNotConfiguredSkipCategory,
                 "KVM is available on this host but the framework's KVM guest "
                 "backend is only implemented for x86-64 (no AArch64 guest "
                 "support yet)");
    } else {
        log_skip(DeviceNotConfiguredSkipCategory,
                 "No /dev/kvm on this host; KVM not available");
    }
    return EXIT_SKIP;
}

int kvm_generic_cleanup(struct test *t)
{
    return EXIT_SUCCESS;
}

#else // !__linux__

int kvm_generic_init(struct test *)
{
    log_skip(OsNotSupportedSkipCategory, "Not supported on this OS");
    return EXIT_SKIP;
}

int kvm_generic_run(struct test *test, int cpu)
{
    __builtin_unreachable();
}

int kvm_generic_cleanup(struct test *)
{
    return EXIT_SUCCESS;
}

#endif

initfunc group_kvm_init(void) __attribute__((nothrow));
initfunc group_kvm_init(void)
{
    return kvm_generic_init;
}
