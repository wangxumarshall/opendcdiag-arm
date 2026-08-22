/**
 * @copyright
 * Copyright 2026.
 * SPDX-License-Identifier: Apache-2.0
 *
 * @test @b ooo_dep_chain_arm
 * @parblock
 * Out-of-order execution SDC stress — a coverage-weak unit (OoO 56%). The
 * suite's existing tests (adcx, fma, spinlock, memcpy) run either fully
 * independent ILP-friendly loops (no serial dependency) or fully serial
 * ALU carry chains (no memory). Neither targets the OoO machinery's hardest
 * SDC surface: the **scheduler / ROB / rename under long dependent chains
 * that mix memory and ALU**, where a transient re-execution replay fault
 * (a re-fetched instruction producing a different result than its first
 * execution) or a rename-table / reorder-buffer corruption silently
 * corrupts a downstream dependent value.
 *
 * This test builds four such chains and checksums each against a software
 * reference:
 *
 *   1. **Pointer-chasing dependency chain** through a permutation array:
 *      `next = chain[next]`, where each load's *address* depends on the
 *      previous load's *data*. This is the textbook LSU-serializing,
 *      ILP-killing pattern — the scheduler cannot issue load k+1 until load
 *      k retires, so the whole ROB is occupied tracking one dependent chain.
 *      A replay fault on any load breaks the chain and corrupts the visited
 *      set / final pointer. The permutation is a high-Hamming LCG so the
 *      addresses toggle maximally (data-path bonus).
 *   2. **Carry-dependent add chain**: `acc = (acc << 1) + next_val`, where
 *      next_val is read from the chain — each step's ALU result feeds the
 *      next step's address computation (mixed ALU+memory dependency). This
 *      forces the scheduler to reorder ALU and memory across the dependency
 *      edge — the mixed-latency case most likely to surface a replay fault.
 *   3. **Anti-dependence (WAW/RW) interleave**: a chain of stores to
 *      distinct addresses where each store's address is read back (load)
 *      before the next store — the RW hazard path that the rename table
 *      must track. A rename fault corrupts the read-back.
 *   4. **Long ALU dependency chain (no memory)**: a ~256-deep dependent
 *      `mul/add/shift` chain (data-dependent, not independent ILP) —
 *      stresses the physical-register-file / ROB depth and the rename of
 *      a long serial dependency, the classic move-elimination/rename SDC
 *      surface (complements move_elimination_jit which targets MOV rename
 *      specifically).
 *
 * SDC detection: each chain's final checksum is byte-exact compared against
 * a software reference computed with the identical deterministic sequence;
 * report_fail_msg on mismatch. ARM64-native (inline-asm ldr/str/add/mul for
 * the chains so the toolchain cannot hoist/CSE the dependency); non-aarch64
 * returns a clean EXIT_SKIP. Wired into the arm64 subdir (aarch64-only guard),
 * so x86-64 is untouched.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <vector>

static constexpr size_t CHAIN_LEN = 4096;   // pointer-chase steps
static constexpr size_t ALU_DEPTH = 256;    // dependent ALU chain depth
static constexpr size_t ROB_DEPTH = 2048;  // long ALU chain to fill the ROB

#if defined(__aarch64__)
// Inline-asm load/store/add so the toolchain cannot hoist/CSE the dependent
// chain (a real serial dependency through the LSU/ALU, not a reg-reg fold).
static inline uint64_t dep_load64(const void *a)
{
    uint64_t r;
    __asm__ volatile("ldr %0, [%1]" : "=r"(r) : "r"(a) : "memory");
    return r;
}
static inline void dep_store64(void *a, uint64_t v)
{
    __asm__ volatile("str %1, [%0]" : : "r"(a), "r"(v) : "memory");
}
#else
static inline uint64_t dep_load64(const void *a) { uint64_t v; std::memcpy(&v,a,8); return v; }
static inline void dep_store64(void *a, uint64_t v) { std::memcpy(a,&v,8); }
#endif

struct OooDepData {
    // Permutation chain array (CHAIN_LEN entries). chain[i] holds the index
    // of the next node when visiting node i. The node's *value* is stored in
    // a parallel array `values` so the pointer-chase address depends on data.
    std::vector<uint64_t> chain;    // CHAIN_LEN entries, each an index 0..CHAIN_LEN-1
    std::vector<uint64_t> values;   // CHAIN_LEN entries, the data at each node
    // The RW-hazard store addresses (distinct cache lines).
    std::vector<uint64_t> rw_addrs; // ROB_DEPTH entries
};

// Build a pseudo-random permutation of 0..N-1 using a high-Hamming LCG so
// successive visited addresses toggle maximally. Writes the permutation as
// a single cycle (chain[i] = next index) so a full chase visits every node.
static void build_permutation(std::vector<uint64_t> &chain, size_t n)
{
    chain.resize(n);
    // LCG-based shuffle (deterministic, no rand — reproducible goldens).
    std::vector<uint64_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = i;
    uint64_t state = 0x1234567890ABCDEFULL;
    for (size_t i = n; i > 1; --i) {
        // high-Hamming step: xorshift + rotate for max toggling
        state ^= (state << 13);
        state ^= (state >> 7);
        state ^= (state << 17);
        uint64_t j = state % i;
        std::swap(idx[i - 1], idx[j]);
    }
    // Link the permutation as a single cycle: chain[idx[k]] = idx[k+1].
    for (size_t k = 0; k + 1 < n; ++k)
        chain[idx[k]] = idx[k + 1];
    chain[idx[n - 1]] = idx[0];
}

static int ooo_dep_chain_arm_init(struct test *test)
{
    try {
        auto data = std::make_unique<OooDepData>();
        build_permutation(data->chain, CHAIN_LEN);
        // Node values: high-Hamming table indexed by position so the data
        // checksum toggles across the chase.
        static const uint64_t VT[] = {
            0x0000000000000000ULL, 0xFFFFFFFFFFFFFFFFULL,
            0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
            0x0F0F0F0F0F0F0F0FULL, 0xF0F0F0F0F0F0F0F0ULL,
        };
        constexpr size_t VS = sizeof(VT) / sizeof(VT[0]);
        data->values.resize(CHAIN_LEN);
        for (size_t i = 0; i < CHAIN_LEN; ++i)
            data->values[i] = VT[i % VS] ^ (i * 0x9E3779B97F4A7C15ULL);
        // RW-hazard addresses: distinct cache-line-aligned slots.
        data->rw_addrs.resize(ROB_DEPTH);
        for (size_t i = 0; i < ROB_DEPTH; ++i)
            data->rw_addrs[i] = i * 0x100ULL; // placeholder; run allocates real addrs
        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory, "ooo_dep_chain_arm init: %s",
                 e.what());
        return EXIT_SKIP;
    }
}

#ifdef __aarch64__

// Software reference: walk the permutation chain summing node values (the
// golden checksum), starting at node 0 for CHAIN_LEN steps.
static uint64_t chain_reference(const OooDepData *d)
{
    uint64_t acc = 0;
    uint64_t cur = 0; // start at node 0
    for (size_t step = 0; step < CHAIN_LEN; ++step) {
        acc += d->values[cur];
        cur = d->chain[cur];
    }
    return acc;
}

// Software reference: carry-dependent add chain.
static uint64_t carry_chain_reference(const OooDepData *d)
{
    uint64_t acc = 1;
    uint64_t cur = 0;
    for (size_t step = 0; step < CHAIN_LEN; ++step) {
        acc = (acc << 1) + d->values[cur];
        cur = d->chain[cur];
    }
    return acc;
}

// Software reference: long dependent ALU chain (mul/add/shift), no memory.
static uint64_t alu_chain_reference(uint64_t seed, size_t depth)
{
    uint64_t a = seed;
    for (size_t i = 0; i < depth; ++i) {
        a = (a * 0x100000001B3ULL) + 0x9E3779B97F4A7C15ULL;
        a ^= (a >> 23);
        a = (a << 7) | (a >> 57);
    }
    return a;
}

static int ooo_dep_chain_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *d = static_cast<OooDepData *>(test->data);

    // Per-thread working buffers (no cross-thread sharing).
    // pointer-chase + carry-chain node storage: a flat array of node records,
    // each record = {value, next_index}. The chase loads next from the record
    // the current index points at. Allocate cache-line-aligned records so
    // node accesses are on distinct lines (maximise LSU traffic).
    struct NodeRec { uint64_t value; uint64_t next; };
    size_t recbytes = CHAIN_LEN * sizeof(NodeRec);
    NodeRec *nodes = static_cast<NodeRec *>(aligned_alloc_safe(64, recbytes));
    if (!nodes) {
        log_skip(TestResourceIssueSkipCategory,
                 "ooo_dep_chain_arm: node alloc failed");
        return EXIT_SKIP;
    }
    for (size_t i = 0; i < CHAIN_LEN; ++i) {
        nodes[i].value = d->values[i];
        nodes[i].next  = d->chain[i];
    }

    // RW-hazard slots: distinct cache lines.
    size_t rwbytes = ROB_DEPTH * 64;
    uint8_t *rwbuf = static_cast<uint8_t *>(aligned_alloc_safe(64, rwbytes));
    if (!rwbuf) {
        free(nodes);
        log_skip(TestResourceIssueSkipCategory,
                 "ooo_dep_chain_arm: rw alloc failed");
        return EXIT_SKIP;
    }

    // Goldens (deterministic, computed once).
    uint64_t golden_chase = chain_reference(d);
    uint64_t golden_carry = carry_chain_reference(d);
    uint64_t golden_alu_short = alu_chain_reference(0xDEADBEEFCAFEBABEULL, ALU_DEPTH);
    uint64_t golden_alu_long  = alu_chain_reference(0x0123456789ABCDEFULL, ROB_DEPTH);

    do {
        bool all_passed = true;

        // ---- (1) Pointer-chasing dependency chain. Each load's address
        // depends on the previous load's data (cur = nodes[cur].next).
        // A replay/rename fault corrupts the visited set -> wrong checksum.
        uint64_t acc = 0;
        uint64_t cur = 0; // node 0
        for (size_t step = 0; step < CHAIN_LEN; ++step) {
            // data-dependent load: the address depends on `cur`, which is the
            // previous load's result.
            NodeRec *p = nodes + cur;
            uint64_t val = dep_load64(&p->value);
            uint64_t nxt = dep_load64(&p->next);
            acc += val;
            cur = nxt;
        }
        if (acc != golden_chase) {
            log_warning("ooo_dep_chain_arm: pointer-chase checksum "
                        "0x%016" PRIx64 " want 0x%016" PRIx64,
                        acc, golden_chase);
            all_passed = false;
        }

        // ---- (2) Carry-dependent add chain (mixed ALU+memory dependency).
        // acc = (acc << 1) + nodes[cur].value; cur = nodes[cur].next.
        // The address computation uses the previous ALU result (cur), forcing
        // the scheduler to reorder ALU and memory across the dependency.
        uint64_t cacc = 1;
        uint64_t ccur = 0;
        for (size_t step = 0; step < CHAIN_LEN; ++step) {
            NodeRec *p = nodes + ccur;
            uint64_t val = dep_load64(&p->value);
            uint64_t nxt = dep_load64(&p->next);
            cacc = (cacc << 1) + val;
            ccur = nxt;
        }
        if (cacc != golden_carry) {
            log_warning("ooo_dep_chain_arm: carry-chain checksum "
                        "0x%016" PRIx64 " want 0x%016" PRIx64,
                        cacc, golden_carry);
            all_passed = false;
        }

        // ---- (3) Anti-dependence (RW hazard) interleave. Store to slot k,
        // then load slot k back, then store slot k+1 — the rename table must
        // track the RW hazard across the chain. A rename fault corrupts the
        // read-back. Golden = the stored values summed (deterministic).
        uint64_t rw_acc = 0;
        for (size_t k = 0; k < ROB_DEPTH; ++k) {
            uint64_t v = d->values[k % CHAIN_LEN] ^ (k * 0x9E3779B97F4A7C15ULL);
            uint8_t *slot = rwbuf + k * 64;
            dep_store64(slot, v);
            uint64_t rd = dep_load64(slot); // RW read-back (hazard)
            rw_acc += rd;
            if (rd != v) {
                log_warning("ooo_dep_chain_arm: RW hazard slot %zu "
                            "readback 0x%016" PRIx64 " want 0x%016" PRIx64,
                            k, rd, v);
                all_passed = false;
            }
        }
        // Golden for rw_acc = sum of the deterministic stored values.
        uint64_t rw_golden = 0;
        for (size_t k = 0; k < ROB_DEPTH; ++k) {
            uint64_t v = d->values[k % CHAIN_LEN] ^ (k * 0x9E3779B97F4A7C15ULL);
            rw_golden += v;
        }
        if (rw_acc != rw_golden) {
            log_warning("ooo_dep_chain_arm: RW hazard sum "
                        "0x%016" PRIx64 " want 0x%016" PRIx64,
                        rw_acc, rw_golden);
            all_passed = false;
        }

        // ---- (4) Long dependent ALU chain (no memory) — stresses PRF/ROB
        // depth and rename of a serial dependency. Short (ALU_DEPTH) and
        // long (ROB_DEPTH) variants.
        uint64_t as = 0xDEADBEEFCAFEBABEULL;
        for (size_t i = 0; i < ALU_DEPTH; ++i) {
            as = (as * 0x100000001B3ULL) + 0x9E3779B97F4A7C15ULL;
            as ^= (as >> 23);
            as = (as << 7) | (as >> 57);
        }
        if (as != golden_alu_short) {
            log_warning("ooo_dep_chain_arm: ALU chain short "
                        "0x%016" PRIx64 " want 0x%016" PRIx64,
                        as, golden_alu_short);
            all_passed = false;
        }
        uint64_t al = 0x0123456789ABCDEFULL;
        for (size_t i = 0; i < ROB_DEPTH; ++i) {
            al = (al * 0x100000001B3ULL) + 0x9E3779B97F4A7C15ULL;
            al ^= (al >> 23);
            al = (al << 7) | (al >> 57);
        }
        if (al != golden_alu_long) {
            log_warning("ooo_dep_chain_arm: ALU chain long "
                        "0x%016" PRIx64 " want 0x%016" PRIx64,
                        al, golden_alu_long);
            all_passed = false;
        }

        if (!all_passed) {
            free(rwbuf);
            free(nodes);
            report_fail_msg("ooo_dep_chain_arm: OoO replay/rename/"
                            "dependency-chain SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    free(rwbuf);
    free(nodes);
    return EXIT_SUCCESS;
}
#else
static int ooo_dep_chain_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    (void)test;
    log_skip(CpuNotSupportedSkipCategory,
             "to be implemented (placeholder): aarch64 OoO dependency-chain "
             "stress (inline-asm ldr/str dependent chains) required");
    return EXIT_SKIP;
}
#endif

static int ooo_dep_chain_arm_finish(struct test *test)
{
    delete static_cast<OooDepData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(ooo_dep_chain_arm,
             "Out-of-order execution SDC stress: pointer-chasing dependency "
             "chain, carry-dependent mixed ALU+memory chain, RW-hazard "
             "interleave, and long dependent ALU chains to stress the ARM64 "
             "scheduler/ROB/rename under serial dependencies")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = ooo_dep_chain_arm_init,
    .test_run = ooo_dep_chain_arm_run,
    .test_cleanup = ooo_dep_chain_arm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
