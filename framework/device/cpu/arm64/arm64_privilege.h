/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARM64_PRIVILEGE_H
#define ARM64_PRIVILEGE_H

#include <cstdint>
#include <cstddef>

/*
 * ARM64-native hardware-access abstraction.
 *
 * This replaces the earlier x86-shaped hw_access_ops struct, which carried
 * read_msr()/write_msr() function pointers. An MSR is an x86 concept with no
 * userspace equivalent on AArch64 (the ARM register file is read via
 * MRS/MSR *system* instructions that are EL1+/EL2 privileged and not exposed
 * to userspace). Keeping MSR accessors in the ARM64 backend was x86 coupling
 * that leaked a non-existent interface and would mislead any caller into
 * thinking an ARM MSR read was available — it was not, and the accessors were
 * never defined (link-failure landmine).
 *
 * The ARM64-native RAS/ECC access paths are instead:
 *   - EDAC sysfs (/sys/devices/system/edac/mc/mcN/{ce,ue}_count),
 *     controller-wide (not per-CPU);
 *   - ACPI APEI/GHES error-injection/signaling;
 *   - the vendor HiSilicon RAS char driver (/dev/hisi_*_ras);
 * none of which goes through an MSR. The abstraction below models those paths
 * directly so it reflects what the silicon/firmware actually exposes on ARM.
 */

/* ARM64-native access method. No MSR, no x86 PERF_EVENT-MSR path. */
enum access_method_t
{
    ACCESS_USER_SPACE,
    ACCESS_SYSFS,          /* EDAC sysfs, controller-wide */
    ACCESS_PROCFS,
    ACCESS_ACPI,           /* APEI / GHES */
    ACCESS_VENDOR_DRIVER,  /* /dev/hisi_*_ras */
};

struct memory_error_stats
{
    uint64_t ce_count;
    uint64_t ue_count;
    bool ce_fatal;
    bool ue_fatal;
    char dimm_name[256];
    char dimm_id[256];
};

/*
 * ARM64-native hardware-access operations.
 *
 * read_ecc() returns controller-wide (not per-CPU) corrected/uncorrected
 * error counts into *stats; the cpu argument is retained only for DIMM
 * attribution in APEI/GHES layouts that key on a CPU, and is ignored by the
 * EDAC backend. setup_ras_monitoring() arms whatever signaling the backend
 * supports (e.g. registering for SError SIGBUS on a future APEI backend).
 */
struct hw_access_ops
{
    int (*init)(void);
    int (*read_ecc)(memory_error_stats *stats, int cpu);
    int (*setup_ras_monitoring)(void);
    void (*cleanup)(void);
    const char *backend_name;
};

#endif // ARM64_PRIVILEGE_H
