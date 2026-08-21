/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b mrn_rmw_dump
 * @parblock
 * Dump-on-failure variant of mrn_rmw: identical ALU read-modify-write +
 * store-to-load-forwarding hot path, but on miscompare it records the
 * input / golden / actual / xor values (plus a per-byte breakdown) so the
 * flipped bit of an SDC event can be located. Mirrors the movbe_dump
 * design: the hot loop body is byte-for-byte identical to mrn_rmw.cpp's
 * mrn_rmw_run; the dump lives entirely in the cold failure branch, reading
 * values it needs from data->srcA/srcB/expected and the local res/dst.
 * Do NOT add extra volatile reads or local snapshots to the hot path —
 * that perturbs the instruction schedule and can stop the defect from
 * triggering (see memory movbe-dump-compiler-dce-root-cause).
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

#define MRN_RMW_COUNT 1024

struct MrnRmwData {
    uint64_t *srcA;
    uint64_t *srcB;
    uint64_t *expected;
};

#if defined(__x86_64__) || defined(__i386__)

static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("movq %1, %0" : "=m"(*reinterpret_cast<uint64_t*>(addr)) : "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("movq %1, %0" : "=r"(res) : "m"(*reinterpret_cast<const uint64_t*>(addr)) : "memory");
    return res;
}

#elif defined(__aarch64__)

static inline void store_qword(void *addr, uint64_t val) {
    __asm__ volatile ("str %1, [%0]" : : "r"(addr), "r"(val) : "memory");
}
static inline uint64_t load_qword(const void *addr) {
    uint64_t res;
    __asm__ volatile ("ldr %0, [%1]" : "=r"(res) : "r"(addr) : "memory");
    return res;
}

#endif

static int mrn_rmw_init(struct test *test) {
    auto *data = static_cast<MrnRmwData *>(malloc(sizeof(MrnRmwData)));
    data->srcA = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));
    data->srcB = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));
    data->expected = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));

    memset_random(data->srcA, MRN_RMW_COUNT * sizeof(uint64_t));
    memset_random(data->srcB, MRN_RMW_COUNT * sizeof(uint64_t));

    // Compute expected golden values
    for (int i = 0; i < MRN_RMW_COUNT; i++) {
        switch (i % 4) {
            case 0: data->expected[i] = data->srcA[i] + data->srcB[i]; break;
            case 1: data->expected[i] = data->srcA[i] - data->srcB[i]; break;
            case 2: data->expected[i] = data->srcA[i] ^ data->srcB[i]; break;
            case 3: data->expected[i] = data->srcA[i] & data->srcB[i]; break;
        }
    }

    test->data = data;
    return EXIT_SUCCESS;
}

static int mrn_rmw_run(struct test *test, int cpu) {
    auto *data = static_cast<MrnRmwData *>(test->data);
    uint64_t *temp = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));
    uint64_t *dst  = static_cast<uint64_t *>(aligned_alloc(64, MRN_RMW_COUNT * sizeof(uint64_t)));

    TEST_LOOP(test, 1 << 13) {
        for (int i = 0; i < MRN_RMW_COUNT; i++) {
            uint64_t a = data->srcA[i];
            uint64_t b = data->srcB[i];
            uint64_t res;

            switch (i % 4) {
                case 0: res = a + b; break;
                case 1: res = a - b; break;
                case 2: res = a ^ b; break;
                case 3: res = a & b; break;
            }

            // Store RMW result to temp, then load to dst
            store_qword(&temp[i], res);
            store_qword(&dst[i], load_qword(&temp[i]));
        }

        // Hot path above is byte-for-byte identical to mrn_rmw.cpp. Only on
        // miscompare do we locate the first mismatch and dump input/golden/
        // actual/xor + per-byte breakdown. All dump reads are from the
        // source arrays and the freshly computed dst — no hot-path snapshot.
        if (memcmp(dst, data->expected, MRN_RMW_COUNT * sizeof(uint64_t)) != 0) {
            for (int i = 0; i < MRN_RMW_COUNT; i++) {
                if (dst[i] != data->expected[i]) {
                    uint64_t inputA = data->srcA[i];
                    uint64_t inputB = data->srcB[i];
                    uint64_t golden = data->expected[i];
                    uint64_t actual = dst[i];
                    uint64_t xorv = golden ^ actual;
                    const char *op = (i % 4 == 0) ? "add"
                                   : (i % 4 == 1) ? "sub"
                                   : (i % 4 == 2) ? "xor" : "and";
                    unsigned char *gb = (unsigned char *)&golden;
                    unsigned char *ab = (unsigned char *)&actual;
                    unsigned char *xb = (unsigned char *)&xorv;
                    log_error("mrn_rmw_dump: miscompare at index %u (op=%s): "
                              "inputA=0x%016lX inputB=0x%016lX golden=0x%016lX "
                              "actual=0x%016lX xor=0x%016lX | "
                              "bytes[b7..b0] golden=%02X%02X%02X%02X%02X%02X%02X%02X "
                              "actual=%02X%02X%02X%02X%02X%02X%02X%02X "
                              "xor=%02X%02X%02X%02X%02X%02X%02X%02X",
                              (unsigned)i, op,
                              (unsigned long)inputA, (unsigned long)inputB,
                              (unsigned long)golden, (unsigned long)actual,
                              (unsigned long)xorv,
                              gb[7], gb[6], gb[5], gb[4], gb[3], gb[2], gb[1], gb[0],
                              ab[7], ab[6], ab[5], ab[4], ab[3], ab[2], ab[1], ab[0],
                              xb[7], xb[6], xb[5], xb[4], xb[3], xb[2], xb[1], xb[0]);
                    report_fail_msg("mrn_rmw_dump: data miscompare at index %u (op=%s)", (unsigned)i, op);
                }
            }
        }
    }

    free(dst);
    free(temp);
    return EXIT_SUCCESS;
}

static int mrn_rmw_cleanup(struct test *test) {
    auto *data = static_cast<MrnRmwData *>(test->data);
    if (data) {
        free(data->srcA);
        free(data->srcB);
        free(data->expected);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(mrn_rmw_dump, "mrn_rmw dump variant: records input/golden/actual/xor on miscompare (ALU RMW + store-load forwarding)")
    .test_init = mrn_rmw_init,
    .test_run = mrn_rmw_run,
    .test_cleanup = mrn_rmw_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
