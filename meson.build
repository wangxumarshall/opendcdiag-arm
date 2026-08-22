# Copyright 2022 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

project(
    'OpenDCDiag',
    ['c', 'cpp'],
    default_options: [
        'b_pie=true',
        'b_ndebug=if-release',
        'b_staticpic=true',
        'c_std=gnu17',
        # cpp_std is NOT set here; it is chosen dynamically below from the
        # cxx_compat option + detected gcc version (gnu++23 on gcc12+, gnu++20
        # on gcc10, gnu++17 below), then applied via add_project_arguments.
    ],
    # Lowered from >=1.3 to >=0.54 so the same build files work on older
    # openEuler LTS toolchains (20.03 ships meson 0.54.3, 22.03 ships 0.59.4).
    # The only >=0.56 call used (meson.project_source_root) is replaced by
    # meson.source_root() below, which is valid on 0.54 through 1.x; no
    # other 0.56+-only meson feature is used in the shared build files.
    # openEuler 24.03 (meson 1.3.1) satisfies this trivially — no regression.
    meson_version : '>=0.54',
)

cc = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')

bash = find_program('bash')
perl = find_program('perl')
python = find_program('python3')
shell = find_program('sh')

# ---- openEuler version + C++ standard detection ------------------------------
# The default CPU build path uses a handful of C++23 facilities (notably
# std::to_underlying). openEuler 24.03 (gcc 12.3) compiles gnu++23 natively;
# 22.03 (gcc 10) tops out at gnu++20; 20.03 (gcc 7) tops out at gnu++17 and
# lacks std::span entirely. Rather than force one std on everyone, detect the
# host openEuler major version and the gcc major version and pick the highest
# standard the toolchain supports. The framework/openeuler_compat.h header then
# polyfills the missing pieces under a guard macro, so the 24.03 build is
# byte-identical to before (no polyfills active, gnu++23 unchanged).
openeuler_detect = run_command(
    find_program('sh'),
    files('scripts/detect-os.sh'),
    check: false,
)
oe_fields = openeuler_detect.stdout().strip().split(' ')
oe_id = oe_fields.length() > 0 ? oe_fields[0] : ''
oe_version_id = oe_fields.length() > 1 ? oe_fields[1] : ''
oe_major_int_str = oe_fields.length() > 2 ? oe_fields[2] : '0'
oe_major_int = oe_major_int_str.to_int()
oe_sp = oe_fields.length() > 3 ? oe_fields[3] : ''
is_openeuler = oe_id == 'openeuler'

# gcc major version (e.g. "12.3.1" -> 12); used for the auto cpp_std pick.
cpp_ver_full = cpp.version()
cpp_ver_parts = cpp_ver_full.split('.')
cpp_major = cpp_ver_parts.length() > 0 ? cpp_ver_parts[0].to_int() : 0

cpp_compat_opt = get_option('cxx_compat')
if cpp_compat_opt == 'auto'
    if cpp_major >= 12
        cpp_std_resolved = 'gnu++23'
    elif cpp_major >= 10
        cpp_std_resolved = 'gnu++20'
    else
        cpp_std_resolved = 'gnu++17'
    endif
elif cpp_compat_opt == 'c++23'
    cpp_std_resolved = 'gnu++23'
elif cpp_compat_opt == 'c++20'
    cpp_std_resolved = 'gnu++20'
else
    cpp_std_resolved = 'gnu++17'
endif

# Apply the resolved std via -std on the command line. We deliberately do not
# set cpp_std in project(default_options:) so meson does not warn about the
# manual -std flag; the std is chosen dynamically from the detected gcc version.
add_project_arguments('-std=' + cpp_std_resolved, language: 'cpp')

# Numeric compat level consumed by framework/openeuler_compat.h:
#   23 -> native std::to_underlying / std::span available
#   20 -> std::span native, to_underlying polyfilled (gcc 10, gnu++20)
#   17 -> both polyfilled (gcc 7, gnu++17, no <span>)
if cpp_std_resolved == 'gnu++23'
    sandstone_cpp_compat = 23
elif cpp_std_resolved == 'gnu++20'
    sandstone_cpp_compat = 20
else
    sandstone_cpp_compat = 17
endif

add_project_arguments('-DSANDSTONE_CPP_COMPAT=@0@'.format(sandstone_cpp_compat),
                      language: ['c', 'cpp'])
if is_openeuler
    add_project_arguments('-DSANDSTONE_OPENEULER_MAJOR=@0@'.format(oe_major_int),
                          '-DSANDSTONE_OPENEULER_SP="@0@"'.format(oe_sp),
                          language: ['c', 'cpp'])
endif

summary('openEuler:', is_openeuler ? (oe_version_id + ' (' + oe_sp + ')') : 'no')
summary('gcc major:', cpp_major)
summary('C++ std (resolved):', cpp_std_resolved, section: 'Toolchain')
summary('C++ compat level:', sandstone_cpp_compat, section: 'Toolchain')

if cpp.get_id() != 'gcc' or cc.get_id() != 'gcc'
    warning(
        'We recommend building OpenDCDiag with the GNU Compiler Collection (gcc). Your build may fail or your executable may not behave as expected.',
    )
endif

if cpp.get_linker_id() != 'ld.bfd' or cc.get_linker_id() != 'ld.bfd'
    warning(
        'We recommend linking OpenDCDiag with the binutils linker (ld). Your build may fail or your executable may not behave as expected.',
    )
endif

generated_short_ids = configure_file(
    input: 'scripts/generate-short-ids.pl',
    output: 'sandstone_short_ids.h',
    command: [
        perl, files('scripts/generate-short-ids.pl'),
        '@OUTPUT@', 'tests',
    ]
)

device_type = get_option('device_type')

march_generic_flags = []
march_flags = []
march_base = get_option('march_base')
if march_base != ''
    march_flags += '-march=' + march_base
endif
if host_machine.cpu_family() == 'x86_64'
    if march_base == ''
        march_base = 'haswell'
    endif
    march_generic_flags = [
        '-march=core2',
        '-mno-sse3',
    ]
    march_flags = [
        '-march=' + march_base,
        '-mtune=skylake',
        '-maes',
        '-mpclmul',
        '-mno-rdrnd',
        '-falign-loops=32',
    ]
    if cc.has_argument('-Wa,-muse-unaligned-vector-move')
        march_flags += ['-Wa,-muse-unaligned-vector-move']
    elif target_machine.system() == 'windows' and cpp.get_id() == 'gcc'
        warning(
            'GCC for Windows is unable to generate 32-byte-aligned stacks necessary for AVX content. '
            + 'Please upgrade to a version of binutils that supports the option -muse-unaligned-vector-move (2.38 or later). '
            + 'The application will randomly crash at runtime otherwise.',
        )
    endif
endif

debug_nasm_flags = ''
if get_option('buildtype') == 'debug'
    debug_c_flags = [
        '-ggdb3',
        '-O0',
    ]
elif get_option('buildtype') == 'debugoptimized'
    debug_c_flags = [
        '-ggdb3',
        '-O3',
        '-D_FORTIFY_SOURCE=2',
    ]
else
    debug_c_flags = [
        '-O3',
        '-g',
        '-D_FORTIFY_SOURCE=2',
    ]
endif

if get_option('static_libstdcpp') == true or target_machine.system() == 'windows'
    add_project_link_arguments(
        [
            '-static-libstdc++',
        ],
        language: 'cpp',
    )
endif

nasm_system_flags = [
    '-felf64',
    '-Xgnu',
    '-DLINUX',
    '-D__LINUX__',
]
default_c_flags = []
default_cpp_flags = []

foreach ccflag : [
    '-fstack-clash-protection',
    '-fstack-protector-strong',
    '-fcf-protection=full',
    '-mcet-switch',
]
    if cc.has_argument(ccflag)
        add_project_arguments(ccflag, language: ['c', 'cpp'])
    endif
endforeach

foreach lflag : [
    '-Wl,-z,relro',
    '-Wl,-z,noexecstack',
]
    if cc.links('int main() { return 0; }', args: [lflag], name: lflag)
        add_project_link_arguments(lflag, language: ['c', 'cpp'])
    endif
endforeach

if target_machine.system() == 'windows'
    add_project_arguments(
        [
            '-D__STDC_FORMAT_MACROS',
            '-D__USE_MINGW_ANSI_STDIO',
            '-D_POSIX',
            '-D_WIN32_WINNT=0x0A00',
            '-D_NO_SYS_GUID_OPERATOR_EQ_',
            '-Wa,-mbig-obj',
            '-include', meson.current_source_dir()
            + '/framework/sysdeps/windows/win32_stdlib.h',
        ],
        language: ['c', 'cpp'],
    )
    add_project_link_arguments(
        [
            '-mconsole',
            '-Wl,--stack,8388608',
            '-static-libgcc',
            '-Wl,-static',
            '-lpthread',
            '-lm',
            '-lssp',
            '-Wl,-Bdynamic',
            '-lshlwapi',
            '-lntdll',
        ],
        language: ['c', 'cpp'],
    )
    nasm_system_flags = [
        '-fwin64',
        '-Xgnu',
        '-DWIN_ABI',
    ]
elif target_machine.system() == 'darwin'
    add_project_arguments(
        [
            '-D__unix__', # Why doesn't Apple think their OS is Unix?
            '-D_DARWIN_C_SOURCE',
        ],
        language: ['c', 'cpp'],
    )
    nasm_system_flags = ['-fmacho64', '-Xgnu', '-DAPPLE', '-D__APPLE__', '--prefix=_']
else
    if get_option('buildtype') == 'debug'
        debug_nasm_flags = '-gdwarf'
    endif
    if get_option('buildtype') == 'debug'
        debug_nasm_flags = '-gdwarf'
    endif
endif

default_c_flags += [
    '-DSANDSTONE',
    '-fno-associative-math',
    '-fno-asynchronous-unwind-tables',
]

default_cpp_flags += [
    '-DSANDSTONE',
    '-fno-associative-math',
    '-fvisibility-inlines-hidden',
]

if get_option('b_sanitize') != 'none'
    default_c_flags += [
        '-g',
        '-fno-omit-frame-pointer',
    ]
    default_cpp_flags += [
        '-g',
        '-fno-omit-frame-pointer',
    ]
endif

default_c_warn = [
    '-Wall',
    '-Wextra',
    '-Werror=format-security',
    '-Werror=incompatible-pointer-types',
    '-Werror=implicit-function-declaration',
    '-Werror=int-conversion',
    '-Wno-unused-parameter',
    '-Wno-sign-compare',
]
if cc.has_argument('-Werror=discarded-qualifiers')
    default_c_warn += '-Werror=discarded-qualifiers'
endif

default_cpp_warn = [
    '-Wall',
    '-Wextra',
    '-Wformat',
    '-Wformat-security',
    '-Wno-unused-parameter',
    '-Wno-sign-compare',
    '-Wno-missing-field-initializers',
    '-Wno-non-virtual-dtor',
]

# Subdirs can add target dependencies if they wish
target_deps = []

boost_dep = dependency('boost', version : '>=1.69', required : false)
# Check for required headers
# (Also accomodates distributions which do not package a pkg-config file for boost)
boost_has_hdrs = cpp.check_header('boost/algorithm/string.hpp') and cpp.check_header('boost/type_traits/is_complex.hpp')
has_boost = boost_dep.found() or boost_has_hdrs

if not has_boost
    assert(false, 'Could not find boost or its headers')
elif not boost_dep.found()
    boost_dep = []
endif

pthread_dep = dependency('threads')

dl_dep = []
gtest_dep = []
libatomic_dep = []
if target_machine.system() != 'windows'
    dl_dep = cc.find_library('dl')
    gtest_dep = dependency('gtest_main', required: false)
endif


# On aarch64, std::atomic<__int128> (used by the spinlock_stress_cmpxchg16b
# test for 128-bit CAS) lowers to calls to the libatomic runtime helpers
# (__atomic_load_16 / __atomic_store_16 / __atomic_compare_exchange_16).
# There is no "-mcx16" on aarch64 GCC to force them inline, so link libatomic
# on non-x86-64 hosts. x86-64 builds its 128-bit atomics inline via -mcx16-style
# LOCK CMPXCHG16B (enabled by march flags) and does not need this, so it is
# left untouched.
if host_machine.cpu_family() != 'x86_64'
    libatomic_dep = cc.find_library('atomic', required: false)
endif

if get_option('coverage') == true
    gcov_dep = cc.find_library('gcov')
    target_deps += [gcov_dep]
    default_c_flags += [
        '-fprofile-arcs',
        '-ftest-coverage',
    ]
    default_cpp_flags += [
        '-fprofile-arcs',
        '-ftest-coverage',
    ]
endif

target_deps += [
    boost_dep,
    dl_dep,
    libatomic_dep,
    pthread_dep,
]

top_incdir = include_directories('.')

# Subdirs add (static) libraries to these arrays for linking
sandstone_tests = []
sandstone_tests_link = []
sandstone_tests_link_whole = []

unittests_sources = []
unittests_deps = []
unittests_flags = []

unittests_deps += [
    gtest_dep,
    pthread_dep,
]

subdir('framework')
subdir('tests')

sandstone_tests_link_whole += [sandstone_tests]

executable(
    get_option('executable_name'),
    install: true,
    dependencies: target_deps,
    link_with: [
        sandstone_tests_link,
    ],
    link_whole: [
        framework_a,
        sandstone_tests_link_whole,
    ],
    include_directories: [
        framework_incdir,
    ],
    cpp_args: [
        default_cpp_warn,
        default_cpp_flags,
    ],
)

executable(
    'unittests',
    files(
        'framework/sandstone_unittests_utils.cpp',
        'framework/sysdeps/linux/malloc.cpp',
    ),
    unittests_sources,
    generated_files,
    dependencies: unittests_deps,
    include_directories: [
        framework_incdir,
    ],
    cpp_args: [
        '-march=haswell',
        debug_c_flags,
        default_cpp_warn,
        '-USANDSTONE',
        unittests_flags,
    ],
    build_by_default: false,
    native: true,
)

summary(
    {
        'Logging format': get_option('logging_format'),
        'Using -march': march_base,
        'Static libstdc++': get_option('static_libstdcpp')
        or target_machine.system() == 'windows',
        'Executable name': get_option('executable_name'),
        'Fallback executable name': get_option('fallback_exec'),
        'Device type:': device_type,
    },
    section: 'Configuration',
    bool_yn: true,
)
