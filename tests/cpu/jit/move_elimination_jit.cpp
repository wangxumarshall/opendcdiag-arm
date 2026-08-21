/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b move_elimination_jit
 * @parblock
 * Test move elimination with 32 and 64 bit values. This test uses JIT
 * code generation to create long chains of dependent MOV instructions
 * between general-purpose registers, stressing the CPU's move
 * elimination (register renaming) logic. Both 32-bit and 64-bit
 * register widths are tested with random data to detect silent data
 * corruption during the rename/forwarding stage.
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

/* ===================================================================== */
/* JIT memory helpers                                                    */
/* ===================================================================== */

static void *alloc_jit(size_t size) {
    void *mem = mmap(nullptr, size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) return nullptr;
    return mem;
}

static void make_executable(void *mem, size_t size) {
    mprotect(mem, size, PROT_READ | PROT_EXEC);
}

static void flush_icache(void *mem, size_t size) {
#if defined(__aarch64__) || defined(_M_ARM64)
    __builtin___clear_cache(static_cast<char*>(mem), static_cast<char*>(mem) + size);
#endif
}

static void free_jit(void *mem, size_t size) {
    munmap(mem, size);
}

/* ===================================================================== */
/* Platform-specific JIT code generation                                 */
/* ===================================================================== */

typedef uint64_t (*jit_func_t)(uint64_t);

#if defined(__x86_64__)

// x86-64 register numbers
#define RAX  0
#define RCX  1
#define RDX  2
#define RBX  3
#define RSI  6
#define RDI  7
#define R8   8
#define R9   9
#define R10  10
#define R11  11
#define R12  12
#define R13  13
#define R14  14
#define R15  15

static const int chain_regs[] = {
    RAX, RCX, RDX, RBX, RSI, RDI,
    R8, R9, R10, R11, R12, R13, R14, R15
};
constexpr size_t NUM_CHAIN_REGS = sizeof(chain_regs) / sizeof(chain_regs[0]);
constexpr int CHAIN_CYCLES = 18;

static void emit_push_r64(uint8_t **p, int reg) {
    if (reg >= 8) {
        *(*p)++ = 0x41;
        *(*p)++ = 0x50 + (reg & 7);
    } else {
        *(*p)++ = 0x50 + reg;
    }
}

static void emit_pop_r64(uint8_t **p, int reg) {
    if (reg >= 8) {
        *(*p)++ = 0x41;
        *(*p)++ = 0x58 + (reg & 7);
    } else {
        *(*p)++ = 0x58 + reg;
    }
}

static void emit_mov_r64_r64(uint8_t **p, int dst, int src) {
    uint8_t rex = 0x48;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    *(*p)++ = rex;
    *(*p)++ = 0x89;
    *(*p)++ = 0xC0 | ((src & 7) << 3) | (dst & 7);
}

static void emit_mov_r32_r32(uint8_t **p, int dst, int src) {
    uint8_t rex = 0;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    if (rex) *(*p)++ = 0x40 | rex;
    *(*p)++ = 0x89;
    *(*p)++ = 0xC0 | ((src & 7) << 3) | (dst & 7);
}

static void emit_ret(uint8_t **p) { *(*p)++ = 0xC3; }

static void gen_jit64(uint8_t *code) {
    uint8_t *p = code;
    emit_push_r64(&p, RBX); emit_push_r64(&p, R12); emit_push_r64(&p, R13);
    emit_push_r64(&p, R14); emit_push_r64(&p, R15);

    emit_mov_r64_r64(&p, RAX, RDI); // Move input (Linux ABI: RDI) to RAX

    for (int i = 0; i < CHAIN_CYCLES; i++) {
        for (size_t j = 0; j < NUM_CHAIN_REGS; j++) {
            int src = chain_regs[j];
            int dst = chain_regs[(j + 1) % NUM_CHAIN_REGS];
            emit_mov_r64_r64(&p, dst, src);
        }
    }

    emit_pop_r64(&p, R15); emit_pop_r64(&p, R14); emit_pop_r64(&p, R13);
    emit_pop_r64(&p, R12); emit_pop_r64(&p, RBX);
    emit_ret(&p);
}

static void gen_jit32(uint8_t *code) {
    uint8_t *p = code;
    emit_push_r64(&p, RBX); emit_push_r64(&p, R12); emit_push_r64(&p, R13);
    emit_push_r64(&p, R14); emit_push_r64(&p, R15);

    emit_mov_r32_r32(&p, RAX, RDI);

    for (int i = 0; i < CHAIN_CYCLES; i++) {
        for (size_t j = 0; j < NUM_CHAIN_REGS; j++) {
            int src = chain_regs[j];
            int dst = chain_regs[(j + 1) % NUM_CHAIN_REGS];
            emit_mov_r32_r32(&p, dst, src);
        }
    }

    emit_pop_r64(&p, R15); emit_pop_r64(&p, R14); emit_pop_r64(&p, R13);
    emit_pop_r64(&p, R12); emit_pop_r64(&p, RBX);
    emit_ret(&p);
}

constexpr size_t JIT_BUF_SIZE = 4096;

#elif defined(__aarch64__)

static const int chain_regs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr size_t NUM_CHAIN_REGS = 16;
constexpr int CHAIN_CYCLES = 16;

static void emit_mov_x64(uint32_t **p, int dst, int src) {
    *(*p)++ = 0xAA0003E0u | ((uint32_t)src << 16) | (uint32_t)dst;
}

static void emit_mov_x32(uint32_t **p, int dst, int src) {
    *(*p)++ = 0x2A0003E0u | ((uint32_t)src << 16) | (uint32_t)dst;
}

static void emit_ret(uint32_t **p) { *(*p)++ = 0xD65F03C0u; }

static void gen_jit64(uint32_t *code) {
    uint32_t *p = code;
    for (int i = 0; i < CHAIN_CYCLES; i++) {
        for (size_t j = 0; j < NUM_CHAIN_REGS; j++) {
            int src = chain_regs[j];
            int dst = chain_regs[(j + 1) % NUM_CHAIN_REGS];
            emit_mov_x64(&p, dst, src);
        }
    }
    emit_ret(&p);
}

static void gen_jit32(uint32_t *code) {
    uint32_t *p = code;
    for (int i = 0; i < CHAIN_CYCLES; i++) {
        for (size_t j = 0; j < NUM_CHAIN_REGS; j++) {
            int src = chain_regs[j];
            int dst = chain_regs[(j + 1) % NUM_CHAIN_REGS];
            emit_mov_x32(&p, dst, src);
        }
    }
    emit_ret(&p);
}

constexpr size_t JIT_BUF_SIZE = 4096; // bytes; 1024 ARM64 instructions

#endif

/* ===================================================================== */
/* Test data and functions                                               */
/* ===================================================================== */

struct MoveElimData {
    jit_func_t func64;
    jit_func_t func32;
    void *jit_mem;
    size_t jit_size;
};

static int move_elimination_jit_init(struct test *test) {
#if !defined(__x86_64__) && !defined(__aarch64__)
    log_skip(OsNotSupportedSkipCategory, "move_elimination_jit: unsupported architecture");
    return EXIT_SKIP;
#else
    auto *data = static_cast<MoveElimData *>(malloc(sizeof(MoveElimData)));
    if (!data) return -ENOMEM;

    data->jit_size = JIT_BUF_SIZE * 2;
    data->jit_mem = alloc_jit(data->jit_size);
    if (!data->jit_mem) {
        free(data);
        return -ENOMEM;
    }

#if defined(__x86_64__)
    uint8_t *base = static_cast<uint8_t *>(data->jit_mem);
    gen_jit64(base);
    gen_jit32(base + JIT_BUF_SIZE);
    data->func64 = reinterpret_cast<jit_func_t>(base);
    data->func32 = reinterpret_cast<jit_func_t>(base + JIT_BUF_SIZE);
#elif defined(__aarch64__)
    uint32_t *base = static_cast<uint32_t *>(data->jit_mem);
    gen_jit64(base);
    gen_jit32(base + JIT_BUF_SIZE / 4);
    data->func64 = reinterpret_cast<jit_func_t>(base);
    data->func32 = reinterpret_cast<jit_func_t>(base + JIT_BUF_SIZE / 4);
#endif

    make_executable(data->jit_mem, data->jit_size);
    flush_icache(data->jit_mem, data->jit_size);

    test->data = data;
    return EXIT_SUCCESS;
#endif
}

static int move_elimination_jit_run(struct test *test, int cpu) {
    auto *data = static_cast<MoveElimData *>(test->data);

    TEST_LOOP(test, 1 << 16) {
        uint64_t val64 = random64();
        uint64_t result64 = data->func64(val64);
        if (result64 != val64) {
            report_fail_msg("64-bit move elimination failed: expected 0x%016lx, got 0x%016lx",
                             static_cast<unsigned long>(val64), static_cast<unsigned long>(result64));
        }

        uint64_t val32 = random64();
        uint64_t result32 = data->func32(val32);
        uint64_t expected32 = val32 & 0xFFFFFFFF;
        if (result32 != expected32) {
            report_fail_msg("32-bit move elimination failed: expected 0x%08lx, got 0x%016lx",
                             static_cast<unsigned long>(expected32), static_cast<unsigned long>(result32));
        }
    }
    return EXIT_SUCCESS;
}

static int move_elimination_jit_cleanup(struct test *test) {
    auto *data = static_cast<MoveElimData *>(test->data);
    if (data) {
        if (data->jit_mem) free_jit(data->jit_mem, data->jit_size);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(move_elimination_jit, "Test move elimination with 32 and 64 bit values")
    .test_init    = move_elimination_jit_init,
    .test_run     = move_elimination_jit_run,
    .test_cleanup = move_elimination_jit_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
