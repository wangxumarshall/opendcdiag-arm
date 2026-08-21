/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b partial_store_forwarding
 * @parblock
 * Load accessing a store buffer entry for partial data. This test
 * performs random stores and loads of varying widths (1, 2, 4, 8 bytes)
 * to a shared memory region. It maintains a shadow buffer to track
 * expected memory contents, and verifies that every load (especially
 * those accessing only a portion of a recently written store buffer
 * entry) returns the correctly merged data.
 * @endparblock
 */

#include "sandstone.h"
#include <cstdint>
#include <cstring>

// 强制生成不同位宽的 load 指令
static inline uint64_t read_mem(volatile uint8_t *addr, int size) {
    switch (size) {
        case 1: return *reinterpret_cast<volatile uint8_t*>(addr);
        case 2: return *reinterpret_cast<volatile uint16_t*>(addr);
        case 4: return *reinterpret_cast<volatile uint32_t*>(addr);
        case 8: return *reinterpret_cast<volatile uint64_t*>(addr);
    }
    return 0;
}

// 强制生成不同位宽的 store 指令
static inline void write_mem(volatile uint8_t *addr, int size, uint64_t val) {
    switch (size) {
        case 1: *reinterpret_cast<volatile uint8_t*>(addr) = static_cast<uint8_t>(val); break;
        case 2: *reinterpret_cast<volatile uint16_t*>(addr) = static_cast<uint16_t>(val); break;
        case 4: *reinterpret_cast<volatile uint32_t*>(addr) = static_cast<uint32_t>(val); break;
        case 8: *reinterpret_cast<volatile uint64_t*>(addr) = val; break;
    }
}

static int partial_store_forwarding_run(struct test *test, int cpu) {
    // 线程局部的内存缓冲区，避免多线程竞争
    alignas(64) uint8_t real_buf[256];
    alignas(64) uint8_t shadow_buf[256];

    // 初始化数据
    memset_random(real_buf, sizeof(real_buf));
    memcpy(shadow_buf, real_buf, sizeof(real_buf));

    TEST_LOOP(test, 1 << 16) {
        // 随机选取一个偏移量和大小进行写操作
        uintptr_t s_off = random32() % (sizeof(real_buf) - 8);
        int s_sz = 1 << (random32() % 4); // 1, 2, 4, 8
        uint64_t s_val = random64();

        // 写入真实缓冲区
        write_mem(real_buf + s_off, s_sz, s_val);
        // 更新影子缓冲区
        write_mem(reinterpret_cast<volatile uint8_t*>(shadow_buf + s_off), s_sz, s_val);
        
        // 内存屏障，防止编译器将 store 和随后的 load 优化掉或重排
        asm volatile("" ::: "memory");

        // 随机选取一个偏移量和大小进行读操作 (很可能会访问到刚写入的部分数据)
        uintptr_t l_off = random32() % (sizeof(real_buf) - 8);
        int l_sz = 1 << (random32() % 4); // 1, 2, 4, 8

        uint64_t real_val = read_mem(real_buf + l_off, l_sz);
        uint64_t shadow_val = read_mem(reinterpret_cast<volatile uint8_t*>(shadow_buf + l_off), l_sz);

        if (real_val != shadow_val) {
            report_fail_msg("Partial store forwarding failed at offset %lu, size %d: expected 0x%016lx, got 0x%016lx",
                             l_off, l_sz, shadow_val, real_val);
        }
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(partial_store_forwarding, "Load accessing a store buffer entry for partial data")
    .test_run     = partial_store_forwarding_run,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
