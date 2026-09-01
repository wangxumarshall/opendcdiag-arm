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
# search path); the C++ tensor/operator headers ship only under a third-party
# tree on the build host. Configured via -Denable_acl=auto|enabled|disabled and
# -Dacl_incdir=/path (the header tree; empty = system default).
# On 22.03/20.03 the ACL runtime is built against a newer libstdc++ (GLIBCXX_3.4.29+)
# and won't link in the older toolchain; pass -Denable_acl=disabled there (the
# container-build.sh driver does this via -D, no source sed needed).
# When disabled (or auto + not found), acl_sources is empty and acl_dep is a
# no-op declare_dependency(), so acl_tests_lib is an empty static lib that pulls
# no ACL symbols — the 2 ACL tests simply aren't built.
# ---------------------------------------------------------------------------
acl_enable = get_option('enable_acl')
acl_incdir_opt = get_option('acl_incdir')
acl_lib = cc.find_library('arm_compute', dirs : ['/usr/lib64'], required : (acl_enable == 'enabled'))

# auto: found iff the library is present AND headers are reachable (either a
# user-specified -Dacl_incdir, or arm_compute/ in the system include path).
acl_found = acl_lib.found()
if acl_found and acl_incdir_opt == ''
  acl_found = cc.has_header('arm_compute/arm.h', required : false)
endif

acl_incdir = acl_incdir_opt != '' ? acl_incdir_opt : '/usr/include'
acl_sources = []
acl_dep = declare_dependency()
if acl_enable != 'disabled' and acl_found
  acl_dep = declare_dependency(
      compile_args : acl_incdir_opt != '' ? ['-isystem', acl_incdir] : [],
      link_args    : ['-L/usr/lib64', '-larm_compute'],
  )
  acl_sources = files(
      'acl_gemm.cpp',
      'fisttp_arm.cpp',
  )
endif

# ---------------------------------------------------------------------------
# compiler-rt builtins: static soft-float integer implementation (__addsf3,
# __subsf3, __mulsf3, __divsf3 ...). -Wl,--whole-archive prevents the linker
# from dropping the soft-float symbols as "unused".
# ---------------------------------------------------------------------------
crt_builtins_path = '/usr/lib/clang/17/lib/aarch64-openEuler-linux-gnu/libclang_rt.builtins.a'
crt_builtins_lib = declare_dependency(
    link_args : ['-Wl,--whole-archive',
                  crt_builtins_path,
                  '-Wl,--no-whole-archive'],
)

# ---------------------------------------------------------------------------
# Source grouping. The 10 dep-available tests (7 GMP + 2 ACL + 1 compiler-rt)
# are grouped by library so each static_library pulls only the deps it needs.
# The 3 AOR tests are intentionally excluded (see header comment).
# ---------------------------------------------------------------------------

# 8 GMP-backed tests: adcx_arm, adox_arm, adcxlong_arm, adcx_adox_interleaved_arm,
# bigint_mulx_arm, gmp_bigadd, gmp_bignum, operand_space_arm.
# operand_space_arm is the operand-space SDC stressor: it feeds the ARM64
# adder/carry-chain/multiplier a deterministic high-Hamming-distance operand
# table (0x0000...<->0xFFFF... alternating, walking-one) instead of uniform
# random, to force maximum per-cycle gate toggling (the "版图一.1 操作数空间"
# coverage dimension that uniform-random operands miss).
gmp_sources = files(
    'adcx_arm.cpp',
    'adcxlong_arm.cpp',
    'adox_arm.cpp',
    'adcx_adox_interleaved_arm.cpp',
    'bigint_mulx_arm.cpp',
    'gmp_bigadd.cpp',
    'gmp_bignum.cpp',
    'operand_space_arm.cpp',
)

# 2 ACL-backed tests: acl_gemm (NEON GEMM), fisttp_arm (FCVTZS cast).
# NOTE: acl_gemm currently returns EXIT_SKIP from init/run — ACL's NEGEMM
# crashes (SIGSEGV @0x518) under sandstone's fork-per-iteration model until
# fork-safety is fixed; the test is built and linked so the symbol is present
# and reports a clean skip. fisttp_arm is a real ACL-backed test.
# acl_sources is set above (empty when -Denable_acl=disabled or ACL not found).

# 1 compiler-rt test: soft-float __addsf3/__mulsf3/__divsf3 vs golden.
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
# objects are linked into the final sdcshield binary. build_by_default:false
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

sandstone_tests += [
    gmp_tests_lib,
    acl_tests_lib,
    crt_tests_lib,
]
