# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0
#
# ARM64-native NEON and crypto (AES) tests. These are self-contained
# aarch64-only tests using <arm_neon.h>; they are skipped (no-op) on x86-64
# because the entire subdir is entered only under the aarch64 guard below.
# (The x86-64 counterpart of crypto coverage is the OpenSSL SHA test under
# tests/cpu/openssl, gated separately by ssl_link_type.)

if host_machine.cpu_family() == 'aarch64'
    # crypto_test.cpp uses vaeseq_u8 / vaesmcq_u8, which require the +crypto
    # target; neon_test.cpp uses baseline armv8.1-A NEON. A single march covers
    # both files.
    arm64_cpp_flags = [
        '-march=armv8.1-a+crc+crypto',
    ]

    arm64_tests = files(
        'neon_test.cpp',
        'crypto_test.cpp',
        'sdc_test.cpp',
    )

    arm64_tests_lib = static_library(
        'tests_arm64',
        arm64_tests,
        build_by_default: false,
        include_directories: [
            framework_incdir,
        ],
        dependencies: [
            boost_dep,
        ],
        cpp_args: [
            debug_c_flags,
            default_cpp_warn,
            default_cpp_flags,
            march_generic_flags,
            arm64_cpp_flags,
        ],
    )

    sandstone_tests += [
        arm64_tests_lib,
    ]
endif
