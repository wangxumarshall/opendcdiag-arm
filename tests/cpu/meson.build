# Copyright 2022 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

dep_static = get_option('dependency_link') == 'static'

setmod = import('sourceset')

tests_config = configuration_data()
tests_set_base = setmod.source_set()
tests_set_hsw = setmod.source_set()
tests_set_skx = setmod.source_set()
tests_set_sve = setmod.source_set()

tests_common_c_args = [
    debug_c_flags,
    default_c_warn,
    default_c_flags,
    march_flags,
]

tests_common_cpp_args = [
    debug_c_flags,
    default_cpp_warn,
    default_cpp_flags,
    march_flags,
]

tests_common_incdirs = [
    framework_incdir,
]

if target_machine.cpu_family() != 'x86_64'
    # non-x86 Eigen does not get as much attention so this annoying
    # warning was left behind
    tests_common_cpp_args += '-Wno-deprecated-copy'
endif
if target_machine.system() == 'windows' and get_option('buildtype') == 'debug'
    # Force a little bit of optimization, otherwise we get:
    # Fatal error: tests/libtests_base.a.p/eigen_svd_svd_cdouble_noavx512.cpp.obj: file too big
    tests_common_cpp_args += '-Og'
endif

tests_set_base.add(
    files(
        'ifs/sandstone_ifs.c',
        'ifs/ifs.c',
        # Memory test suite (ported from the reference tree). The 7 NEON-backed
        # tests (vmovnt*, memcpy_variations_dyn) self-skip on non-aarch64 via
        # #ifdef __aarch64__ guards in init/run; the other 12 are arch-neutral
        # (std::memcpy / std::atomic / __sync_synchronize / portable inline asm).
        'memory/memcpy0.cpp',
        'memory/memcpy1.cpp',
        'memory/memcpy_l1d_size.cpp',
        'memory/memcpy_l2_cache_size.cpp',
        'memory/memcpy_l3_cache_size.cpp',
        'memory/memcpy_sem.cpp',
        'memory/memcpy_shuffle.cpp',
        'memory/memcpy_shuffle_v2.cpp',
        'memory/mem_disambiguation.cpp',
        'memory/mfence.cpp',
        'memory/mfence_pure.cpp',
        'memory/modified_sort.cpp',
        'memory/memcpy_variations_dyn.cpp',
        'memory/vmovnt1.cpp',
        'memory/vmovnt1_vex.cpp',
        'memory/vmovnt2.cpp',
        'memory/vmovnt2_vex.cpp',
        'memory/vmovnt3.cpp',
        'memory/vmovnt3_vex.cpp',
        # Cache coherency / stress tests. cachebounce is portable (std::atomic);
        # cache_stress_aggressor uses ARM NEON under #ifdef __aarch64__ guards and
        # returns a clean EXIT_SKIP on non-aarch64 (x86-64 untouched).
        'cache/cachebounce.cpp',
        'cache/cache_stress_aggressor.cpp',
    )
)

# Lock test suite (9 tests, std::atomic/std::atomic_flag-based, no x86
# intrinsics, no inline asm — portable C++ that builds on all arches).
tests_set_base.add(
    files(
        'lock/lockgen.cpp',
        'lock/lock_instr.cpp',
        'lock/lockless_cmpxchg.cpp',
        'lock/lockless_cmpxchg8b.cpp',
        'lock/lockless_cmpxchg16b.cpp',
        'lock/locks.cpp',
        'lock/locks_ccb.cpp',
        'lock/locks_ccb_xch_only.cpp',
        'lock/locks_xch_only.cpp',
    )
)

# ARM64-native In-Silicon Test (IST), the counterpart of Intel IFS on x86.
# Only built on aarch64; on x86 the IFS tests above provide the equivalent.
if host_machine.cpu_family() == 'aarch64'
    tests_set_base.add(
        files(
            'ist/ist.c',
        )
    )
endif

# Atomic / memory-ordering test suite. The NEON-backed variants
# (atomic_simd_128/256/512) guard their ARM SIMD usage with #ifdef
# __aarch64__ and return a clean placeholder skip on other arches
# (CLAUDE.md placeholder-test honesty). atomic_seq_cst and atomic_simd_128
# (which uses the portable __int128 extension, not NEON) build on all
# arches. The two *_unaligned SIMD tests are kept commented out, matching
# the reference tree's selection.
tests_set_base.add(
    files(
        'atomic/atomic_simd_128.cpp',
        #'atomic/atomic_simd_128_unaligned.cpp',
        'atomic/atomic_simd_256.cpp',
        #'atomic/atomic_simd_256_unaligned.cpp',
        'atomic/atomic_simd_512.cpp',
        'atomic/atomic_seq_cst.cpp',
    )
)

if framework_config.get('SANDSTONE_SSL_BUILD') == 1
    tests_set_base.add(
        when : crypto_dep,
        if_true: files(
            # Tests that depends on openssl
            'openssl/openssl_sha.cpp',
        )
    )
elif target_machine.system() == 'windows'
    tests_set_base.add(
        files(
            # Print skip in windows
            'openssl/openssl_sha.cpp',
        )
    )
endif

# On aarch64, system Eigen 3.3.x breaks on GCC 12+ (deprecated-enum-enum
# conversions turn into hard errors under the framework's -Wextra). The repo
# ships Eigen 5.0.0+ under third-part/eigen5 for this exact reason, so prefer
# it unconditionally on ARM64 and make the build self-contained (no need to
# export PKG_CONFIG_PATH=./third-part/eigen5). On x86-64 keep the historical
# behaviour of looking up the system eigen3 via pkg-config.
if host_machine.cpu_family() == 'aarch64'
    eigen3_dep = declare_dependency(
        include_directories: include_directories(
            '../../third-part/eigen5',
            is_system : true,
        ),
    )
else
    eigen3_dep = dependency('eigen3', include_type : 'system', static : dep_static)
endif
tests_set_base.add(
    when : eigen3_dep,
    if_true : files(
        'eigen_gemm/double14.cpp',
        'eigen_gemm/gemm_cdouble_dynamic_square.cpp',
        'eigen_gemm/gemm_cdouble_dynamic_square_dump.cpp',
        'eigen_gemm/gemm_double_dynamic_square.cpp',
        'eigen_gemm/gemm_double_dynamic_square_dump.cpp',
        'eigen_gemm/gemm_float_dynamic_square.cpp',
        'eigen_gemm/gemm_float_dynamic_square_dump.cpp',
        'eigen_sparse/eigen_sparse.cpp',
        'eigen_svd/svd.cpp',
        'eigen_svd/svd_cdouble_noavx512.cpp',
        'eigen_svd/svd_double.cpp',
        'eigen_svd/svd_fvectors.cpp',
        'eigen_svd_jacobi/svd.cpp',
        'eigen_svd_jacobi/svd_cdouble.cpp',
        'eigen_svd_jacobi/svd_double.cpp',
        'eigen_svd_jacobi/svd_fvectors.cpp',
    )
)

# On non-x86 (e.g. ARM64) eigen_svd_cdouble is not in the x86-only SKX set, so
# build the NEON baseline copy here. On x86 the SKX build block below compiles
# it with AVX-512 instead.
if host_machine.cpu_family() != 'x86_64'
    tests_set_base.add(
        when : eigen3_dep,
        if_true : files(
            'eigen_svd/svd_cdouble.cpp',
        )
    )
endif

tests_set_skx.add(
    when : eigen3_dep,
    if_true : files(
        'eigen_svd/svd_cdouble.cpp',
    )
)

# ARM64 SVE-backed SVD variant (counterpart of eigen_svd_cdouble). Compiled
# against Eigen's SVE packet backend and renamed Eigen namespace; only built
# on aarch64 (see the SVE build block below).
tests_set_sve.add(
    when : eigen3_dep,
    if_true : files(
        'eigen_svd/svd_cdouble_sve.cpp',
    )
)

zstd_dep = dependency('libzstd', static : dep_static)
tests_set_base.add(
    when : zstd_dep,
    if_true : files(
        'zstd/test.c',
    )
)

zlib_dep = dependency('zlib', static : dep_static)
tests_set_base.add(
    when : zlib_dep,
    if_true : files(
        'zlib/fuzz.c',
        'zlib/test.c',
    )
)

# port/arm64-fma-tests
# FMA test suite. Ported from the x86 AVX2/AVX-512 FMA tests to ARM NEON
# vfma. The "avx2"/"avx512" suffixes are inherited x86 names; on aarch64 the
# implementations use NEON float32x4_t/float64x2_t vfmaq. On non-aarch64
# (e.g. x86-64) every _run is compiled out via #ifdef __aarch64__ and returns
# EXIT_SKIP from init with "to be implemented (placeholder): ARM NEON FMA
# required", so the suite is built on all arches but only executes on ARM64.
tests_set_base.add(
    files(
        'fma/fma.cpp',
        'fma/fma_patterns_avx512_pd.cpp',
        'fma/fma_patterns_avx512_ps.cpp',
        'fma/fma_tail_avx2.cpp',
        'fma/fma_tail_avx512.cpp',
        'fma/fmatail_avx2.cpp',
        'fma/fmatail_avx512.cpp',
        'fma/fmatail_double_nested_avx2.cpp',
        'fma/fmatail_double_nested_avx512.cpp',
        'fma/fmatail_nested_avx2.cpp',
        'fma/fmatail_nested_avx512.cpp',
    )
)

# Spinlock stress suite. The tests are std::atomic-based (no x86 intrinsics,
# no arm_neon.h) and portable across both reference architectures: aarch64
# uses an ARM "yield" hint in the spin loop, x86-64 an "rep nop" (encoded as
# the "pause" mnemonic) hint. Both are guarded per-arch inside each source
# file, so the whole set builds unconditionally on tests_set_base.
tests_set_base.add(
    files(
        'spinlock/spinlock_array.cpp',
        'spinlock/spinlock_bank.cpp',
        'spinlock/spinlock_crosses_cacheline.cpp',
        'spinlock/spinlock_cross_socket.cpp',
        'spinlock/spinlock_mini.cpp',
        'spinlock/spinlock_randomsweep.cpp',
        'spinlock/spinlock_rmw_cmpxchg.cpp',
        'spinlock/spinlock_rmw_cmpxchg_pause.cpp',
        'spinlock/spinlock_rmw_xchg.cpp',
        'spinlock/spinlock_rmw_xchg_pause.cpp',
        'spinlock/spinlock_same_core.cpp',
        'spinlock/spinlock_stress.cpp',
        'spinlock/spinlock_stress_bts.cpp',
        'spinlock/spinlock_stress_cmpxchg.cpp',
        'spinlock/spinlock_stress_cmpxchg16b.cpp',
        'spinlock/spinlock_stress_cmpxchg_even_odd.cpp',
        'spinlock/spinlock_unaligned.cpp',
        'spinlock/spinlock_with_hle.cpp',
    )
)

tests_config_base = tests_set_base.apply(tests_config)

# libatomic provides the __atomic_load_16 / __atomic_store_16 /
# __atomic_compare_exchange_16 runtime symbols that the C++ standard library
# emits for std::atomic<unsigned __int128> (a 128-bit CAS, used by the
# lockless_cmpxchg16b test). AArch64's base ISA has no single 128-bit CAS
# instruction, so GCC lowers it to a libatomic call; x86-64 likewise emits
# these calls unless compiled with -mcx16. required:false keeps the build
# working on toolchains that inline 128-bit atomics natively.
atomic_lib = cc.find_library('atomic', required : false)

tests_base_a = static_library(
    'tests_base',
    sources : tests_config_base.sources(),
    build_by_default: false,
    include_directories : [
        tests_common_incdirs,
    ],
    dependencies: [
        tests_config_base.dependencies(),
        boost_dep,
        atomic_lib,
    ],
    c_args : [
        tests_common_c_args,
    ],
    cpp_args : [
        tests_common_cpp_args,
    ],
)

sandstone_tests += [
    tests_base_a,
]

if host_machine.cpu_family() == 'x86_64'
    march_hsw = [
        '-march=haswell',
        '-mtune=skylake',
    ]
    march_skx = [
        '-march=skylake-avx512',
        '-mtune=skylake-avx512',
    ]

    # Check if march_base has a higher CPU target, in which case
    # we should not downgrade
    if cc.has_define('__AVX512F__', args: tests_common_c_args)
        # We're already targetting AVX512
        march_skx = []
        march_hsw = []
    elif cc.has_define('__AVX2__', args: tests_common_c_args)
        march_hsw = []
    endif

    tests_config_hsw = tests_set_hsw.apply(tests_config)
    tests_hsw_sources = tests_config_hsw.sources()

    if tests_hsw_sources.length() > 0
        tests_hsw_a = static_library(
            'tests_hsw',
            sources : tests_hsw_sources,
            dependencies : tests_config_hsw.dependencies(),
            build_by_default: false,
            include_directories : [
                tests_common_incdirs,
            ],
            c_args : [
                tests_common_c_args,
                march_hsw,
            ],
            cpp_args : [
                tests_common_cpp_args,
                march_hsw,
                '-DEigen=EigenAVX2',
            ],
        )
        sandstone_tests += [tests_hsw_a]
    endif

    tests_config_skx = tests_set_skx.apply(tests_config)
    tests_skx_sources = tests_config_skx.sources()

    if tests_skx_sources.length() > 0
        tests_skx_a = static_library(
            'tests_skx',
            sources : tests_skx_sources,
            build_by_default: false,
            include_directories : [
                tests_common_incdirs,
            ],
            dependencies: [
                tests_config_skx.dependencies(),
                boost_dep,
            ],
            c_args : [
                tests_common_c_args,
                march_skx,
            ],
            cpp_args : [
                tests_common_cpp_args,
                march_skx,
                '-DEigen=EigenAVX512',
            ],
        )
        sandstone_tests += [tests_skx_a]
    endif

    unittests_sources += files(
        'ifs/unit/ifs_unit_utils.cpp',
        'ifs/unit/ifs_unittests.cpp',
        )
endif

# ARM64 SVE build block: mirror of the x86 SKX/HSW blocks above. Compiles the
# SVD SVE variant against Eigen's SVE packet backend (-DEIGEN_ARM64_USE_SVE)
# with a fixed vector length, and renames the Eigen namespace so it links
# beside the NEON baseline without symbol clashes.
if host_machine.cpu_family() == 'aarch64'
    march_sve = [
        '-march=armv8.2-a+sve',
        '-msve-vector-bits=128',
    ]

    tests_config_sve = tests_set_sve.apply(tests_config)
    tests_sve_sources = tests_config_sve.sources()

    if tests_sve_sources.length() > 0
        tests_sve_a = static_library(
            'tests_sve',
            sources : tests_sve_sources,
            dependencies: [
                tests_config_sve.dependencies(),
                boost_dep,
            ],
            build_by_default: false,
            include_directories: [
                tests_common_incdirs,
            ],
            c_args : [
                tests_common_c_args,
                march_sve,
            ],
            cpp_args : [
                tests_common_cpp_args,
                march_sve,
                '-DEIGEN_ARM64_USE_SVE',
                '-DEigen=EigenSVE',
            ],
        )
        sandstone_tests += [tests_sve_a]
    endif
endif

# ARM64-native NEON + crypto (AES) tests. The subdir guards the actual build
# behind `if host_machine.cpu_family() == 'aarch64'` so x86-64 is untouched.
subdir('arm64')
