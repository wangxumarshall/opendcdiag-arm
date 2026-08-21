/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mem_disambiguation
 * @parblock
 * stresses store to load forwarding. Performs many store-then-load
 * sequences at random offsets within a per-thread buffer, including
 * same-address forwarding and partial-overlap patterns, to stress the
 * CPU's memory disambiguation and store-to-load forwarding logic.
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>

#define DISAMBIG_BUFFER_SIZE   4096
#define DISAMBIG_PATTERN_COUNT  512

struct MemDisambigData {
    uint64_t *offsets;   /* random 8-byte-aligned offsets within buffer */
    uint64_t *values;    /* random values to store                       */
};

/* ------------------------------------------------------------------ */
/* Platform-specific store/load primitives                            */
/* ------------------------------------------------------------------ */

#if defined(__x86_64__) || defined(__i386__)

static inline void store_qword(void *addr, uint64_t val)
{
    __asm__ volatile ("movq %1, %0"
        : "=m"(*reinterpret_cast<uint64_t*>(addr))
        : "r"(val)
        : "memory");
}

static inline uint64_t load_qword(const void *addr)
{
    uint64_t result;
    __asm__ volatile ("movq %1, %0"
        : "=r"(result)
        : "m"(*reinterpret_cast<const uint64_t*>(addr))
        : "memory");
    return result;
}

static inline uint32_t load_dword(const void *addr)
{
    uint32_t result;
    __asm__ volatile ("movl %1, %0"
        : "=r"(result)
        : "m"(*reinterpret_cast<const uint32_t*>(addr))
        : "memory");
    return result;
}

#elif defined(__aarch64__)

static inline void store_qword(void *addr, uint64_t val)
{
    __asm__ volatile ("str %1, [%0]"
        :
        : "r"(addr), "r"(val)
        : "memory");
}

static inline uint64_t load_qword(const void *addr)
{
    uint64_t result;
    __asm__ volatile ("ldr %0, [%1]"
        : "=r"(result)
        : "r"(addr)
        : "memory");
    return result;
}

static inline uint32_t load_dword(const void *addr)
{
    uint32_t result;
    __asm__ volatile ("ldr %w0, [%1]"
        : "=r"(result)
        : "r"(addr)
        : "memory");
    return result;
}

#endif /* platform */

/* ------------------------------------------------------------------ */
/* Standard test callbacks                                            */
/* ------------------------------------------------------------------ */

static int mem_disambig_init(struct test *test)
{
    auto *data = static_cast<MemDisambigData *>(malloc(sizeof(MemDisambigData)));
    data->offsets = static_cast<uint64_t *>(malloc(DISAMBIG_PATTERN_COUNT * sizeof(uint64_t)));
    data->values  = static_cast<uint64_t *>(malloc(DISAMBIG_PATTERN_COUNT * sizeof(uint64_t)));

    for (int i = 0; i < DISAMBIG_PATTERN_COUNT; i++) {
        data->offsets[i] = (random64() % (DISAMBIG_BUFFER_SIZE - 16)) & ~7ULL;
        data->values[i]  = random64();
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mem_disambig_run(struct test *test, int cpu)
{
    auto *data = static_cast<MemDisambigData *>(test->data);

    /*
     * Per-thread buffer: each thread gets its own to avoid data races.
     * The framework's overridden allocator zero-fills on allocation.
     */
    uint8_t *buffer = static_cast<uint8_t *>(aligned_alloc(64, DISAMBIG_BUFFER_SIZE));

    TEST_LOOP(test, 1 << 13) {
        /*
         * Pattern 1 — Basic store-to-load forwarding:
         * Store 8 bytes, then immediately load 8 bytes from the
         * same address. The CPU should forward from the store buffer.
         */
        for (int i = 0; i < DISAMBIG_PATTERN_COUNT; i++) {
            uint64_t off   = data->offsets[i];
            uint64_t value  = data->values[i];

            store_qword(buffer + off, value);
            uint64_t loaded = load_qword(buffer + off);

            if (loaded != value) {
                report_fail_msg(
                    "store-to-load forward mismatch at pattern %d "
                    "(offset %llu): expected 0x%llx got 0x%llx",
                    i,
                    static_cast<unsigned long long>(off),
                    static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(loaded));
            }
        }

        /*
         * Pattern 2 — Partial store-to-load forwarding:
         * Store 8 bytes, then load 4 bytes from offset+2 (which
         * partially overlaps the stored 8-byte region). This
         * stresses partial forwarding logic in the store buffer.
         */
        for (int i = 0; i < DISAMBIG_PATTERN_COUNT; i++) {
            uint64_t off   = data->offsets[i];
            uint64_t value = data->values[i];

            store_qword(buffer + off, value);
            uint32_t loaded  = load_dword(buffer + off + 2);
            uint32_t expected = static_cast<uint32_t>(value >> 16);

            if (loaded != expected) {
                report_fail_msg(
                    "partial store-to-load forward mismatch at "
                    "pattern %d: expected 0x%x got 0x%x",
                    i, expected, loaded);
            }
        }

        /*
         * Pattern 3 — Repeated store-then-load to the same address:
         * Store the same value three times, then load. This trains
         * the store buffer predictor on a single address.
         */
        for (int i = 0; i < DISAMBIG_PATTERN_COUNT; i++) {
            uint64_t value = data->values[i];

            store_qword(buffer, value);
            store_qword(buffer, value);
            store_qword(buffer, value);
            uint64_t loaded = load_qword(buffer);

            if (loaded != value) {
                report_fail_msg(
                    "repeated store-load mismatch at pattern %d: "
                    "expected 0x%llx got 0x%llx",
                    i,
                    static_cast<unsigned long long>(value),
                    static_cast<unsigned long long>(loaded));
            }
        }
    }

    free(buffer);
    return EXIT_SUCCESS;
}

static int mem_disambig_cleanup(struct test *test)
{
    auto *data = static_cast<MemDisambigData *>(test->data);
    if (data) {
        free(data->values);
        free(data->offsets);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mem_disambiguation, "stresses store to load forwarding")
    .test_init    = mem_disambig_init,
    .test_run     = mem_disambig_run,
    .test_cleanup = mem_disambig_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
