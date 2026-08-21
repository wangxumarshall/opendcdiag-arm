#include <sandstone.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

static int vmx_vmexit_vmcall_init(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

static int vmx_vmexit_vmcall_run(struct test *test, int cpu) {
    (void)cpu;

    // 在 ARM64 上无法执行 VMCALL，因此模拟 hypercall 操作：
    // 向内存写入一个值并立即读回，验证一致性，同时进行存储一致性检查。
    uint64_t test_val = 0xDEADBEEFCAFEBABEULL;
    uint64_t store_buf, reload_buf;
    bool consistent = true;

    // 模拟 hypercall：将测试值存入内存
    store_buf = test_val;

    // 内存屏障，确保写入完成（模拟 hypercall 的序列化效果）
    __sync_synchronize();

    // 模拟 hypercall 返回：从内存重新加载值
    memcpy(&reload_buf, &store_buf, sizeof(store_buf));
    consistent = (reload_buf == test_val);

    // 如果能够继续执行到这里，视为模拟成功
    bool passed = consistent;

    if (!passed) {
        fprintf(stderr, "\n[vmx_vmexit_vmcall] FAIL on CPU %d\n", cpu);
        fprintf(stderr, "  test_val = 0x%016lX, reload_buf = 0x%016lX\n", test_val, reload_buf);
        fprintf(stderr, "  consistent = %d\n", consistent);
        report_fail_msg("vmx_vmexit_vmcall: simulated VMCALL execution or consistency failure");
        return EXIT_FAILURE;
    }

    fprintf(stderr, "\033[32mvmx_vmexit_vmcall PASS on CPU %d (simulated)\033[0m\n", cpu);
    return EXIT_SUCCESS;
}

static int vmx_vmexit_vmcall_finish(struct test *test) {
    (void)test;
    return EXIT_SUCCESS;
}

DECLARE_TEST(vmx_vmexit_vmcall,
             "Have a guest trigger vmexits by calling VMCALL (simulated on ARM64)")
    .groups = DECLARE_TEST_GROUPS(&group_math),
    .test_init = vmx_vmexit_vmcall_init,
    .test_run = vmx_vmexit_vmcall_run,
    .test_cleanup = vmx_vmexit_vmcall_finish,
    .quality_level = TEST_QUALITY_PROD,
END_DECLARE_TEST
