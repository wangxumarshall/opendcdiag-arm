/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARM64_PRIVILEGE_H
#define ARM64_PRIVILEGE_H

#include <cstdint>
#include <cstddef>

enum access_method_t
{
    ACCESS_USER_SPACE,
    ACCESS_SYSFS,
    ACCESS_PROCFS,
    ACCESS_PERF_EVENT,
    ACCESS_ACPI
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

struct hw_access_ops
{
    int (*init)(void);
    int (*read_msr)(uint32_t msr, uint64_t *value, int cpu);
    int (*write_msr)(uint32_t msr, uint64_t value, int cpu);
    int (*read_ecc)(memory_error_stats *stats, int cpu);
    int (*setup_ras_monitoring)(void);
    void (*cleanup)(void);
    const char *backend_name;
};

// NOTE: the free-function prototypes that were declared here
// (arm64_get_hw_access_ops, arm64_hw_init) had no definition anywhere in the
// tree — they would link-fail if called, and the hw_access_ops struct (a
// hardware-access abstraction shaped after x86 MSR access) is currently
// unused: ARM64 has no userspace MSR interface, and the working ECC/RAS
// path goes directly through Kunpeng920EccDetector. The struct definition is
// kept (it documents the abstraction) but the unimplemented accessors are
// removed to avoid misleading callers.

#endif // ARM64_PRIVILEGE_H
