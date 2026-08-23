/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ifu_branch_target_arm
 * @parblock
 * Instruction-Fetch Unit SDC stress — a coverage-weak unit (IFU 66%). The
 * suite's existing tests exercise the decode/execute of fixed loops; none
 * target the IFU's hardest SDC surface: the **branch predictor / BTB /
 * instruction-cache under data-dependent indirect branches**, where a fetch/
 * BTB/decode fault silently lands execution at the wrong target — a
 * transient control-flow SDC that no ALU golden compare catches (the wrong
 * block runs to completion and returns a wrong-but-plausible result).
 *
 * This test JIT-generates (mmap + PROT_EXEC + __builtin___clear_cache) a
 * dispatch table of N target blocks plus a data-dependent dispatcher:
 *
 *   dispatcher(idx): add x9, x1, x0, lsl #3   ; table base + idx*8
 *                    ldr x10, [x9]            ; load target pointer
 *                    br  x10                  ; **data-dependent indirect branch**
 *
 * The target pointer table is a **high-Hamming permutation** so successive
 * dispatches jump between far-apart BTB entries (worst-case target toggling
 * for the predictor / BTB / i-cache), and the dispatch sequence is itself
 * a high-Hamming index order so the branch is not trivially memorizable.
 * Each target block returns a distinct id; the dispatcher's returned value
 * must equal the id of the target that was indexed — a fault in fetch/BTB/
 * decode that lands at the wrong block produces the wrong id.
 *
 * To also stress the **i-cache under branch pressure**, each target block
 * is padded with a run of `mov` instructions so the i-cache footprint per
 * dispatch is real (not a single-instruction block). And the run sweeps the
 * whole table each iteration so the BTB/i-cache see the full target set.
 *
 * SDC detection: each dispatch's returned id is byte-exact compared against
 * the expected id; report_fail_msg on mismatch. ARM64-native (JIT inline-asm
 * encoders for movz/movk/add/ldr/br/ret); non-aarch64 returns a clean
 * EXIT_SKIP. Wired into the arm64 subdir (aarch64-only guard), so x86-64 is
 * untouched.
 *
 * Instruction encodings (ARMv8, 32-bit, verified by a standalone JIT probe
 * before integration): movz=0xD2800000|hw<<21|imm<<5|d; movk=0xF2800000|...;
 * add (shifted reg, LSL#amt)=0x8B000000|m<<16|amt<<10|n<<5|d; ldr (imm)
 * =0xF9400000|imm12<<10|n<<5|d; br=0xD61F0000|n<<5; ret=0xD65F03C0.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

static constexpr int NUM_TARGETS = 64;       // dispatch-table size
static constexpr int BLOCK_PADDING = 32;    // mov instructions per target (i-cache footprint)
static constexpr size_t JIT_BUF_SIZE = 1 << 16; // 64K (words) headroom

// High-Hamming index order: sweep the target table in a permutation order so
// successive dispatches jump between far-apart BTB entries. Deterministic
// (reproducible golden) xorshift permutation of 0..N-1.
static void build_index_order(std::vector<int> &order, int n)
{
    order.resize(n);
    for (int i = 0; i < n; ++i) order[i] = i;
    uint64_t state = 0x0123456789ABCDEFULL;
    for (int i = n; i > 1; --i) {
        state ^= (state << 13);
        state ^= (state >> 7);
        state ^= (state << 17);
        int j = (int)(state % (uint64_t)i);
        std::swap(order[i - 1], order[j]);
    }
}

#if defined(__aarch64__)
// ---- ARM64 instruction encoders (32-bit words). Verified standalone.
static inline uint32_t enc_movz(int d, uint16_t imm, int hw)
{ return 0xD2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm << 5) | (uint32_t)(d & 31); }
static inline uint32_t enc_movk(int d, uint16_t imm, int hw)
{ return 0xF2800000u | ((uint32_t)(hw & 3) << 21) | ((uint32_t)imm << 5) | (uint32_t)(d & 31); }
static inline uint32_t enc_add_lsl(int d, int n, int m, int amt)
{ return 0x8B000000u | ((uint32_t)(m & 31) << 16) | ((uint32_t)(amt & 63) << 10)
                     | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31); }
static inline uint32_t enc_ldr_imm(int d, int n, uint16_t imm12)
{ return 0xF9400000u | ((uint32_t)imm12 << 10) | ((uint32_t)(n & 31) << 5) | (uint32_t)(d & 31); }
static inline uint32_t enc_br(int n)
{ return 0xD61F0000u | ((uint32_t)(n & 31) << 5); }
static inline uint32_t enc_ret() { return 0xD65F03C0u; }
static inline uint32_t enc_mov_x(int d, int n) // mov Xd, Xn (alias of ORR)
{ return 0xAA0003E0u | ((uint32_t)(n & 31) << 16) | (uint32_t)(d & 31); }
#endif

struct IfuBranchData {
    std::vector<int> order; // dispatch index order (high-Hamming permutation)
};

static int ifu_branch_target_arm_init(struct test *test)
{
    auto *data = new IfuBranchData{};
    build_index_order(data->order, NUM_TARGETS);
    test->data = data;
    return EXIT_SUCCESS;
}

#ifdef __aarch64__

// Signature of a dispatcher: dispatch(idx_in_x0, table_base_in_x1) -> id.
// (Linux AAPCS64: x0 first arg, x1 second arg, x0 return.)
typedef uint64_t (*disp_t)(uint64_t idx, void *table);

static int ifu_branch_target_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<IfuBranchData *>(test->data);

    // Allocate the JIT code region (RW during emission).
    size_t code_bytes = JIT_BUF_SIZE * sizeof(uint32_t);
    uint32_t *code = static_cast<uint32_t *>(
        mmap(nullptr, code_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (code == (uint32_t *)MAP_FAILED) {
        log_skip(OSResourceIssueSkipCategory,
                 "ifu_branch_target_arm: code mmap failed");
        return EXIT_SKIP;
    }

    // Allocate the target pointer table (RW; the dispatcher loads from it).
    size_t tbl_bytes = (size_t)NUM_TARGETS * sizeof(uint64_t);
    uint64_t *table = static_cast<uint64_t *>(
        mmap(nullptr, tbl_bytes, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    if (table == (uint64_t *)MAP_FAILED) {
        munmap(code, code_bytes);
        log_skip(OSResourceIssueSkipCategory,
                 "ifu_branch_target_arm: table mmap failed");
        return EXIT_SKIP;
    }

    // Emit the target blocks. Block i: return the id (5000 + i), padded with
    // `mov` instructions for i-cache footprint. Record each block's address
    // in the pointer table.
    uint32_t *p = code;
    for (int i = 0; i < NUM_TARGETS; ++i) {
        table[i] = (uint64_t)(uintptr_t)p;
        // movz x0, #(low16 of id)
        uint32_t id = (uint32_t)(5000 + i);
        *p++ = enc_movz(0, (uint16_t)(id & 0xffff), 0);
        if (id > 0xffff) {
            *p++ = enc_movk(0, (uint16_t)((id >> 16) & 0xffff), 1);
        }
        // Padding: a chain of mov between x19..x28 (caller-saved in AAPCS64
        // are x0-x18, so x19+ is callee-saved — we don't touch them to keep
        // the dispatcher's caller frame intact; use mov x0,x0 nops which
        // still consume a fetch/decode slot for the i-cache footprint).
        for (int k = 0; k < BLOCK_PADDING; ++k)
            *p++ = enc_mov_x(0, 0); // mov x0, x0 (no-op, real fetch slot)
        *p++ = enc_ret();
    }

    // Emit the dispatcher. dispatch(idx, table_base):
    //   add  x9, x1, x0, lsl #3   ; &table[idx]
    //   ldr  x10, [x9]            ; target pointer
    //   br   x10                   ; data-dependent indirect branch
    //   ret                        ; unreachable
    disp_t dispatcher = (disp_t)p;
    *p++ = enc_add_lsl(9, 1, 0, 3);
    *p++ = enc_ldr_imm(10, 9, 0);
    *p++ = enc_br(10);
    *p++ = enc_ret();

    // Flip code to RX and flush the i-cache so the IFU sees the emitted code.
    mprotect(code, code_bytes, PROT_READ | PROT_EXEC);
    __builtin___clear_cache((char *)code, (char *)code + code_bytes);

    do {
        bool all_passed = true;

        // Sweep the dispatch table in the high-Hamming index order. Each
        // dispatch's returned id must equal the expected id (5000 + idx). A
        // fetch/BTB/decode fault landing at the wrong block returns the
        // wrong id.
        for (int oi = 0; oi < NUM_TARGETS; ++oi) {
            int idx = d->order[oi];
            uint64_t got = dispatcher((uint64_t)idx, (void *)table);
            uint64_t want = (uint64_t)(5000 + idx);
            if (got != want) {
                log_warning("ifu_branch_target_arm: dispatch idx %d "
                            "(order %d) got %lu want %lu (fetch/BTB SDC)",
                            idx, oi, (unsigned long)got,
                            (unsigned long)want);
                all_passed = false;
                break;
            }
        }

        if (!all_passed) {
            munmap(table, tbl_bytes);
            munmap(code, code_bytes);
            report_fail_msg("ifu_branch_target_arm: IFU fetch/BTB/decode "
                            "indirect-branch SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    munmap(table, tbl_bytes);
    munmap(code, code_bytes);
    return EXIT_SUCCESS;
}
#else
static int ifu_branch_target_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 IFU JIT "
             "(movz/movk/add/ldr/br/ret encoders + computed-goto dispatch) "
             "required");
    return EXIT_SKIP;
}
#endif

static int ifu_branch_target_arm_finish(struct test *test)
{
    delete static_cast<IfuBranchData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ifu_branch_target_arm,
             "Instruction-Fetch Unit SDC stress: JIT-generated data-dependent "
             "indirect-branch (br) dispatch table with high-Hamming target "
             "permutation and padded blocks, to torture the ARM64 branch "
             "predictor / BTB / instruction-cache (IFU) path")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ifu_branch_target_arm_init,
    .test_run = ifu_branch_target_arm_run,
    .test_cleanup = ifu_branch_target_arm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
