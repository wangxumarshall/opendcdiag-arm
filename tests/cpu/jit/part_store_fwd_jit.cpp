/**
 * @copyright
 * Copyright 2022 Intel Corporation.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b part_store_fwd_jit
 * @parblock
 * Partial store forward. This test uses JIT code generation to create
 * tight sequences of a store followed immediately by a load of a
 * different width to the same memory address. This stresses the CPU's
 * store-to-load forwarding mechanism for partial data, verifying that
 * the CPU correctly forwards and merges the data without silent data
 * corruption.
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
#if defined(__aarch64__) || defined(_M_ARM64)
    __builtin___clear_cache(static_cast<char*>(mem), static_cast<char*>(mem) + size);
#endif
}

static void free_jit(void *mem, size_t size) {
    munmap(mem, size);
}

typedef uint64_t (*jit_func_t)(uint8_t *ptr, uint64_t val);

#if defined(__x86_64__)

static void emit_byte(uint8_t **p, uint8_t b) { *(*p)++ = b; }

static void gen_jit_funcs(uint8_t *code, jit_func_t *funcs) {
    uint8_t *p = code;

    // 0: Store 8-bit, Load 16-bit
    uint8_t *f0 = p;
    emit_byte(&p, 0x40); emit_byte(&p, 0x88); emit_byte(&p, 0x37); // mov byte [rdi], sil
    emit_byte(&p, 0x0F); emit_byte(&p, 0xB7); emit_byte(&p, 0x07); // movzx eax, word [rdi]
    emit_byte(&p, 0xC3); // ret
    funcs[0] = reinterpret_cast<jit_func_t>(f0);

    // 1: Store 16-bit, Load 32-bit
    uint8_t *f1 = p;
    emit_byte(&p, 0x66); emit_byte(&p, 0x89); emit_byte(&p, 0x37); // mov word [rdi], si
    emit_byte(&p, 0x8B); emit_byte(&p, 0x07);                     // mov eax, dword [rdi]
    emit_byte(&p, 0xC3);
    funcs[1] = reinterpret_cast<jit_func_t>(f1);

    // 2: Store 32-bit, Load 64-bit
    uint8_t *f2 = p;
    emit_byte(&p, 0x89); emit_byte(&p, 0x37);                     // mov dword [rdi], esi
    emit_byte(&p, 0x48); emit_byte(&p, 0x8B); emit_byte(&p, 0x07); // mov rax, qword [rdi]
    emit_byte(&p, 0xC3);
    funcs[2] = reinterpret_cast<jit_func_t>(f2);

    // 3: Store 64-bit, Load 32-bit
    uint8_t *f3 = p;
    emit_byte(&p, 0x48); emit_byte(&p, 0x89); emit_byte(&p, 0x37); // mov qword [rdi], rsi
    emit_byte(&p, 0x8B); emit_byte(&p, 0x07);                     // mov eax, dword [rdi]
    emit_byte(&p, 0xC3);
    funcs[3] = reinterpret_cast<jit_func_t>(f3);
}
#elif defined(__aarch64__)

static void emit_word(uint32_t **p, uint32_t w) { *(*p)++ = w; }

static void gen_jit_funcs(uint32_t *code, jit_func_t *funcs) {
    uint32_t *p = code;

    // 0: Store 8-bit, Load 16-bit
    uint32_t *f0 = p;
    emit_word(&p, 0x39000001); // strb w1, [x0]
    emit_word(&p, 0x79400000); // ldrh w0, [x0]
    emit_word(&p, 0xD65F03C0); // ret
    funcs[0] = reinterpret_cast<jit_func_t>(f0);

    // 1: Store 16-bit, Load 32-bit
    uint32_t *f1 = p;
    emit_word(&p, 0x79000001); // strh w1, [x0]
    emit_word(&p, 0xB9400000); // ldr w0, [x0]
    emit_word(&p, 0xD65F03C0);
    funcs[1] = reinterpret_cast<jit_func_t>(f1);

    // 2: Store 32-bit, Load 64-bit
    uint32_t *f2 = p;
    emit_word(&p, 0xB9000001); // str w1, [x0]
    emit_word(&p, 0xF9400000); // ldr x0, [x0]
    emit_word(&p, 0xD65F03C0);
    funcs[2] = reinterpret_cast<jit_func_t>(f2);

    // 3: Store 64-bit, Load 32-bit
    uint32_t *f3 = p;
    emit_word(&p, 0xF9000001); // str x1, [x0]
    emit_word(&p, 0xB9400000); // ldr w0, [x0]
    emit_word(&p, 0xD65F03C0);
    funcs[3] = reinterpret_cast<jit_func_t>(f3);
}
#endif

struct PartStoreFwdData {
    jit_func_t funcs[4];
    void *jit_mem;
    size_t jit_size;
};

static int part_store_fwd_jit_init(struct test *test) {
#if !defined(__x86_64__) && !defined(__aarch64__)
    log_skip(OsNotSupportedSkipCategory, "part_store_fwd_jit: unsupported architecture");
    return EXIT_SKIP;
#else
    auto *data = static_cast<PartStoreFwdData *>(malloc(sizeof(PartStoreFwdData)));
    if (!data) return -ENOMEM;

    data->jit_size = 4096;
    data->jit_mem = alloc_jit(data->jit_size);
    if (!data->jit_mem) { free(data); return -ENOMEM; }

#if defined(__x86_64__)
    gen_jit_funcs(static_cast<uint8_t *>(data->jit_mem), data->funcs);
#elif defined(__aarch64__)
    gen_jit_funcs(static_cast<uint32_t *>(data->jit_mem), data->funcs);
#endif

    make_executable(data->jit_mem, data->jit_size);
    flush_icache(data->jit_mem, data->jit_size);

    test->data = data;
    return EXIT_SUCCESS;
#endif
}

static int part_store_fwd_jit_run(struct test *test, int cpu) {
#if !defined(__x86_64__) && !defined(__aarch64__)
    (void)test; (void)cpu;
    return EXIT_SUCCESS;
#else
    auto *data = static_cast<PartStoreFwdData *>(test->data);
    alignas(64) uint8_t buf[32];
    memset_random(buf, sizeof(buf));

    TEST_LOOP(test, 1 << 16) {
        uintptr_t off = random32() % 8;
        uint8_t *ptr = buf + off;
        int pattern = random32() % 4;

        if (pattern == 0) {
            uint16_t old = *reinterpret_cast<volatile uint16_t*>(ptr);
            uint8_t val = random32();
            uint16_t res = static_cast<uint16_t>(data->funcs[0](ptr, val));
            uint16_t expected = (old & 0xFF00) | val;
            if (res != expected) report_fail_msg("Partial store forward (8->16) failed: expected 0x%04x, got 0x%04x", expected, res);
        } else if (pattern == 1) {
            uint32_t old = *reinterpret_cast<volatile uint32_t*>(ptr);
            uint16_t val = random32();
            uint32_t res = static_cast<uint32_t>(data->funcs[1](ptr, val));
            uint32_t expected = (old & 0xFFFF0000) | val;
            if (res != expected) report_fail_msg("Partial store forward (16->32) failed: expected 0x%08x, got 0x%08x", expected, res);
        } else if (pattern == 2) {
            uint64_t old = *reinterpret_cast<volatile uint64_t*>(ptr);
            uint32_t val = random32();
            uint64_t res = data->funcs[2](ptr, val);
            uint64_t expected = (old & 0xFFFFFFFF00000000ULL) | val;
            if (res != expected) report_fail_msg("Partial store forward (32->64) failed: expected 0x%016lx, got 0x%016lx", expected, res);
        } else {
            uint64_t val = random64();
            uint32_t res = static_cast<uint32_t>(data->funcs[3](ptr, val));
            uint32_t expected = static_cast<uint32_t>(val);
            if (res != expected) report_fail_msg("Partial store forward (64->32) failed: expected 0x%08x, got 0x%08x", expected, res);
        }
    }
    return EXIT_SUCCESS;
#endif
}

static int part_store_fwd_jit_cleanup(struct test *test) {
#if defined(__x86_64__) || defined(__aarch64__)
    auto *data = static_cast<PartStoreFwdData *>(test->data);
    if (data) {
        if (data->jit_mem) free_jit(data->jit_mem, data->jit_size);
        free(data);
    }
#else
    (void)test;
#endif
    return EXIT_SUCCESS;
}

DECLARE_TEST(part_store_fwd_jit, "Partial store forward")
    .test_init    = part_store_fwd_jit_init,
    .test_run     = part_store_fwd_jit_run,
    .test_cleanup = part_store_fwd_jit_cleanup,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
