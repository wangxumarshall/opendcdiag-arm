/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b zero_control_vec_vex_jit
 * @parblock
 * Test the top bits of a YMM register are zeroed by VEX instructions that specify the corresponding XMM register as a destination
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

static void *alloc_jit(size_t size) {
    void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return (mem == MAP_FAILED) ? nullptr : mem;
}

static void make_executable(void *mem, size_t size) {
    mprotect(mem, size, PROT_READ | PROT_EXEC);
}

static void flush_icache(void *mem, size_t size) {
#if defined(__aarch64__)
    __builtin___clear_cache(static_cast<char*>(mem), static_cast<char*>(mem) + size);
#endif
}

static void free_jit(void *mem, size_t size) {
    munmap(mem, size);
}

typedef void (*jit_func_t)(const void *input, void *output);

#if defined(__x86_64__)

static void emit_bytes(uint8_t **p, const uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        *(*p)++ = bytes[i];
    }
}

static void gen_vex_test(uint8_t *code) {
    uint8_t *p = code;
    const uint8_t vmovdqu64_load[] = {0x62, 0xF1, 0xFE, 0x48, 0x6F, 0x07}; // vmovdqu64 zmm0, [rdi]
    const uint8_t vpxor[]          = {0xC5, 0xF9, 0xEF, 0xC0};           // vpxor xmm0, xmm0, xmm0
    const uint8_t vmovdqu64_store[] = {0x62, 0xF1, 0xFE, 0x48, 0x7F, 0x06}; // vmovdqu64 [rsi], zmm0
    const uint8_t ret[]            = {0xC3};

    emit_bytes(&p, vmovdqu64_load, sizeof(vmovdqu64_load));
    emit_bytes(&p, vpxor, sizeof(vpxor));
    emit_bytes(&p, vmovdqu64_store, sizeof(vmovdqu64_store));
    emit_bytes(&p, ret, sizeof(ret));
}
#endif

struct ZeroControlData {
    jit_func_t func;
    void *jit_mem;
    size_t jit_size;
};

static int zero_control_vec_vex_jit_init(struct test *test) {
#if !defined(__x86_64__)
    log_skip(OsNotSupportedSkipCategory, "zero_control_vec_vex_jit: unsupported architecture (requires x86_64)");
    return EXIT_SKIP;
#else
    auto *data = static_cast<ZeroControlData *>(malloc(sizeof(ZeroControlData)));
    if (!data) return -ENOMEM;

    data->jit_size = 4096;
    data->jit_mem = alloc_jit(data->jit_size);
    if (!data->jit_mem) { free(data); return -ENOMEM; }

    gen_vex_test(static_cast<uint8_t *>(data->jit_mem));
    data->func = reinterpret_cast<jit_func_t>(data->jit_mem);

    make_executable(data->jit_mem, data->jit_size);
    flush_icache(data->jit_mem, data->jit_size);

    test->data = data;
    return EXIT_SUCCESS;
#endif
}

static int zero_control_vec_vex_jit_run(struct test *test, int cpu) {
#if !defined(__x86_64__)
    (void)test; (void)cpu;
    return EXIT_SUCCESS;
#else
    auto *data = static_cast<ZeroControlData *>(test->data);
    alignas(64) uint8_t input[64];
    alignas(64) uint8_t output[64];

    TEST_LOOP(test, 1 << 13) {
        memset_random(input, sizeof(input));
        // Ensure high bits are non-zero
        for (int i = 16; i < 64; ++i) input[i] |= 0xAA;

        data->func(input, output);

        // VEX.128 zeroes the top 128 bits of the YMM register.
        // On AVX-512 CPUs, it will also zero the upper 256 bits of ZMM.
        // We verify that the entire YMM register (lower 256 bits) is zeroed.
        bool ymm_ok = true;
        for (int i = 0; i < 32; ++i) {
            if (output[i] != 0) ymm_ok = false;
        }

        if (!ymm_ok) {
            report_fail_msg("VEX zero control failed: YMM high bits not zeroed");
        }
    }
    return EXIT_SUCCESS;
#endif
}

static int zero_control_vec_vex_jit_cleanup(struct test *test) {
#if defined(__x86_64__)
    auto *data = static_cast<ZeroControlData *>(test->data);
    if (data) {
        if (data->jit_mem) free_jit(data->jit_mem, data->jit_size);
        free(data);
    }
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(zero_control_vec_vex_jit, "Test the top bits of a YMM register are zeroed by VEX instructions that specify the corresponding XMM register as a destination")
    .test_init    = zero_control_vec_vex_jit_init,
    .test_run     = zero_control_vec_vex_jit_run,
    .test_cleanup = zero_control_vec_vex_jit_cleanup,
#if defined(__x86_64__)
    .minimum_cpu  = cpu_skylake_avx512,
#endif
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
