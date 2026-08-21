/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_reloaded
 * @parblock
 * Initiates 1-byte, 4-byte and 8-byte MRN store-load pairs. Tests the
 * memory renaming / store-to-load forwarding logic across different
 * data access granularities.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define MRN_RELOADED_COUNT 1024

struct MrnReloadedData {
    uint8_t  *src8;
    uint32_t *src32;
    uint64_t *src64;
};

#if defined(__x86_64__) || defined(__i386__)

static inline void store_byte(void *addr, uint8_t val) {
    __asm__ volatile ("movb %1, %0" : "=m"(*reinterpret_cast<uint8_t*>(addr)) : "r"(val) : "memory");
}
static inline uint8_t load_byte(const void *addr) {
    uint8_t res;
    __asm__ volatile ("movb %1, %0" : "=r"(res) : "m"(*reinterpret_cast<const uint8_t*>(addr)) : "memory");
    return res;
}
static inline void store_dword(void *addr, uint32_t val) {
    __asm__ volatile ("movl %1, %0" : "=m"(*reinterpret_cast<uint32_t*>(addr)) : "r"(val) : "memory");
}
static inline uint32_t load_dword(const void *addr) {
    uint32_t res;
    __asm__ volatile ("movl %1, %0" : "=r"(res) : "m"(*reinterpret_cast<const uint32_t*>(addr)) : "memory");
    return res;
}
static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("movq %1, %0" : "=m"(*reinterpret_cast<uint64_t*>(addr)) : "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("movq %1, %0" : "=r"(res) : "m"(*reinterpret_cast<const uint64_t*>(addr)) : "memory");
    return res;
}

#elif defined(__aarch64__)

static inline void store_byte(void *addr, uint8_t val) {
    __asm__ volatile ("strb %w1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint8_t load_byte(const void *addr) {
    uint8_t res;
    __asm__ volatile ("ldrb %w0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
static inline void store_dword(void *addr, uint32_t val) {
    __asm__ volatile ("str %w1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint32_t load_dword(const void *addr) {
    uint32_t res;
    __asm__ volatile ("ldr %w0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}
static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int mrn_reloaded_init(struct test *test) {
    auto *data = static_cast<MrnReloadedData *>(malloc(sizeof(MrnReloadedData)));
    data->src8  = static_cast<uint8_t *>(aligned_alloc(64, MRN_RELOADED_COUNT * sizeof(uint8_t)));
    data->src32 = static_cast<uint32_t *>(aligned_alloc(64, MRN_RELOADED_COUNT * sizeof(uint32_t)));
    data->src64 = static_cast<uint64_t *>(aligned_alloc(64, MRN_RELOADED_COUNT * sizeof(uint64_t)));
    
    memset_random(data->src8, MRN_RELOADED_COUNT * sizeof(uint8_t));
    memset_random(data->src32, MRN_RELOADED_COUNT * sizeof(uint32_t));
    memset_random(data->src64, MRN_RELOADED_COUNT * sizeof(uint64_t));
    
    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_reloaded_run(struct test *test, int cpu) {
    auto *data = static_cast<MrnReloadedData *>(test->data);
    uint8_t  *temp8  = static_cast<uint8_t *>(aligned_alloc(64, MRN_RELOADED_COUNT * sizeof(uint8_t)));
    uint32_t *temp32 = static_cast<uint32_t *>(aligned_alloc(64, MRN_RELOADED_COUNT * sizeof(uint32_t)));
    uint64_t *temp64 = static_cast<uint64_t *>(aligned_alloc(64, MRN_RELOADED_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 8) {
        for (int i = 0; i < MRN_RELOADED_COUNT; i++) {
            store_byte(&temp8[i], data->src8[i]);
            uint8_t r8 = load_byte(&temp8[i]);
            if (r8 != data->src8[i]) report_fail_msg("mrn_reloaded 1-byte miscompare at %d", i);
        }
        for (int i = 0; i < MRN_RELOADED_COUNT; i++) {
            store_dword(&temp32[i], data->src32[i]);
            uint32_t r32 = load_dword(&temp32[i]);
            if (r32 != data->src32[i]) report_fail_msg("mrn_reloaded 4-byte miscompare at %d", i);
        }
        for (int i = 0; i < MRN_RELOADED_COUNT; i++) {
            store_qword(&temp64[i], data->src64[i]);
            uint64_t r64 = load_qword(&temp64[i]);
            if (r64 != data->src64[i]) report_fail_msg("mrn_reloaded 8-byte miscompare at %d", i);
        }
    }

    free(temp8);
    free(temp32);
    free(temp64);
    return EXIT_SUCCESS;
}

static int mrn_reloaded_cleanup(struct test *test) {
    auto *data = static_cast<MrnReloadedData *>(test->data);
    if (data) {
        free(data->src8);
        free(data->src32);
        free(data->src64);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_reloaded, "Initiates 1-byte, 4-byte and 8-byte MRN store-load pairs")
    .test_init = mrn_reloaded_init,
    .test_run = mrn_reloaded_run,
    .test_cleanup = mrn_reloaded_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
