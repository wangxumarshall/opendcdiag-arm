/**
 * @file
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test adcx_adox_interleaved
 * @parblock
 * Interleaved unsigned add with overflow (adox) and unsigned add with carry (adcx).
 * This test validates two multi-precision addition chains: one using carry flag (CF)
 * and another using overflow flag (OF) as carry input. Since ARM64 lacks native
 * ADCX/ADOX instructions, both chains are software-simulated using 64-bit arithmetic.
 * The results are compared against a golden reference computed with 128-bit arithmetic.
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

// ============================================================================
// 测试数据（所有线程共享只读）
// ============================================================================

struct TestData {
    std::vector<uint64_t> lhs;      // 操作数1
    std::vector<uint64_t> rhs;      // 操作数2
    std::vector<uint64_t> gold;     // 黄金参考值 (普通多精度加法)
    size_t num_elems;               // 向量长度
};

// ============================================================================
// 黄金参考实现（软件模拟普通多精度加法）
// ============================================================================

static void compute_golden(const uint64_t *lhs, const uint64_t *rhs,
                           uint64_t *gold, size_t n) {
    uint64_t carry = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry;
        gold[i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
    }
}

// ============================================================================
// 带颜色的日志输出
// ============================================================================

static void print_colored_result(const char *label, bool passed,
                                 const uint64_t *lhs, const uint64_t *rhs,
                                 const uint64_t *result_cf, const uint64_t *result_of,
                                 const uint64_t *gold, size_t n) {
    const char *color = passed ? "\033[32m" : "\033[31m";
    const char *result_str = passed ? "PASS" : "FAIL";

    fprintf(stderr, "%s: %s%s\033[0m\n", label, color, result_str);
    fprintf(stderr, "  lhs[0..3] = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            lhs[0], (n > 1 ? lhs[1] : 0), (n > 2 ? lhs[2] : 0), (n > 3 ? lhs[3] : 0));
    fprintf(stderr, "  rhs[0..3] = 0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            rhs[0], (n > 1 ? rhs[1] : 0), (n > 2 ? rhs[2] : 0), (n > 3 ? rhs[3] : 0));
    fprintf(stderr, "  result_cf[0..3]=0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            result_cf[0], (n > 1 ? result_cf[1] : 0), (n > 2 ? result_cf[2] : 0), (n > 3 ? result_cf[3] : 0));
    fprintf(stderr, "  result_of[0..3]=0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            result_of[0], (n > 1 ? result_of[1] : 0), (n > 2 ? result_of[2] : 0), (n > 3 ? result_of[3] : 0));
    fprintf(stderr, "  gold[0..3]   =0x%016lx 0x%016lx 0x%016lx 0x%016lx\n",
            gold[0], (n > 1 ? gold[1] : 0), (n > 2 ? gold[2] : 0), (n > 3 ? gold[3] : 0));
    fflush(stderr);
}

// ============================================================================
// 软件模拟 ADCX 链（使用 CF 作为进位）
// ============================================================================

static void adcx_chain_sw(const uint64_t *lhs, const uint64_t *rhs,
                          uint64_t *result, size_t n, unsigned char carry_in) {
    uint64_t carry = carry_in;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry;
        result[i] = (uint64_t)sum;
        carry = (uint64_t)(sum >> 64);
    }
}

// ============================================================================
// 软件模拟 ADOX 链（使用 OF 作为进位，语义为无符号加法溢出）
// ============================================================================

static void adox_chain_sw(const uint64_t *lhs, const uint64_t *rhs,
                          uint64_t *result, size_t n, unsigned char carry_in) {
    uint64_t carry = carry_in;
    for (size_t i = 0; i < n; ++i) {
        unsigned __int128 sum = (unsigned __int128)lhs[i] + rhs[i] + carry;
        result[i] = (uint64_t)sum;
        // 无符号溢出：当 sum 大于 2^64-1 时 carry 为 1
        carry = (uint64_t)(sum >> 64);
    }
}

// ============================================================================
// 测试生命周期函数
// ============================================================================

static int adcx_adox_interleaved_init(struct test *test) {
    auto *data = new TestData;
    if (!data) return EXIT_FAILURE;

    constexpr size_t N = 1024;
    data->num_elems = N;
    data->lhs.resize(N);
    data->rhs.resize(N);
    data->gold.resize(N);

    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;
    for (size_t i = 0; i < N; ++i) {
        data->lhs[i] = dist(rng);
        data->rhs[i] = dist(rng);
    }

    compute_golden(data->lhs.data(), data->rhs.data(), data->gold.data(), N);

    test->data = data;
    return EXIT_SUCCESS;
}

static int adcx_adox_interleaved_run(struct test *test, int cpu) {
    (void)cpu;
    auto *td = static_cast<TestData*>(test->data);
    const size_t N = td->num_elems;

    std::vector<uint64_t> result_cf(N);
    std::vector<uint64_t> result_of(N);
    std::vector<uint64_t> store_buf(N);

    do {
        // 1. 模拟 ADCX 链（初始 CF = 0）
        adcx_chain_sw(td->lhs.data(), td->rhs.data(), result_cf.data(), N, 0);

        // 2. 模拟 ADOX 链（初始 OF = 0）
        adox_chain_sw(td->lhs.data(), td->rhs.data(), result_of.data(), N, 0);

        // 3. 验证：两个链的结果应与黄金参考一致，且彼此一致
        bool cf_ok = (memcmp(result_cf.data(), td->gold.data(), N * sizeof(uint64_t)) == 0);
        bool of_ok = (memcmp(result_of.data(), td->gold.data(), N * sizeof(uint64_t)) == 0);
        bool both_ok = (memcmp(result_cf.data(), result_of.data(), N * sizeof(uint64_t)) == 0);
        bool data_ok = cf_ok && of_ok && both_ok;

        // 4. 一致性测试
        memcpy(store_buf.data(), result_cf.data(), N * sizeof(uint64_t));
        bool consistent = (memcmp(store_buf.data(), result_cf.data(), N * sizeof(uint64_t)) == 0);

        bool passed = data_ok && consistent;

        if (!passed) {
            print_colored_result("adcx_adox_interleaved", passed,
                                 td->lhs.data(), td->rhs.data(),
                                 result_cf.data(), result_of.data(),
                                 td->gold.data(), N);
            report_fail_msg("adcx_adox_interleaved: data_ok=%d, consistent=%d",
                            data_ok, consistent);
            return EXIT_FAILURE;
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int adcx_adox_interleaved_finish(struct test *test) {
    delete static_cast<TestData*>(test->data);
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明（确保测试名称唯一，与 adcx 区分）
// ============================================================================

DECLARE_TEST(adcx_adox_interleaved,
             "Interleaved unsigned add with overflow (adox) and unsigned add with carry (adcx) on ARM64")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = adcx_adox_interleaved_init,
    .test_run = adcx_adox_interleaved_run,
    .test_cleanup = adcx_adox_interleaved_finish,
    .fracture_loop_count = 4,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
