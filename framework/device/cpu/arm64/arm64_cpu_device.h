/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARM64_CPU_DEVICE_H
#define ARM64_CPU_DEVICE_H

#include "cpu_device.h"

struct arm64_cpu_info_t
{
    int cpu_number;
    uint64_t microcode;

    int16_t thread_id;
    int16_t core_id;
    int16_t cluster_id;
    int16_t numa_id;
    int16_t package_id;

    uint64_t midr_el1;
    uint64_t id_aa64pfr0;
    uint64_t id_aa64pfr1;

    struct cache_info_t cache[3];

    NativeCoreType native_core_type;
};

typedef struct arm64_cpu_info_t device_info_t;

extern struct arm64_cpu_info_t *device_info;

#endif // ARM64_CPU_DEVICE_H
