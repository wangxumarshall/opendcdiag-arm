/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b move_elimination_8_jit
 * @parblock
 * Test move elimination with 8 bit values. This test uses JIT code
 * generation to create long chains of dependent 8-bit MOV instructions
 * between byte registers (AL, CL, DL, BL, R8B–R15B on x86-64),
 * stressing the CPU's move elimination logic for partial-register
 * writes. On ARM64, which lacks 8-bit registers, 32-bit MOVs are used
 * and only the low byte of the result is verified.
 * @endparblock
 */

#include "sandstone.h"

#include <cstdint>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>

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

typedef uint64_t (*jit_func_t)(uint64_t);

#if defined(__x86_64__)

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

static const int byte_regs[] = {
    0, 1, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
};
constexpr size_t NUM_BYTE_REGS = sizeof(byte_regs) / sizeof(byte_regs[0]);
constexpr int CHAIN_CYCLES = 18;

static void emit_push_r64(uint8_t **p, int reg) {
    if (reg >= 8) { *(*p)++ = 0x41; *(*p)++ = 0x50 + (reg & 7); }
    else { *(*p)++ = 0x50 + reg; }
}

static void emit_pop_r64(uint8_t **p, int reg) {
    if (reg >= 8) { *(*p)++ = 0x41; *(*p)++ = 0x58 + (reg & 7); }
    else { *(*p)++ = 0x58 + reg; }
}

static void emit_mov_r8_r8(uint8_t **p, int dst, int src) {
    uint8_t rex = 0x40;
    if (src >= 8) rex |= 0x04;
    if (dst >= 8) rex |= 0x01;
    *(*p)++ = rex;
    *(*p)++ = 0x88;
    *(*p)++ = 0xC0 | ((src & 7) << 3) | (dst & 7);
}

static void emit_movzx_eax_r8(uint8_t **p, int src) {
    if (src >= 8) *(*p)++ = 0x41;
    *(*p)++ = 0x0F;
    *(*p)++ = 0xB6;
    *(*p)++ = 0xC0 | (src & 7);
}

static void emit_ret(uint8_t **p) { *(*p)++ = 0xC3; }

static void gen_jit8(uint8_t *code) {
    uint8_t *p = code;
    emit_push_r64(&p, RBX); emit_push_r64(&p, R12); emit_push_r64(&p, R13);
    emit_push_r64(&p, R14); emit_push_r64(&p, R15);

    emit_mov_r8_r8(&p, 0 /* AL */, 7 /* DIL */); // Linux ABI: RDI low byte

    for (int i = 0; i < CHAIN_CYCLES; i++) {
        for (size_t j = 0; j < NUM_BYTE_REGS; j++) {
            int src = byte_regs[j];
            int dst = byte_regs[(j + 1) % NUM_BYTE_REGS];
            emit_mov_r8_r8(&p, dst, src);
        }
    }

    emit_movzx_eax_r8(&p, 0 /* AL */);

    emit_pop_r64(&p, R15); emit_pop_r64(&p, R14); emit_pop_r64(&p, R13);
    emit_pop_r64(&p, R12); emit_pop_r64(&p, RBX);
    emit_ret(&p);
}

constexpr size_t JIT_BUF_SIZE = 4096;

#elif defined(__aarch64__)

static const int chain_regs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
constexpr size_t NUM_CHAIN_REGS = 16;
constexpr int CHAIN_CYCLES = 16;

static void emit_mov_x32(uint32_t **p, int dst, int src) {
    *(*p)++ = 0x2A0003E0u | ((uint32_t)src << 16) | (uint32_t)dst;
}

static void emit_ret(uint32_t **p) { *(*p)++ = 0xD65F03C0u; }

static void gen_jit8(uint32_t *code) {
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

constexpr size_t JIT_BUF_SIZE = 4096;

#endif

struct MoveElim8Data {
    jit_func_t func8;
    void *jit_mem;
    size_t jit_size;
};

static int move_elimination_8_jit_init(struct test *test) {
#if !defined(__x86_64__) && !defined(__aarch64__)
    log_skip(OsNotSupportedSkipCategory, "move_elimination_8_jit: unsupported architecture");
    return EXIT_SKIP;
#else
    auto *data = static_cast<MoveElim8Data *>(malloc(sizeof(MoveElim8Data)));
    if (!data) return -ENOMEM;

    data->jit_size = JIT_BUF_SIZE;
    data->jit_mem = alloc_jit(data->jit_size);
    if (!data->jit_mem) { free(data); return -ENOMEM; }

#if defined(__x86_64__)
    gen_jit8(static_cast<uint8_t *>(data->jit_mem));
#elif defined(__aarch64__)
    gen_jit8(static_cast<uint32_t *>(data->jit_mem));
#endif
    data->func8 = reinterpret_cast<jit_func_t>(data->jit_mem);

    make_executable(data->jit_mem, data->jit_size);
    flush_icache(data->jit_mem, data->jit_size);

    test->data = data;
    return EXIT_SUCCESS;
#endif
}

static int move_elimination_8_jit_run(struct test *test, int cpu) {
    auto *data = static_cast<MoveElim8Data *>(test->data);

    TEST_LOOP(test, 1 << 16) {
        uint64_t val = random64();
        uint64_t result = data->func8(val);
        uint64_t expected = val & 0xFF;
        if ((result & 0xFF) != expected) {
            report_fail_msg("8-bit move elimination failed: expected 0x%02lx, got 0x%02lx",
                             static_cast<unsigned long>(expected),
                             static_cast<unsigned long>(result & 0xFF));
        }
    }
    return EXIT_SUCCESS;
}

static int move_elimination_8_jit_cleanup(struct test *test) {
    auto *data = static_cast<MoveElim8Data *>(test->data);
    if (data) {
        if (data->jit_mem) free_jit(data->jit_mem, data->jit_size);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(move_elimination_8_jit, "Test move elimination with 8 bit values")
    .test_init    = move_elimination_8_jit_init,
    .test_run     = move_elimination_8_jit_run,
    .test_cleanup = move_elimination_8_jit_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
