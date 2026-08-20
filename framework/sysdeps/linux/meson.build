# Copyright 2022 Intel Corporation.
# SPDX-License-Identifier: Apache-2.0

if (host_machine.cpu_family() == 'x86_64')
    framework_files += files(
        'msr.c',
    )
endif

# The interrupt monitor parses /proc/interrupts (x86 MCE/TRM) on x86-64
# and the EDAC RAS counters on AArch64; both are Linux-specific.
if (host_machine.cpu_family() == 'x86_64' or host_machine.cpu_family() == 'aarch64')
    framework_files += files(
        'interrupt_monitor.cpp',
    )
endif

if device_type == 'cpu'
    if host_machine.cpu_family() == 'x86_64'
        framework_files += files(
            'kvm.c',
        )
    else
        framework_files += files(
            '../generic/kvm.c',
        )
    endif
endif

framework_files += \
    files(
        'cpu_affinity.cpp',
        'memfpt.cpp',
        'physicaladdress.cpp',
    )

premain_files += files('malloc.cpp')

if get_option('cpp_link_args').contains('-static')
    ar = find_program('ar')
    objcopy = find_program('objcopy')

    # Need to extract malloc.o from libc.a and transform it
    # Step 1: find libc.a by asking the compiler
    libc_a = run_command(cc.cmd_array(), '-print-file-name=libc.a', check: true)

    # Step 2: run our extractor script
    glibc_malloc_o = custom_target(
        'glibc_malloc.o',
        input: [
            libc_a.stdout().strip(),
            files('malloc.cpp'),
        ],
        output: 'glibc_malloc.o',
        command: [
            shell,
            files('extract-malloc.sh'),
            'AR=' + ar.full_path(),
            'OBJCOPY=' + objcopy.full_path(),
            '--',
            '@INPUT@',
            '@OUTPUT@',
        ]
    )

    # Step 3: add this file to exta_objects list
    extra_objects += glibc_malloc_o
endif # if static
