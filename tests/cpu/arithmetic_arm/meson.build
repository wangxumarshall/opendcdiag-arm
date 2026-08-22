# Copyright 2022 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0
#
# arithmetic_arm: tests that exercise ARM-optimised third-party libraries
# (GMP / Arm Compute Library / compiler-rt) to catch silent data corruption
# in the integer, floating-point and memory subsystems on ARM64 (Kunpeng 920).
#
# These tests are ARM-native: GMP, ACL and compiler-rt soft-float builtins are
# consumed on aarch64, and the whole subdir is entered only under the aarch64
# guard in tests/cpu/meson.build (see subdir('arithmetic_arm') there). On x86-64
# the sources are not compiled at all, so x86 behaviour is untouched.
#
# Dependency availability on this host (verified):
#   * GMP  .................. system package (/usr/include/gmp.h, /usr/lib64/libgmp.so)
#   * Arm Compute Library ... libarm_compute.so at /usr/lib64; headers under the
#                             third-party arm-opt-install tree (no /usr/include/arm_compute).
#   * compiler-rt builtins .. /usr/lib/clang/17/lib/aarch64-openEuler-linux-gnu/
#                             libclang_rt.builtins.a (soft-float __addsf3 et al.)
#
# The Arm Optimized Routines (libmathlib.a / libstringlib.a) static archives are
# NOT available on this host (only the .so versions ship, and the reference port
# links them statically to avoid a runtime GLIBC_2.38 dependency). The 3 AOR
# tests (aor_math.cpp, aor_string.cpp, cbw_arm.cpp) are therefore excluded
# below — they are listed as comments and not built, so there is no link error.
#
# Test IDs use an `_arm` suffix (adcx_arm, adox_arm, adcxlong_arm,
# adcx_adox_interleaved_arm) and are DISTINCT from the already-ported
# tests/cpu/arithmetic IDs (adcx, adox, adcxlong, adcx_adox_interleaved — no
# suffix, which use __int128). The _arm variants use GMP as the golden
# reference; both sets coexist without ID clash.

arm_cpp_flags = [
    '-march=armv8.1-a+crc+crypto',
]

# Filesystem module for the optional-dep existence checks below.
fs = import('fs')

# ---------------------------------------------------------------------------
# GMP: system package. Prefer pkg-config, fall back to find_library.
# ---------------------------------------------------------------------------
gmp_dep = dependency('gmp', static : dep_static, required : false)
if not gmp_dep.found()
    gmp_lib = cc.find_library('gmp', required : false, static : dep_static)
    gmp_dep = declare_dependency(
        compile_args : ['-isystem', '/usr/include'],
        dependencies : [gmp_lib],
    )
endif

# ---------------------------------------------------------------------------
# Arm Compute Library: libarm_compute.so lives in /usr/lib64 (default linker
# search path); the C++ tensor/operator headers ship only under a
# third-party arm-opt-install tree (NOT packaged in openEuler RPMs, so absent
# in a clean openEuler container). The ACL test build is therefore optional:
# it is enabled only when the headers are actually present (the developer's
# machine via the acl_incdir option, or the ACL_INCDIR env var). In a plain
# openEuler container the ACL tests are skipped — they are non-default-built
# static_library()s, so their absence does not break the main opendcdiag build.
# ---------------------------------------------------------------------------
acl_incdir = get_option('acl_incdir')
if acl_incdir == ''
    # Fall back to the ACL_INCDIR env var if set, else the historical
    # hard-coded host path (developer machine). run_command reads the env at
    # configure time (works on meson >=0.47).
    acl_incdir_env = run_command(
        find_program('sh'), '-c', 'echo "${ACL_INCDIR:-}"',
        check: false,
    ).stdout().strip()
    if acl_incdir_env != ''
        acl_incdir = acl_incdir_env
    else
        acl_incdir = '/home/sdc/root/arm64-sdc-fuzzing/third_party/arm-opt-install/include'
    endif
endif
acl_headers_present = fs.exists(acl_incdir / 'arm_compute/core/Types.h')
if acl_headers_present
    acl_dep = declare_dependency(
        compile_args : ['-isystem', acl_incdir],
        link_args    : ['-L/usr/lib64', '-larm_compute'],
    )
endif

# ---------------------------------------------------------------------------
# compiler-rt builtins: static soft-float integer implementation (__addsf3,
# __subsf3, __mulsf3, __divsf3 ...). -Wl,--whole-archive prevents the linker
# from dropping the soft-float symbols as "unused". The static archive is a
# clang-17 artifact that is NOT in the openEuler RPM trees, so this test is
# optional too: built only when the archive path exists.
# ---------------------------------------------------------------------------
crt_builtins_path = '/usr/lib/clang/17/lib/aarch64-openEuler-linux-gnu/libclang_rt.builtins.a'
crt_builtins_present = fs.exists(crt_builtins_path)
if crt_builtins_present
    crt_builtins_lib = declare_dependency(
        link_args : ['-Wl,--whole-archive',
                      crt_builtins_path,
                      '-Wl,--no-whole-archive'],
    )
endif

# ---------------------------------------------------------------------------
# Source grouping. The 10 dep-available tests (7 GMP + 2 ACL + 1 compiler-rt)
# are grouped by library so each static_library pulls only the deps it needs.
# The 3 AOR tests are intentionally excluded (see header comment).
# ---------------------------------------------------------------------------

# 7 GMP-backed tests: adcx_arm, adox_arm, adcxlong_arm, adcx_adox_interleaved_arm,
# bigint_mulx_arm, gmp_bigadd, gmp_bignum.
gmp_sources = files(
    'adcx_arm.cpp',
    'adcxlong_arm.cpp',
    'adox_arm.cpp',
    'adcx_adox_interleaved_arm.cpp',
    'bigint_mulx_arm.cpp',
    'gmp_bigadd.cpp',
    'gmp_bignum.cpp',
)

# 2 ACL-backed tests: acl_gemm (NEON GEMM), fisttp_arm (FCVTZS cast).
# NOTE: acl_gemm currently returns EXIT_SKIP from init/run — ACL's NEGEMM
# crashes (SIGSEGV @0x518) under sandstone's fork-per-iteration model until
# fork-safety is fixed; the test is built and linked so the symbol is present
# and reports a clean skip. fisttp_arm is a real ACL-backed test.
# Both are only built when the ACL headers are present (see acl_headers_present
# above); in a clean openEuler container without the third-party ACL tree,
# they are skipped rather than failing the build.
acl_sources = files(
    'acl_gemm.cpp',
    'fisttp_arm.cpp',
)

# 1 compiler-rt test: soft-float __addsf3/__mulsf3/__divsf3 vs golden.
# Only built when the clang-17 compiler-rt static archive is present.
crt_sources = files(
    'crt_builtins.cpp',
)

# Excluded — AOR libmathlib.a / libstringlib.a static archives are not present
# on this host, so these 3 tests would fail to link:
#   'aor_math.cpp',
#   'aor_string.cpp',
#   'cbw_arm.cpp',

# ---------------------------------------------------------------------------
# Per-library static libraries. Each is added to sandstone_tests so the test
# objects are linked into the final opendcdiag binary. build_by_default:false
# keeps the objects out of the default ninja target until the main binary pulls
# them in (matches the pattern used by tests_crc / tests_arm64).
# ---------------------------------------------------------------------------
gmp_tests_lib = static_library(
    'tests_arithmetic_arm_gmp',
    gmp_sources,
    build_by_default: false,
    include_directories: [
        framework_incdir,
        tests_common_incdirs,
    ],
    dependencies: [
        boost_dep,
        gmp_dep,
    ],
    cpp_args: [
        debug_c_flags,
        default_cpp_flags,
        default_cpp_warn,
        march_generic_flags,
        arm_cpp_flags,
    ],
)

acl_tests_lib = []
if acl_headers_present
    acl_tests_lib = static_library(
        'tests_arithmetic_arm_acl',
        acl_sources,
        build_by_default: false,
        include_directories: [
            framework_incdir,
            tests_common_incdirs,
        ],
        dependencies: [
            boost_dep,
            acl_dep,
        ],
        cpp_args: [
            debug_c_flags,
            default_cpp_flags,
            default_cpp_warn,
            march_generic_flags,
            arm_cpp_flags,
        ],
    )
endif

crt_tests_lib = []
if crt_builtins_present
    crt_tests_lib = static_library(
        'tests_arithmetic_arm_crt',
        crt_sources,
        build_by_default: false,
        include_directories: [
            framework_incdir,
            tests_common_incdirs,
        ],
        dependencies: [
            boost_dep,
            crt_builtins_lib,
        ],
        cpp_args: [
            debug_c_flags,
            default_cpp_flags,
            default_cpp_warn,
            march_generic_flags,
            arm_cpp_flags,
        ],
    )
endif

# Only the libraries that actually got built are added to the test set.
sandstone_tests += [gmp_tests_lib]
if acl_headers_present
    sandstone_tests += [acl_tests_lib]
endif
if crt_builtins_present
    sandstone_tests += [crt_tests_lib]
endif
