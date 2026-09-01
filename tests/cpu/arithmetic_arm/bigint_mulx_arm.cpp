// 文件：tests/cpu/arithmetic_arm/bigint_mulx_arm.cpp
/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test bigint_mulx_arm
 * @parblock
 * 512-bit large integer multiplication via the GMP library (ARM64).
 * The golden low-half product is precomputed in init (GMP mpz_mul), and every
 * run recomputes the product with GMP (which exercises UMULL 64x64->128 inside
 * mpn_mul_basecase) and compares the low 512 bits against the golden. Pattern
 * follows Intel OpenDCDiag's Eigen-based design.
 *
 * Logging follows SDCShield convention: pass path is silent; on mismatch the
 * failing inputs (a, b) and actual-vs-golden outputs are dumped via log_data()
 * before the thread is marked failed via report_fail_msg().
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <gmp.h>

static constexpr int NUM_WORDS = 8;          // 512 位 = 8 × 64 位
static constexpr int BITS = 512;
static constexpr uint64_t FIXED_SEED = 0x123456789ABCDEF0ULL;

struct TestData {
    std::vector<uint64_t> a_words;
    std::vector<uint64_t> b_words;
    std::vector<uint64_t> golden_product;    // 乘积的低 NUM_WORDS 个字
};

// 辅助：GMP -> uint64_t 数组（小端序），只取低 low_words 个字
static void mpz_to_words_trunc(uint64_t *words, const mpz_t src, int num_words) {
    // 截断 src 到 num_words * 64 位
    mpz_t tmp;
    mpz_init(tmp);
    mpz_mod_2exp(tmp, src, num_words * 64);
    memset(words, 0, num_words * sizeof(uint64_t));
    mpz_export(words, nullptr, -1, sizeof(uint64_t), 0, 0, tmp);
    mpz_clear(tmp);
}

// 辅助：uint64_t 数组 -> GMP
static void words_to_mpz(mpz_t dst, const uint64_t *words, int num_words) {
    mpz_import(dst, num_words, -1, sizeof(uint64_t), 0, 0, words);
}

// 初始化：生成固定随机输入，预计算黄金结果。所有线程共享同一份只读数据。
static int bigint_mulx_arm_init(struct test *test) {
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    data->a_words.resize(NUM_WORDS);
    data->b_words.resize(NUM_WORDS);
    data->golden_product.resize(NUM_WORDS);

    std::mt19937_64 rng(FIXED_SEED);
    std::uniform_int_distribution<uint64_t> dist(0, UINT64_MAX);

    for (int i = 0; i < NUM_WORDS; ++i) {
        data->a_words[i] = dist(rng);
        data->b_words[i] = dist(rng);
    }

    // 使用 GMP 计算精确乘积
    mpz_t a, b, product;
    mpz_init(a);
    mpz_init(b);
    mpz_init(product);

    words_to_mpz(a, data->a_words.data(), NUM_WORDS);
    words_to_mpz(b, data->b_words.data(), NUM_WORDS);
    mpz_mul(product, a, b);

    // 提取乘积的低 NUM_WORDS 个字（512 位）
    mpz_to_words_trunc(data->golden_product.data(), product, NUM_WORDS);

    mpz_clear(a);
    mpz_clear(b);
    mpz_clear(product);

    test->data = data;
    return EXIT_SUCCESS;
}

// 运行测试：每次迭代计算并比较，使用线程局部缓冲
static int bigint_mulx_arm_run(struct test *test, int cpu) {
    (void)cpu;
    auto *data = static_cast<TestData*>(test->data);
    if (!data) return EXIT_FAILURE;

    const uint64_t *a = data->a_words.data();
    const uint64_t *b = data->b_words.data();

    mpz_t a_mpz, b_mpz, product;
    mpz_init(a_mpz);
    mpz_init(b_mpz);
    mpz_init(product);

    do {
        // 使用 GMP 重新计算乘积
        words_to_mpz(a_mpz, a, NUM_WORDS);
        words_to_mpz(b_mpz, b, NUM_WORDS);
        mpz_mul(product, a_mpz, b_mpz);

        uint64_t result[NUM_WORDS];
        mpz_to_words_trunc(result, product, NUM_WORDS);

        // 与黄金结果比较
        bool data_ok = true;
        int mismatch_word = 0;
        for (int i = 0; i < NUM_WORDS; ++i) {
            if (result[i] != data->golden_product[i]) {
                data_ok = false;
                mismatch_word = i;
                break;
            }
        }

        // 存储一致性测试
        uint64_t store_buf[NUM_WORDS];
        memcpy(store_buf, result, sizeof(store_buf));
        uint64_t reload_buf[NUM_WORDS];
        memcpy(reload_buf, store_buf, sizeof(store_buf));
        bool consistent = (memcmp(reload_buf, result, sizeof(result)) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            // Fail 详查：把本次输入(a,b)与输出(result,golden)落进 yaml 的 data: 字段，
            // 再用 report_fail_msg 记录失败位置（框架自动标 thread failed）。
            // report_fail_msg 是 noreturn，调用前先释放 GMP 资源。
            char ctx[160];
            snprintf(ctx, sizeof(ctx),
                     "bigint_mulx_arm: word %d mismatch (result=0x%016llX "
                     "golden=0x%016llX) consistent=%d",
                     mismatch_word,
                     (unsigned long long)result[mismatch_word],
                     (unsigned long long)data->golden_product[mismatch_word],
                     (int)consistent);
            log_data("bigint_mulx input a (512-bit LE limbs)", a, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx input b (512-bit LE limbs)", b, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx output result (mpz_mul low 512-bit)", result, NUM_WORDS * sizeof(uint64_t));
            log_data("bigint_mulx golden (precomputed low 512-bit)",
                     data->golden_product.data(), NUM_WORDS * sizeof(uint64_t));
            mpz_clear(a_mpz);
            mpz_clear(b_mpz);
            mpz_clear(product);
            report_fail_msg("%s", ctx);
            // report_fail_msg 不返回
        }

    } while (test_time_condition(test));

    mpz_clear(a_mpz);
    mpz_clear(b_mpz);
    mpz_clear(product);
    return EXIT_SUCCESS;
}

static int bigint_mulx_arm_finish(struct test *test) {
    auto *data = static_cast<TestData*>(test->data);
    delete data;
    test->data = nullptr;
    return EXIT_SUCCESS;
}

DECLARE_TEST(bigint_mulx_arm,
             "512b integer multiplication using GMP (ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = bigint_mulx_arm_init,
    .test_run = bigint_mulx_arm_run,
    .test_cleanup = bigint_mulx_arm_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
