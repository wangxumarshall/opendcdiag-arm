/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b move_elimination_vec_evex_jit
 * @parblock
 * Test move elimination on XMM, YMM and ZMM registers using VEX and
 * EVEX instructions. On x86-64 the test JIT-generates chains of
 * VMOVDQA (VEX.128 for XMM, VEX.256 for YMM) and VMOVDQA32/64 (EVEX.128
 * for XMM, EVEX.256 for YMM, EVEX.512 for ZMM). On ARM64 it tests
 * NEON register moves (ORR Vd.8B/Vn.8B and ORR Vd.16B/Vn.16B) which
 * exercise the equivalent rename / forwarding logic.
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

typedef void (*jit_vec_func_t)(const void *input, void *output);

#if defined(__x86_64__)

#define INPUT_REG   7   // RDI (Linux ABI)
#define OUTPUT_REG  6   // RSI (Linux ABI)

constexpr int NUM_VEC_REGS = 8;
constexpr int VEC_CHAIN_CYCLES = 16;

static void emit_byte(uint8_t **p, uint8_t b) { *(*p)++ = b; }

static void emit_vex_load_store(uint8_t **p, int vec_reg, bool is_store, bool is_ymm) {
    uint8_t L = is_ymm ? 1 : 0;
    // 2-byte VEX prefix
    emit_byte(p, 0xC5);
    uint8_t b1 = 0x79 | (L << 2); // vvvv=1111, pp=01(66)
    if (vec_reg < 8) b1 |= 0x80;  // R = ~vec_reg[3]
    emit_byte(p, b1);

    if (is_store) {
        emit_byte(p, 0x7F); // VMOVDQA r/m, reg
        emit_byte(p, 0x00 | ((vec_reg & 7) << 3) | (OUTPUT_REG & 7));
    } else {
        emit_byte(p, 0x28); // VMOVDQA reg, r/m
        emit_byte(p, 0x00 | ((vec_reg & 7) << 3) | (INPUT_REG & 7));
    }
}

static void emit_vex_mov(uint8_t **p, int dst, int src, bool is_ymm) {
    uint8_t L = is_ymm ? 1 : 0;
    emit_byte(p, 0xC5);
    uint8_t b1 = 0x79 | (L << 2);
    if (dst < 8) b1 |= 0x80;
    emit_byte(p, b1);
    emit_byte(p, 0x28); // VMOVDQA reg, r/m
    emit_byte(p, 0xC0 | ((dst & 7) << 3) | (src & 7));
}

static void emit_evex_load_store(uint8_t **p, int vec_reg, bool is_store, int width, int w_bit) {
    emit_byte(p, 0x62);

    uint8_t b1 = 0x01; // mmmmm = 00001
    if (!(vec_reg & 8)) b1 |= 0x80;  // R
    b1 |= 0x40;                       // X = 1
    if (is_store) {
        if (!(OUTPUT_REG & 8)) b1 |= 0x20; // B for r/m
    } else {
        if (!(INPUT_REG & 8)) b1 |= 0x20; // B for r/m
    }
    if (!(vec_reg & 16)) b1 |= 0x10; // R'
    emit_byte(p, b1);

    // W vvvv 1 pp (bit 2 must be 1 in EVEX byte 2)
    uint8_t b2 = 0x7D; // 0111 1101 -> W=0, vvvv=1111, bit2=1, pp=01(66)
    if (w_bit) b2 |= 0x80;
    emit_byte(p, b2);

    uint8_t b3 = (width == 0) ? 0x08 : (width == 1) ? 0x28 : 0x48;
    emit_byte(p, b3);

    if (is_store) {
        emit_byte(p, 0x7F); // VMOVDQA r/m, reg
        emit_byte(p, 0x00 | ((vec_reg & 7) << 3) | (OUTPUT_REG & 7));
    } else {
        emit_byte(p, 0x6F); // VMOVDQA reg, r/m
        emit_byte(p, 0x00 | ((vec_reg & 7) << 3) | (INPUT_REG & 7));
    }
}

static void emit_evex_mov(uint8_t **p, int dst, int src, int width, int w_bit) {
    emit_byte(p, 0x62);
    uint8_t b1 = 0x01;
    if (!(dst & 8)) b1 |= 0x80;
    b1 |= 0x40; // X = 1
    if (!(src & 8)) b1 |= 0x20; // B for r/m = src
    if (!(dst & 16)) b1 |= 0x10;
    emit_byte(p, b1);

    // W vvvv 1 pp (bit 2 must be 1 in EVEX byte 2)
    uint8_t b2 = 0x7D;
    if (w_bit) b2 |= 0x80;
    emit_byte(p, b2);

    uint8_t b3 = (width == 0) ? 0x08 : (width == 1) ? 0x28 : 0x48;
    emit_byte(p, b3);

    emit_byte(p, 0x6F); // VMOVDQA reg, r/m
    emit_byte(p, 0xC0 | ((dst & 7) << 3) | (src & 7));
}

static void emit_ret(uint8_t **p) { emit_byte(p, 0xC3); }

static void gen_vex128(uint8_t *code) {
    uint8_t *p = code;
    emit_vex_load_store(&p, 0, false, false);
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_vex_mov(&p, (j + 1) % NUM_VEC_REGS, j, false);
    emit_vex_load_store(&p, 0, true, false);
    emit_ret(&p);
}

static void gen_vex256(uint8_t *code) {
    uint8_t *p = code;
    emit_vex_load_store(&p, 0, false, true);
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_vex_mov(&p, (j + 1) % NUM_VEC_REGS, j, true);
    emit_vex_load_store(&p, 0, true, true);
    emit_ret(&p);
}

static void gen_evex128(uint8_t *code) {
    uint8_t *p = code;
    emit_evex_load_store(&p, 0, false, 0, 0);
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_evex_mov(&p, (j + 1) % NUM_VEC_REGS, j, 0, 0);
    emit_evex_load_store(&p, 0, true, 0, 0);
    emit_ret(&p);
}

static void gen_evex256(uint8_t *code) {
    uint8_t *p = code;
    emit_evex_load_store(&p, 0, false, 1, 0);
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_evex_mov(&p, (j + 1) % NUM_VEC_REGS, j, 1, 0);
    emit_evex_load_store(&p, 0, true, 1, 0);
    emit_ret(&p);
}

static void gen_evex512(uint8_t *code) {
    uint8_t *p = code;
    emit_evex_load_store(&p, 0, false, 2, 1);
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_evex_mov(&p, (j + 1) % NUM_VEC_REGS, j, 2, 1);
    emit_evex_load_store(&p, 0, true, 2, 1);
    emit_ret(&p);
}

constexpr size_t JIT_BUF_SIZE = 4096;
constexpr int NUM_JIT_FUNCS = 5;

#elif defined(__aarch64__)

static const int vec_regs[] = {0, 1, 2, 3, 4, 5, 6, 7};
constexpr int NUM_VEC_REGS = 8;
constexpr int VEC_CHAIN_CYCLES = 16;

static void emit_orr_v16b(uint32_t **p, int dst, int src) {
    *(*p)++ = 0x4EA01C00u | ((uint32_t)src << 16) | ((uint32_t)src << 5) | (uint32_t)dst;
}

static void emit_orr_v8b(uint32_t **p, int dst, int src) {
    *(*p)++ = 0x0EA01C00u | ((uint32_t)src << 16) | ((uint32_t)src << 5) | (uint32_t)dst;
}

static void emit_ldr_q(uint32_t **p, int rt, int rn) {
    *(*p)++ = 0x3DC00000u | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static void emit_str_q(uint32_t **p, int rt, int rn) {
    *(*p)++ = 0x3D800000u | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static void emit_ldr_d(uint32_t **p, int rt, int rn) {
    *(*p)++ = 0xF9400000u | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static void emit_str_d(uint32_t **p, int rt, int rn) {
    *(*p)++ = 0xF9000000u | ((uint32_t)rn << 5) | (uint32_t)rt;
}

static void emit_ret(uint32_t **p) { *(*p)++ = 0xD65F03C0u; }

static void gen_neon128(uint32_t *code) {
    uint32_t *p = code;
    emit_ldr_q(&p, 0, 0); // LDR Q0, [X0]
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_orr_v16b(&p, (j + 1) % NUM_VEC_REGS, j);
    emit_str_q(&p, 0, 1); // STR Q0, [X1]
    emit_ret(&p);
}

static void gen_neon64(uint32_t *code) {
    uint32_t *p = code;
    emit_ldr_d(&p, 0, 0);
    for (int i = 0; i < VEC_CHAIN_CYCLES; i++)
        for (int j = 0; j < NUM_VEC_REGS; j++)
            emit_orr_v8b(&p, (j + 1) % NUM_VEC_REGS, j);
    emit_str_d(&p, 0, 1);
    emit_ret(&p);
}

constexpr size_t JIT_BUF_SIZE = 4096;
constexpr int NUM_JIT_FUNCS = 2;

#endif

struct VecTestData {
    void *jit_mem;
    size_t jit_size;
    jit_vec_func_t vex128, vex256, evex128, evex256, evex512;
    jit_vec_func_t neon64, neon128;
};

static int move_elimination_vec_evex_jit_init(struct test *test) {
#if !defined(__x86_64__) && !defined(__aarch64__)
    log_skip(OsNotSupportedSkipCategory, "move_elimination_vec_evex_jit: unsupported architecture");
    return EXIT_SKIP;
#else
    auto *data = static_cast<VecTestData *>(malloc(sizeof(VecTestData)));
    if (!data) return -ENOMEM;
    memset(data, 0, sizeof(*data));

    data->jit_size = JIT_BUF_SIZE * NUM_JIT_FUNCS;
    data->jit_mem = alloc_jit(data->jit_size);
    if (!data->jit_mem) { free(data); return -ENOMEM; }

#if defined(__x86_64__)
    uint8_t *base = static_cast<uint8_t *>(data->jit_mem);
    gen_vex128(base + JIT_BUF_SIZE * 0);
    gen_vex256(base + JIT_BUF_SIZE * 1);
    gen_evex128(base + JIT_BUF_SIZE * 2);
    gen_evex256(base + JIT_BUF_SIZE * 3);
    gen_evex512(base + JIT_BUF_SIZE * 4);
    data->vex128  = reinterpret_cast<jit_vec_func_t>(base + JIT_BUF_SIZE * 0);
    data->vex256  = reinterpret_cast<jit_vec_func_t>(base + JIT_BUF_SIZE * 1);
    data->evex128 = reinterpret_cast<jit_vec_func_t>(base + JIT_BUF_SIZE * 2);
    data->evex256 = reinterpret_cast<jit_vec_func_t>(base + JIT_BUF_SIZE * 3);
    data->evex512 = reinterpret_cast<jit_vec_func_t>(base + JIT_BUF_SIZE * 4);
#elif defined(__aarch64__)
    uint32_t *base = static_cast<uint32_t *>(data->jit_mem);
    gen_neon64(base);
    gen_neon128(base + JIT_BUF_SIZE / 4);
    data->neon64  = reinterpret_cast<jit_vec_func_t>(base);
    data->neon128 = reinterpret_cast<jit_vec_func_t>(base + JIT_BUF_SIZE / 4);
#endif

    make_executable(data->jit_mem, data->jit_size);
    flush_icache(data->jit_mem, data->jit_size);

    test->data = data;
    return EXIT_SUCCESS;
#endif
}

static int move_elimination_vec_evex_jit_run(struct test *test, int cpu) {
    auto *data = static_cast<VecTestData *>(test->data);

    // Allocate per-thread buffers on the stack to avoid multi-thread data races
    alignas(64) uint8_t input[64];
    alignas(64) uint8_t output[64];

    TEST_LOOP(test, 1 << 13) {
        memset_random(input, sizeof(input));

#if defined(__x86_64__)
        data->vex128(input, output);
        memcmp_or_fail(output, input, 16, "VEX.128 (XMM) move elimination");

        data->vex256(input, output);
        memcmp_or_fail(output, input, 32, "VEX.256 (YMM) move elimination");

        data->evex128(input, output);
        memcmp_or_fail(output, input, 16, "EVEX.128 (XMM) move elimination");

        data->evex256(input, output);
        memcmp_or_fail(output, input, 32, "EVEX.256 (YMM) move elimination");

        data->evex512(input, output);
        memcmp_or_fail(output, input, 64, "EVEX.512 (ZMM) move elimination");
#elif defined(__aarch64__)
        data->neon64(input, output);
        memcmp_or_fail(output, input, 8, "NEON 64-bit (D) move elimination");

        data->neon128(input, output);
        memcmp_or_fail(output, input, 16, "NEON 128-bit (Q) move elimination");
#endif
    }
    return EXIT_SUCCESS;
}

static int move_elimination_vec_evex_jit_cleanup(struct test *test) {
    auto *data = static_cast<VecTestData *>(test->data);
    if (data) {
        if (data->jit_mem) free_jit(data->jit_mem, data->jit_size);
        free(data);
    }
    return EXIT_SUCCESS;
}

DECLARE_TEST(move_elimination_vec_evex_jit, "Test move elimination on XMM, YMM and ZMM registers using VEX and EVEX instructions")
    .test_init    = move_elimination_vec_evex_jit_init,
    .test_run     = move_elimination_vec_evex_jit_run,
    .test_cleanup = move_elimination_vec_evex_jit_cleanup,
#if defined(__x86_64__)
    .minimum_cpu  = cpu_skylake_avx512,
#endif
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
