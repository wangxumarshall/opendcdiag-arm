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

// NOTE: the free-function prototypes that were declared here
// (arm64_ras_init, arm64_ras_handler, read_edac_errors, check_apei_available)
// had no definition anywhere in the tree — they would link-fail if called.
// They are removed to avoid misleading callers. Kunpeng920EccDetector (in
// kunpeng920_ecc.cpp) provides the WORKING ECC/RAS reading via its own
// member functions (check_apei_available / read_edac_errors as members),
// which is what the arm64_sdc_* backend actually uses. A standalone ARM64
// RAS signal handler (SError/SEI via SIGBUS, APEI/GHES parsing) is future
// work — not declared here until it is implemented.

#endif // ARM64_RAS_H
