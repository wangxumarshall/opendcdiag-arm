# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

# Must be a configure_file, not custom_target, so it's created at
# meson time and thus before everything. On x86 the config is CPUID-leaf
# based (simd.conf / x86simd_generate.pl); on aarch64 it is HWCAP based
# (simd-arm.conf / armsimd_generate.pl). Both emit the same cpu_features.h
# contract (device_features_t, cpu_feature_*, features_string, x86_locators,
# x86_architectures) consumed by cpuid_internal.h and cpu_device.cpp.
if host_machine.cpu_family() == 'aarch64'
    simd_input = files('simd-arm.conf')
    simd_script = files('scripts/armsimd_generate.pl')
else
    simd_input = files('simd.conf')
    simd_script = files('scripts/x86simd_generate.pl')
endif
generated_files += configure_file(
    output: 'cpu_features.h',
    encoding: 'ascii',
    input: simd_input,
    capture: true,
    command: [
        perl,
        simd_script,
        '@INPUT@',
        '@OUTPUT@',
    ],
)

framework_files += files(
    'cpu_device.cpp',
    'topology.cpp',
    'logging_cpu.cpp',
    'slicing.cpp',
)

unittests_sources += files(
    'unit-tests/thermal_monitor_tests.cpp',
    'unit-tests/topology_tests.cpp',
    'slicing.cpp',
)

premain_files += files(
    'premain.cpp',
)
