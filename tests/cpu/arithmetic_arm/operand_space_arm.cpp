/**
 * @file operand_space_arm.cpp
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test operand_space_arm
 * @parblock
 * Operand-space SDC stressor: feeds the ARM64 adder / carry-chain / multiplier
 * a *deterministic high-Hamming-distance operand table* that forces maximum
 * per-cycle gate toggling (0x0000... <-> 0xFFFF... alternating, 0xAAAA... <->
 * 0x5555..., walking-one/zero), instead of uniform-random inputs.
 *
 * Rationale (SDC coverage gap, "版图一.1 操作数空间"): every existing integer
 * test in this suite feeds the ALU uniform-random 64-bit operands
 * (random32()/mpz_urandomb/mt19937), which average ~16 bits of Hamming distance
 * between successive values and rarely hit the worst-case carry-propagation /
 * prefix-adder / multiplier partial-product paths. Alternating between bitwise
 * complements (Hamming distance 64) every cycle forces every gate in the
 * carry/sum network to toggle 0->1 and 1->0, which is the pattern most likely
 * to excite delay faults and produce silent data corruption.
 *
 * The hardware path runs the big-integer add through GMP (mp_add, which lowers
 * to ADCS), and a 64x64->128 UMULL via mpz_mul; the golden is precomputed in
 * test_init using pure __int128 software simulation. Each run compares the
 * hardware result against the golden with a per-word byte-exact check, a
 * real store-to-load-forwarding probe (inline-asm str then ldr to the same
 * buffer — the store buffer satisfying the subsequent load, a corruption-prone
 * datapath), and a full 128-bit multiply check (both UMULL-low and UMULH-high
 * halves, the high half being a timing-critical path). No per-iteration
 * logging (avoids the timing-perturbation hazard seen in adcx_arm.cpp:186 /
 * adcxlong.cpp:162).
 *
 * ARM64-native (GMP is consumed on aarch64); the whole subdir is entered only
 * under the aarch64 guard in tests/cpu/meson.build, so x86-64 is untouched.
 * @endparblock
 */

#include <sandstone.h>
#include <gmp.h>
#include <cstdint>
#include <cinttypes>
#include <cstring>
#include <memory>

// 256-bit big integer = 4 x 64-bit words.
static constexpr size_t NUM_WORDS = 4;
// Number of (a, b) operand pairs derived from the high-Hamming table.
// Set to the table size so one pass cycles through every entry (the high-bit
// walking-one values at the tail of the table are otherwise unreached in a
// NUM_PAIRS < table-size cycle).
static constexpr size_t NUM_PAIRS = 80;

// ============================================================================
// High-Hamming-distance operand table.
//
// Each entry is a 64-bit word. Successive entries are chosen to maximise the
// number of bits that change between one operand and the next, forcing maximum
// gate toggling in the carry/sum/partial-product networks. A full big integer
// is built by replicating a chosen base word across all NUM_WORDS lanes (so a
// 0xFFFF...F word makes a 256-bit all-ones value), which keeps the carry chain
// fully exercised across all lanes.
// ============================================================================
static const uint64_t HAMMING_TABLE[] = {
    0x0000000000000000ULL,
    0xFFFFFFFFFFFFFFFFULL,   // Hamming distance 64 vs previous (max toggle)
    0xAAAAAAAAAAAAAAAAULL,   // even bits
    0x5555555555555555ULL,    // odd bits  (distance 64 vs previous)
    0x0F0F0F0F0F0F0F0FULL,
    0xF0F0F0F0F0F0F0F0ULL,   // distance 64 vs previous
    0x00FF00FF00FF00FFULL,
    0xFF00FF00FF00FF00ULL,    // distance 64 vs previous
    0x0000FFFF0000FFFFULL,
    0xFFFF0000FFFF0000ULL,    // distance 64 vs previous
    0x00000000FFFFFFFFULL,
    0xFFFFFFFF00000000ULL,    // distance 64 vs previous
    0x123456789ABCDEF0ULL,    // structured mixed pattern
    0xEDCBA9876543210ULL,     // bitwise complement of above (distance 64)
    0x0123456789ABCDEFULL,
    0xFEDCBA9876543210ULL,    // complement (distance 64)
    // walking-one: a single bit set, successive entries shift the bit, so the
    // carry chain sees a single hot bit walking through all 64 positions.
    0x0000000000000001ULL,
    0x0000000000000002ULL,
    0x0000000000000004ULL,
    0x0000000000000008ULL,
    0x0000000000000010ULL,
    0x0000000000000020ULL,
    0x0000000000000040ULL,
    0x0000000000000080ULL,
    0x0000000000000100ULL,
    0x0000000000000200ULL,
    0x0000000000000400ULL,
    0x0000000000000800ULL,
    0x0000000000001000ULL,
    0x0000000000002000ULL,
    0x0000000000004000ULL,
    0x0000000000008000ULL,
    0x0000000000010000ULL,
    0x0000000000020000ULL,
    0x0000000000040000ULL,
    0x0000000000080000ULL,
    0x0000000000100000ULL,
    0x0000000000200000ULL,
    0x0000000000400000ULL,
    0x0000000000800000ULL,
    0x0000000001000000ULL,
    0x0000000002000000ULL,
    0x0000000004000000ULL,
    0x0000000008000000ULL,
    0x0000000010000000ULL,
    0x0000000020000000ULL,
    0x0000000040000000ULL,
    0x0000000080000000ULL,
    0x0000000100000000ULL,
    0x0000000200000000ULL,
    0x0000000400000000ULL,
    0x0000000800000000ULL,
    0x0000001000000000ULL,
    0x0000002000000000ULL,
    0x0000004000000000ULL,
    0x0000008000000000ULL,
    0x0000010000000000ULL,
    0x0000020000000000ULL,
    0x0000040000000000ULL,
    0x0000080000000000ULL,
    0x0000100000000000ULL,
    0x0000200000000000ULL,
    0x0000400000000000ULL,
    0x0000800000000000ULL,
    0x0001000000000000ULL,
    0x0002000000000000ULL,
    0x0004000000000000ULL,
    0x0008000000000000ULL,
    0x0010000000000000ULL,
    0x0020000000000000ULL,
    0x0040000000000000ULL,
    0x0080000000000000ULL,
    0x0100000000000000ULL,
    0x0200000000000000ULL,
    0x0400000000000000ULL,
    0x0800000000000000ULL,
    0x1000000000000000ULL,
    0x2000000000000000ULL,
    0x4000000000000000ULL,
    0x8000000000000000ULL,
};
static constexpr size_t HAMMING_TABLE_SIZE = sizeof(HAMMING_TABLE) / sizeof(HAMMING_TABLE[0]);

struct OperandSpaceData {
    std::vector<uint64_t> a_words;        // NUM_PAIRS * NUM_WORDS
    std::vector<uint64_t> b_words;       // NUM_PAIRS * NUM_WORDS
    std::vector<uint64_t> golden_add;     // NUM_PAIRS * (NUM_WORDS + 1)  (with carry-out)
    std::vector<uint64_t> golden_mul_lo;  // NUM_PAIRS (low 64 bits of a[0]*b[0])
    std::vector<uint64_t> golden_mul_hi;  // NUM_PAIRS (high 64 bits of a[0]*b[0]) — UMULH path
    size_t num_pairs;
};

// ----------------------------------------------------------------------------
// Software reference: 256-bit big-integer add with carry propagation, using
// __int128 to model the ADCS carry chain. Emits NUM_WORDS + 1 words (the extra
// word holds carry-out).
// ----------------------------------------------------------------------------
static void software_add_words(const uint64_t *a, const uint64_t *b,
                               uint64_t *res)
{
    unsigned __int128 carry = 0;
    for (size_t i = 0; i < NUM_WORDS; ++i) {
        unsigned __int128 sum = (unsigned __int128)a[i] + b[i] + carry;
        res[i] = (uint64_t)sum;
        carry = sum >> 64;
    }
    res[NUM_WORDS] = (uint64_t)carry;
}

// Build a 256-bit operand by replicating base_word across all NUM_WORDS lanes.
// (Replication keeps every lane of the carry chain fully toggling when the
// base word flips, rather than leaving high lanes quiescent.)
static inline void fill_operand(uint64_t *dst, uint64_t base_word)
{
    for (size_t i = 0; i < NUM_WORDS; ++i)
        dst[i] = base_word;
}

// ----------------------------------------------------------------------------
// Store-to-load-forwarding probe. The previous version did a software
// memcpy(store_buf, hw) then memcpy(reload_buf, store_buf) and compared — that
// is a memory-to-memory copy through the compiler/regalloc, NOT a hardware
// store->load forward; the store-load forwarding datapath (the store buffer
// satisfying a subsequent load to the same address before it hits L1) was never
// actually exercised. Here we issue a real store then a real load to the same
// address via inline asm with a "memory" clobber, defeating the optimizer so
// the store and load are emitted as separate str/ldr micro-ops hitting the
// store buffer. On non-aarch64 we fall back to a plain store+load (still a real
// memory round-trip, unlike the memcpy version).
// ----------------------------------------------------------------------------
#if defined(__aarch64__)
static inline uint64_t slf_probe(const uint64_t *src, uint64_t *buf, size_t n)
{
    // 1. Copy src -> buf via stores (str).
    for (size_t i = 0; i < n; ++i) {
        __asm__ volatile("str %1, [%0, %2]"
                         : : "r"(buf), "r"(src[i]), "r"(i * sizeof(uint64_t))
                         : "memory");
    }
    // 2. Sum-reload every element via loads (ldr) from buf — each load is
    //    satisfied by the just-written store buffer entry (store-to-load
    //    forwarding). A fault in the forwarding path corrupts the sum.
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i) {
        uint64_t v;
        __asm__ volatile("ldr %0, [%1, %2]"
                         : "=r"(v)
                         : "r"(buf), "r"(i * sizeof(uint64_t))
                         : "memory");
        acc += v;
    }
    return acc;
}
#else
static inline uint64_t slf_probe(const uint64_t *src, uint64_t *buf, size_t n)
{
    for (size_t i = 0; i < n; ++i)
        buf[i] = src[i];
    uint64_t acc = 0;
    for (size_t i = 0; i < n; ++i)
        acc += buf[i];
    return acc;
}
#endif

static int operand_space_arm_init(struct test *test)
{
    try {
        auto data = std::make_unique<OperandSpaceData>();
        data->num_pairs = NUM_PAIRS;
        data->a_words.resize(NUM_PAIRS * NUM_WORDS);
        data->b_words.resize(NUM_PAIRS * NUM_WORDS);
        data->golden_add.resize(NUM_PAIRS * (NUM_WORDS + 1));
        data->golden_mul_lo.resize(NUM_PAIRS);
        data->golden_mul_hi.resize(NUM_PAIRS);

        // Generate operand pairs by cycling through the high-Hamming table.
        // Successive pairs pick complementary table entries (i and i+1 mod N)
        // so that consecutive iterations see a full bitwise complement and the
        // carry chain toggles maximally.
        for (size_t i = 0; i < NUM_PAIRS; ++i) {
            uint64_t a_base = HAMMING_TABLE[i % HAMMING_TABLE_SIZE];
            uint64_t b_base = HAMMING_TABLE[(i + 1) % HAMMING_TABLE_SIZE];

            uint64_t a[NUM_WORDS], b[NUM_WORDS];
            fill_operand(a, a_base);
            fill_operand(b, b_base);

            memcpy(&data->a_words[i * NUM_WORDS], a, NUM_WORDS * sizeof(uint64_t));
            memcpy(&data->b_words[i * NUM_WORDS], b, NUM_WORDS * sizeof(uint64_t));

            // Golden: 256-bit add (with carry-out).
            uint64_t sum[NUM_WORDS + 1];
            software_add_words(a, b, sum);
            memcpy(&data->golden_add[i * (NUM_WORDS + 1)], sum,
                   (NUM_WORDS + 1) * sizeof(uint64_t));

            // Golden: full 64x64->128 multiply low word (UMULL low + UMULH high).
            // Keeping BOTH halves exercises the full multiplier partial-product
            // network — the high 64 bits are non-trivial (e.g. 0xFF..*0xFF.. ->
            // hi=0xFF..FE) and are a timing-critical path the low-only check
            // missed entirely.
            unsigned __int128 prod = (unsigned __int128)a[0] * (unsigned __int128)b[0];
            data->golden_mul_lo[i] = (uint64_t)prod;
            data->golden_mul_hi[i] = (uint64_t)(prod >> 64);
        }

        test->data = data.release();
        return EXIT_SUCCESS;
    } catch (const std::exception &e) {
        log_skip(TestResourceIssueSkipCategory, "operand_space_arm init exception: %s", e.what());
        return EXIT_SKIP;
    }
}

static int operand_space_arm_run(struct test *test, int cpu)
{
    (void)cpu;
    auto *td = static_cast<OperandSpaceData *>(test->data);

    // Per-thread buffers (no cross-thread sharing).
    uint64_t a_words[NUM_WORDS];
    uint64_t b_words[NUM_WORDS];
    uint64_t hw_words[NUM_WORDS + 1];

    mpz_t a_mpz, b_mpz, result_hw;
    mpz_init(a_mpz);
    mpz_init(b_mpz);
    mpz_init(result_hw);

    do {
        bool all_passed = true;

        for (size_t i = 0; i < td->num_pairs; ++i) {
            memcpy(a_words, &td->a_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));
            memcpy(b_words, &td->b_words[i * NUM_WORDS], NUM_WORDS * sizeof(uint64_t));

            // ---- Hardware path: 256-bit add via GMP mpz_add (lowers to ADCS).
            mpz_import(a_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, a_words);
            mpz_import(b_mpz, NUM_WORDS, -1, sizeof(uint64_t), 0, 0, b_words);
            mpz_add(result_hw, a_mpz, b_mpz);

            memset(hw_words, 0, (NUM_WORDS + 1) * sizeof(uint64_t));
            size_t count;
            mpz_export(hw_words, &count, -1, sizeof(uint64_t), 0, 0, result_hw);
            // (count <= NUM_WORDS+1; hw_words already zero-extended above.)

            // ---- Golden compare (byte-exact, per word, including carry-out).
            const uint64_t *golden = &td->golden_add[i * (NUM_WORDS + 1)];
            bool data_ok = (memcmp(hw_words, golden, (NUM_WORDS + 1) * sizeof(uint64_t)) == 0);

            // ---- Store-to-load-forwarding probe. Issue real str then ldr to
            // the same buffer (inline asm + "memory" clobber so the optimizer
            // cannot fuse/elide them); the reloads are satisfied by the store
            // buffer (store-to-load forwarding), a corruption-prone datapath the
            // previous software memcpy never touched. Golden = sum of the add
            // result words; a corrupted forward produces a different sum.
            uint64_t slf_buf[NUM_WORDS + 1];
            uint64_t slf_golden = 0;
            for (size_t w = 0; w < NUM_WORDS + 1; ++w)
                slf_golden += hw_words[w];
            uint64_t slf_got = slf_probe(hw_words, slf_buf, NUM_WORDS + 1);
            bool consistent = (slf_got == slf_golden);

            // ---- Hardware path: full 64x64->128 multiply via mpz_mul on the
            // low limbs. mpz_mul of two 1-limb values exercises the 64x64
            // multiplier; we export BOTH 64-bit halves and check each against
            // the golden — the high half (UMULH) is a timing-critical path
            // the low-only check missed entirely.
            mpz_t a_lo, b_lo, mul_full;
            mpz_init(a_lo); mpz_init(b_lo); mpz_init(mul_full);
            mpz_import(a_lo, 1, -1, sizeof(uint64_t), 0, 0, &a_words[0]);
            mpz_import(b_lo, 1, -1, sizeof(uint64_t), 0, 0, &b_words[0]);
            mpz_mul(mul_full, a_lo, b_lo);
            // 128-bit export into two little-endian limbs.
            uint64_t hw_mul[2] = {0, 0};
            size_t mcount;
            mpz_export(hw_mul, &mcount, -1, sizeof(uint64_t), 0, 0, mul_full);
            bool mul_lo_ok = (hw_mul[0] == td->golden_mul_lo[i]);
            bool mul_hi_ok = (hw_mul[1] == td->golden_mul_hi[i]);
            bool mul_ok = (mul_lo_ok && mul_hi_ok);
            mpz_clear(a_lo); mpz_clear(b_lo); mpz_clear(mul_full);

            if (!(data_ok && consistent && mul_ok)) {
                all_passed = false;
                // One-shot diagnostic log (NOT per-iteration flooding — avoids
                // the timing-perturbation hazard seen in adcx_arm.cpp:186).
                log_warning("operand_space_arm: mismatch at pair %zu "
                            "(add_ok=%d slf_ok=%d mul_lo_ok=%d mul_hi_ok=%d): "
                            "a0=0x%016" PRIx64 " b0=0x%016" PRIx64 " "
                            "hw_add_hi=0x%016" PRIx64 " golden_add_hi=0x%016" PRIx64 " "
                            "hw_mul_lo=0x%016" PRIx64 " golden_mul_lo=0x%016" PRIx64 " "
                            "hw_mul_hi=0x%016" PRIx64 " golden_mul_hi=0x%016" PRIx64,
                            i, data_ok, consistent, mul_lo_ok, mul_hi_ok,
                            a_words[0], b_words[0],
                            hw_words[NUM_WORDS], golden[NUM_WORDS],
                            hw_mul[0], td->golden_mul_lo[i],
                            hw_mul[1], td->golden_mul_hi[i]);
            }
        }

        if (!all_passed) {
            mpz_clear(a_mpz); mpz_clear(b_mpz); mpz_clear(result_hw);
            report_fail_msg("operand_space_arm: high-Hamming operand SDC detected");
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    mpz_clear(a_mpz);
    mpz_clear(b_mpz);
    mpz_clear(result_hw);
    return EXIT_SUCCESS;
}

static int operand_space_arm_finish(struct test *test)
{
    delete static_cast<OperandSpaceData *>(test->data);
    return EXIT_SUCCESS;
}

DECLARE_TEST(operand_space_arm,
             "Operand-space SDC stress: high-Hamming-distance operands forcing "
             "maximum adder/carry-chain/multiplier gate toggling (ARM64 ADCS/UMULL via GMP)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = operand_space_arm_init,
    .test_run = operand_space_arm_run,
    .test_cleanup = operand_space_arm_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
