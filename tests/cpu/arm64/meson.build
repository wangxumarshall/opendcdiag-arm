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
        # di/dt voltage-transient power virus ("版图三 瞬态故障动态激发").
        # Alternates NEON full-load bursts with yield stalls to induce voltage
        # droop and excite transient SDC on the SIMD/FMA path — the only
        # burst/stall power-virus in the suite. Built only on aarch64.
        'power_virus_dit.cpp',
        'sdc_test.cpp',
        # Out-of-order execution SDC stress — coverage-weak unit (OoO 56%).
        # Pointer-chasing dependency chain, carry-dependent mixed ALU+memory
        # chain, RW-hazard interleave, and long dependent ALU chains to stress
        # the scheduler/ROB/rename under serial dependencies — the replay/
        # rename surface no existing test targets. Built only on aarch64.
        'ooo_dep_chain_arm.cpp',
        # LSU store-buffer/load-forwarding SDC stress — coverage-weak unit
        # (LSU 54%). Same-size forward at widths 1/2/4/8/16B and offsets
        # including the cache-line boundary, partial-store forwarding/merging,
        # store-after-load hazard, 128B cross-line forward, and strided
        # same-line forward — the forwarding datapath no existing memory test
        # targets. Built only on aarch64.
        'lsu_store_forward_arm.cpp',
        # L2 cache coherency/split-access SDC stress — coverage-weak unit
        # (L2C 40%). Adds cross-64B-line exclusive ldxr/stxr RMW, store-to-load
        # forwarding across the line boundary, dc-civac writeback probe, and a
        # 2x-L2 thrash sweep — the coherency/snoop/split surfaces that
        # memcpy_l2_cache_size never touches. Built only on aarch64.
        'l2c_cross_cache_line_arm.cpp',
        # MMU/TLB/PTW SDC stress — the coverage-weakest unit (MMU 20%). Adds
        # page-color aliasing (one physical page, many VAs), dual-TLB-lookup
        # cross-page and cross-cache-line split accesses, and a large-ASID
        # pressure sweep, none of which mmu_stress_arm exercises. Built only
        # on aarch64.
        'mmu_split_tlb_arm.cpp',
        # Floating-point/SIMD Unit byte-exact SDC stress (FSU 80% — highest
        # weak unit, but with a real gap: fma.cpp compares with a 1e-6
        # tolerance, so a 1-bit FSU SDC passes. This test is byte-exact
        # throughout: FMA over the full IEEE-754 special-value table, cross-
        # cache-line 128B NEON data path, single<->double widen/narrow, and
        # a high-Hamming dependent FMA chain, all compared bit-exact against
        # libm. Built only on aarch64.
        'fsu_byteexact_arm.cpp',
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
