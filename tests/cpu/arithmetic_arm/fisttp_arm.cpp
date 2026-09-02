// 文件：tests/cpu/arithmetic_arm/fisttp_arm.cpp
/**
 * @copyright SPDX-License-Identifier: Apache-2.0
 *
 * @test fisttp_arm
 * @parblock
 * Floating-point to integer conversion (FCVTZS, round-toward-zero) via the Arm
 * Compute Library.  A fixed random table of float input (range [-100, 100]) is
 * generated at init and the golden int32 outputs are precomputed with
 * std::trunc.  Each run re-runs ACL's NECast (which issues FCVTZS) on the input
 * and compares the int32 output against the golden, plus a store/load
 * consistency check.  Pattern follows Intel OpenDCDiag's Eigen-based design.
 *
 * Per-thread ACL contexts (tensor + cast op) are preconfigured in init with
 * import_memory pointing at per-thread stack buffers, so ACL does not allocate
 * or own any memory that would need to survive sandstone's fork-per-iteration.
 *
 * Logging follows SDCShield convention: pass path is silent; on mismatch the
 * failing inputs and actual-vs-golden outputs are dumped via log_data() before
 * the thread is marked failed via report_fail_msg().
 * @endparblock
 */

#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#include <memory>
#include <cmath>
#include <thread>

// ACL 核心头文件
#include <arm_compute/runtime/NEON/NEFunctions.h>
#include <arm_compute/runtime/Scheduler.h>
#include <arm_compute/core/TensorInfo.h>

static constexpr size_t NUM_ELEMENTS = 1024;
static constexpr float FLOAT_MIN = -100.0f;
static constexpr float FLOAT_MAX = 100.0f;
static constexpr uint64_t FIXED_SEED = 0x123456789ABCDEF0ULL;

// 单个线程专属的资源上下文
struct PerThreadContext {
    alignas(128) float input[NUM_ELEMENTS];
    alignas(128) int32_t output[NUM_ELEMENTS];
    alignas(128) int32_t store_buf[NUM_ELEMENTS];
    alignas(128) int32_t reload_buf[NUM_ELEMENTS];

    arm_compute::Tensor src_tensor{};
    arm_compute::Tensor dst_tensor{};
    arm_compute::NECast cast_op{};
};

struct SharedTestData {
    std::vector<float> golden_input;
    std::vector<int32_t> golden_output;
    // 使用 unique_ptr 确保内存地址绝对固定，避免 resize/push_back 触发析构和深浅拷贝问题
    std::vector<std::unique_ptr<PerThreadContext>> thread_ctx;
};

static inline int32_t truncate_to_int(float val) {
    return static_cast<int32_t>(std::trunc(val));
}

// ============================================================================
// 初始化：生成固定随机输入，预计算黄金结果。所有线程共享同一份只读数据。
// 在单线程 init 阶段预先 configure 所有算子，确保多线程运行阶段零数据竞争。
// ============================================================================
static int fisttp_arm_init(struct test *test) {
    // 1. 禁用 ACL 内部多线程，防止 ACL 的内部工作线程与 Sandstone 的绑核线程发生竞争
    arm_compute::Scheduler::get().set_num_threads(1);

    auto *shared_data = new (std::nothrow) SharedTestData;
    if (!shared_data) return EXIT_FAILURE;

    shared_data->golden_input.resize(NUM_ELEMENTS);
    shared_data->golden_output.resize(NUM_ELEMENTS);

    int max_cpus = num_cpus();
    if (max_cpus <= 0) max_cpus = static_cast<int>(std::thread::hardware_concurrency());
    if (max_cpus <= 0) max_cpus = 256;

    shared_data->thread_ctx.reserve(max_cpus);

    std::mt19937_64 rng(FIXED_SEED);
    std::uniform_real_distribution<float> dist(FLOAT_MIN, FLOAT_MAX);

    for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
        shared_data->golden_input[i] = dist(rng);
        shared_data->golden_output[i] = truncate_to_int(shared_data->golden_input[i]);
    }

    // 2. 在单线程 Init 阶段预先初始化并 configure 所有算子，确保多线程运行阶段零数据竞争
    arm_compute::TensorInfo src_info(arm_compute::TensorShape(NUM_ELEMENTS), 1, arm_compute::DataType::F32);
    arm_compute::TensorInfo dst_info(arm_compute::TensorShape(NUM_ELEMENTS), 1, arm_compute::DataType::S32);

    for (int i = 0; i < max_cpus; ++i) {
        auto ctx = std::make_unique<PerThreadContext>();

        ctx->src_tensor.allocator()->init(src_info);
        ctx->dst_tensor.allocator()->init(dst_info);

        ctx->src_tensor.allocator()->import_memory(ctx->input);
        ctx->dst_tensor.allocator()->import_memory(ctx->output);

        // 核心安全保证：configure 仅在 init 阶段单线程调用一次
        ctx->cast_op.configure(&ctx->src_tensor, &ctx->dst_tensor, arm_compute::ConvertPolicy::WRAP);

        shared_data->thread_ctx.push_back(std::move(ctx));
    }

    test->data = shared_data;
    return EXIT_SUCCESS;
}

// 运行测试：每次迭代调用 ACL NECast (FCVTZS) 并与 golden 比较
static int fisttp_arm_run(struct test *test, int cpu) {
    auto *shared_data = static_cast<SharedTestData*>(test->data);
    if (!shared_data) return EXIT_FAILURE;

    size_t cpu_idx = (cpu >= 0 && static_cast<size_t>(cpu) < shared_data->thread_ctx.size())
                     ? static_cast<size_t>(cpu) : 0;
    auto &ctx = *shared_data->thread_ctx[cpu_idx];

    do {
        // 1. 输入数据准备
        std::memcpy(ctx.input, shared_data->golden_input.data(), NUM_ELEMENTS * sizeof(float));

        // 2. 压测运行：仅调用预先 configure 好的 run() 函数
        ctx.cast_op.run();

        // 3. 结果校验（FCVTZS 转换为整数比对 + Cache 一致性比对）
        bool data_ok = (std::memcmp(ctx.output, shared_data->golden_output.data(), NUM_ELEMENTS * sizeof(int32_t)) == 0);

        std::memcpy(ctx.store_buf, ctx.output, NUM_ELEMENTS * sizeof(int32_t));
        std::memcpy(ctx.reload_buf, ctx.store_buf, NUM_ELEMENTS * sizeof(int32_t));
        bool consistent = (std::memcmp(ctx.reload_buf, ctx.output, NUM_ELEMENTS * sizeof(int32_t)) == 0);

        if (!data_ok || !consistent) {
            // Fail 详查：把本次输入与输出落进 yaml 的 data: 字段，再用 report_fail_msg
            // 记录失败位置（框架自动标 thread failed）。report_fail_msg 是 noreturn。
            // 定位首个不符元素
            int mismatch_idx = -1;
            for (size_t i = 0; i < NUM_ELEMENTS; ++i) {
                if (ctx.output[i] != shared_data->golden_output[i]) { mismatch_idx = (int)i; break; }
            }
            char ctx_msg[200];
            snprintf(ctx_msg, sizeof(ctx_msg),
                     "fisttp_arm (ACL): conversion mismatch on CPU %d at idx %d "
                     "(out=%d golden=%d, data_ok=%d, consistent=%d)",
                     cpu, mismatch_idx,
                     mismatch_idx >= 0 ? ctx.output[mismatch_idx] : 0,
                     mismatch_idx >= 0 ? shared_data->golden_output[mismatch_idx] : 0,
                     (int)data_ok, (int)consistent);
            log_data("fisttp input (float, 1024 elems)",
                     ctx.input, NUM_ELEMENTS * sizeof(float));
            log_data("fisttp output (int32 ACL FCVTZS result, 1024 elems)",
                     ctx.output, NUM_ELEMENTS * sizeof(int32_t));
            log_data("fisttp golden (std::trunc int32 result, 1024 elems)",
                     shared_data->golden_output.data(), NUM_ELEMENTS * sizeof(int32_t));
            report_fail_msg("%s", ctx_msg);
            // report_fail_msg 不返回
        }

    } while (test_time_condition(test));

    return EXIT_SUCCESS;
}

static int fisttp_arm_finish(struct test *test) {
    auto *shared_data = static_cast<SharedTestData*>(test->data);
    if (shared_data) {
        delete shared_data;
    }
    test->data = nullptr;
    return EXIT_SUCCESS;
}

// ============================================================================
// 测试声明
// ============================================================================
DECLARE_TEST(fisttp_arm, "Floating-point to integer conversion (FCVTZS) via Arm Compute Library")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = fisttp_arm_init,
    .test_run = fisttp_arm_run,
    .test_cleanup = fisttp_arm_finish,
    .quality_level = TEST_QUALITY_PROD
END_DECLARE_TEST
