/*
 * Copyright 2025 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ARM64_RAS_H
#define ARM64_RAS_H

#include "arm64_privilege.h"
#include <cstdint>

enum Arm64ErrorType
{
    ARM64_ERROR_NONE,
    ARM64_ERROR_UC,
    ARM64_ERROR_CE,
    ARM64_ERROR_DEFERRED,
    ARM64_ERROR_SEA,
    ARM64_ERROR_SEI
};

struct Arm64ErrorState
{
    Arm64ErrorType type;
    uint64_t fault_address;
    uint64_t syndrome;
    int source;
};

int arm64_ras_init(void);
void arm64_ras_handler(int sig, void *info, void *context);
int read_edac_errors(memory_error_stats *stats, int cpu);
bool check_apei_available(void);

#endif // ARM64_RAS_H
