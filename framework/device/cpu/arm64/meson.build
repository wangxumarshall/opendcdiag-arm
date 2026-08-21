# Copyright 2025 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

if host_machine.cpu_family() == 'aarch64'
    arm64_cpp_flags = [
        '-march=armv8.1-a+crc+crypto',
    ]

    arm64_files = files(
        'arm64_topology.cpp',
        'kunpeng920_ecc.cpp',
        'arm64_sdc_detect.cpp',
    )

    arm64_lib = static_library(
        'framework_arm64',
        arm64_files,
        build_by_default: false,
        include_directories: [
            framework_incdir,
        ],
        dependencies: [
            pthread_dep,
            dl_dep,
        ],
        cpp_args: [
            debug_c_flags,
            march_generic_flags,
            default_cpp_flags,
            default_cpp_warn,
            arm64_cpp_flags,
        ],
    )

    extra_objects += arm64_lib.extract_all_objects(recursive: false)
endif
