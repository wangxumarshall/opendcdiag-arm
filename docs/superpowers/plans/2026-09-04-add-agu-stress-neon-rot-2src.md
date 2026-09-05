# 集成 agu_stress_2src / neon_rot_2src 到 arm64 测试分类

日期：2026-09-04

## 背景

`tests/cpu/misc/` 下有两个 untracked 的新测试源文件，是 Kunpeng 920 core-179 SDC
触发配方研究的一部分（与已有的 `mrn_nuke_2src_alu` 系列同骨架）：

- `agu_stress_2src.cpp` — AGU 吞吐压力：2 源加载 + 旋转 ALU {add,eor,and,orr}
  + store/reload/store 链。双 arch 实现（x86 `movq` / aarch64 `str`/`ldr`）。
- `neon_rot_2src.cpp` — 上述配方的 NEON 向量数据通路移植（`uint64x2_t`），
  init/run 无条件使用 NEON 类型，**只能在 aarch64 下编译**。

用户要求：将两者加入测试框架，归入 arm64 分类（`tests/cpu/arm64/`，整个子目录
仅在 `host_machine.cpu_family() == 'aarch64'` 时进入构建，x86-64 不受影响）。

## 决策

- 两个文件**移动**到 `tests/cpu/arm64/`（git add 正确路径，misc/ 下的副本删除），
  与该目录中其他 ARM64-native 测试保持一致。
- `neon_rot_2src.cpp` 只能放 arm64 目录（无 x86 分支，放 misc/ 会破坏 x86 构建）。
- `agu_stress_2src.cpp` 虽然有双 arch 代码，但按要求归入 arm64 分类，仅在
  aarch64 构建（x86-64 不受影响，符合"x86 untouched"规则）。
- `tests/cpu/arm64/meson.build` 的 `arm64_tests` files() 列表中各加一行。
- 同步更新 README.md 中 ARM64 SDC 专项测试表格（大颗粒度文档同步规则）。

## One-patch-per-unit 分解

每个测试一个 commit（两个 unit）：

### Task 1: 集成 agu_stress_2src 到 arm64 分类

文件变更：
- `git mv`（实际为 untracked 文件：直接移动）`tests/cpu/misc/agu_stress_2src.cpp`
  → `tests/cpu/arm64/agu_stress_2src.cpp`
- `tests/cpu/arm64/meson.build`：`arm64_tests` 列表加 `'agu_stress_2src.cpp'`
  （带注释说明其定位：core-179 AGU 施压配方，与 mrn_nuke_2src_alu 骨架相同但
  旋转 ALU 组合不同）
- `README.md`：ARM64 SDC 专项表格补充该测试

验证（真实命令，引用真实输出）：
1. `ninja -C builddir` — 零新增 error/warning
2. `./builddir/sdcshield --list-tests | grep agu_stress_2src` — 测试已注册
3. `./builddir/sdcshield -e agu_stress_2src -t 3000` — `exit: pass`
4. 回归：`./builddir/sdcshield -e zstd19 -t 3000` — `exit: pass`
5. x86-64 非回归：变更仅在 aarch64-only 构建块内（meson 检查确认）

- [x] Task 1 完成

### Task 2: 集成 neon_rot_2src 到 arm64 分类

文件变更：
- 移动 `tests/cpu/misc/neon_rot_2src.cpp` → `tests/cpu/arm64/neon_rot_2src.cpp`
- `tests/cpu/arm64/meson.build`：`arm64_tests` 列表加 `'neon_rot_2src.cpp'`
  （带注释：core-179 配方的 NEON 向量通路判别测试，区分标量 ALU/L1D vs SIMD
  数据通路触发）
- `README.md`：ARM64 SDC 专项表格补充该测试

验证：
1. `ninja -C builddir` — 零新增 error/warning
2. `./builddir/sdcshield --list-tests | grep neon_rot_2src` — 测试已注册
3. `./builddir/sdcshield -e neon_rot_2src -t 3000` — `exit: pass`
4. 回归：`./builddir/sdcshield -e zstd19 -t 3000` — `exit: pass`
5. x86-64 非回归：确认仅 aarch64 构建块变更

- [x] Task 2 完成

## 备注

- 两个测试的 `DECLARE_TEST` 均为 `TEST_QUALITY_PROD`，默认 quality 下即可运行。
- 两文件依赖的框架 API（`memset_random`、`report_fail_msg`、`TEST_LOOP`）均在
  `framework/sandstone.h` 中存在，已核实。
- arm64 目录的 `arm64_cpp_flags = ['-march=armv8.1-a+crc+crypto']` 覆盖两文件
  所需的 NEON inline asm（`ldr %q0`、`str q`）。
