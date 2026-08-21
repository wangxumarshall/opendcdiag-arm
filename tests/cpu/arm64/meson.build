# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0
#
# ARM64-specific test directory. Currently this builds only the core-179 SDC
# reproducer fuzzing cases (tests/cpu/arm64/sdc_fuzzing/). Other ARM64 test
# modules (neon_test, crypto_test, ...) live in sibling worktrees and are
# reconciled here when those branches merge.

if host_machine.cpu_family() == 'aarch64'
    subdir('sdc_fuzzing')
endif
